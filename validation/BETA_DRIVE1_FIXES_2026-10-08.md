# 첫 상시 BETA 주행에서 확인된 결함 수정 (2026-10-08)

소유자가 v1.0.0-beta.3(persistent)으로 처음 실제 주행한 기록에서 확인한 결함 네 가지를
고쳤습니다. beta.4에도 같은 결함이 있습니다. 주행 원본은 비공개이며 이 문서에는 요약 수치만
옮깁니다. 아래 수정은 호스트, exact-ARM(QEMU), 재생 하네스에서만 검증했고 **차량에서 실행된
적이 없습니다.** 이 기록은 차량 방문이나 주행을 요청하지 않습니다.

## 1. 스스로 만든 기록 지연 (RAW 창 일괄 기록)

**증거.** persistent 프로파일은 GPS 끊김과 복귀마다 약 400 KiB(약 1,100행) RAW 창을 한 번에
기록했습니다. writer는 초당 약 350-630행을 썼고 가장 오래된 미기록 행이 1.5 s 한도를 넘었습니다.
`beta_journal_lag` 행(mono s): 1879.9, 2123.6, 2221.8, 4212.7에서 사건 후 약 +1.5 s에
`lagging`(미기록 381-647행), +2.3-3.2 s에 `current`. 부팅 직후 163.4 s의 첫 창(1,152행)도
164.9 s와 169.4 s에 같은 형태였습니다. 즉 BETA가 개입해야 하는 바로 그 순간에 provenance가
거두어졌습니다.

**변경.**
- `PersistentLog`: 사건 때 `raw_window` 표지만 쓰고 창 행은 worker turn마다 `pump()`가 초당
  최대 150행(토큰 버킷, turn당 16행)으로 씁니다. journal에 즉시 써야 할 행이나 이전 pump의 창 행이
  아직 대기 중이면 쉬므로, writer가 더 느리면 그 속도를 따릅니다.
  배출 중 들어온 raw 행은 창 뒤에 붙어 순서가 유지되고, capture 종료는 나머지를 즉시 씁니다.
  창에서 나온 행은 kind 바로 뒤에 `"raw_window":true`가 붙고, 표지에는
  `"drain":"paced"|"immediate"`가 있습니다.
- `JournalRing`에 세 번째 BULK 등급을 추가했습니다(전역 순서 유지, 자기 링 안에서 오래된 것부터
  버림). journal 지연은 **즉시 기록해야 하는 행**(증거 행과 일반 주기 행)만 셉니다. BULK 행 뒤에
  선 즉시 행은 자기 push 시각부터 세어지므로 writer 정지는 그대로 드러납니다. BETA live이고
  journal current일 때의 창 POSITION 행은 증거 등급을 유지합니다. persistent 프로파일에만
  BULK 링 128 KiB를 더 할당합니다(full 프로파일은 그대로). 1.5 s 한도, 0.5 s 복구 수준과
  250 ms 자체 flush는 바꾸지 않았습니다.
- 분석기: 표시된 창 행은 과거 맥락입니다. journal gap을 닫지 않고, 치환 뒤 "첫 GPS fix"로 쓰이지
  않습니다. 예전 표지(drain 필드 없음)는 행 수 규칙을 그대로 쓰며, 그 행들도 이제 이 검사에서
  빠집니다(예전에는 GPS 복귀 때 창의 끊김 전 FIX 행이 측정에 쓰일 수 있었습니다).

**측정(`test_journal --writer` window_burst, 호스트 / exact-ARM QEMU).** 사건 시점에 약 370 B 행
1,100개, writer를 행마다 sleep으로 늦춤, 20 ms worker turn, 매초 LOST POSITION·치환 SEND·
beta_summary와 100 ms마다 raw batch.

| 경우 | 최대 지연 (호스트 / QEMU) | 최대 미기록 행 | guard 하강 |
|---|---|---|---|
| 창 전체를 사건 시점에 기록(beta.4), writer 초당 350행 | 3296-3577 / 11991 ms | 1106 | 1회 |
| 초당 150행 배출(BULK, 지연 미산정), writer 초당 350행 | 242-246 / 251 ms | 16-18 | 0회 |
| 같은 배출, writer 초당 90행 | 241 / 382 ms | 18-19 | 0회 |
| 같은 트래픽 + writer 1.6 s 정지 | — | — | 1회, 정지 시작 뒤 1514-1519 / 1566 ms |

QEMU에서는 writer가 sleep 외에도 느려 실제 속도가 더 낮습니다. 구현 중 두 가지를 더 고쳤습니다.
(1) 처음에는 즉시 행만 대기 여부로 보아, writer가 배출 속도보다 느린 QEMU에서 BULK 행이 쌓였습니다
(91행, 1930 ms, guard 하강). 이전 pump의 창 행이 모두 writer에 넘어간 뒤에만 다음 행을 내도록
고쳐 배출이 writer 속도를 따릅니다. (2) writer는 64행을 stdio에 넘긴 뒤에야 250 ms 자체 flush를
검사했는데, 느린 writer에서는 그 묶음만으로 1.5 s를 넘었습니다(QEMU 초당 90행 경우 1892 ms,
하강 3회). 이제 stdio의 가장 오래된 즉시 행이 flush 한도에 닿으면 묶음을 끝냅니다(writer 스레드만;
한도·flush·fsync 정책은 그대로).

기존 지연 한도 시험(2.5 s 정지에서 1.5 s 안에 철회, 1.6 s 정지 1325 ms 철회)은 바꾸지 않았고
통과합니다.

## 2. 앵커 게이트와 OEM utc 주기

**분석(beta_anchor 3,657행, mode 1/2 POSITION 전부).** hook이 보는 OEM POSITION 폴링 간격은
p50 1.003 s, p1 0.837 s, p99 1.160 s, 최대 1.759 s(재연결 두 번 제외), 94.5%가 0.9-1.1 s입니다.
utc는 1 Hz이지만 폴링과 위상이 달라 연속 폴링의 utc 차이가 1(3,587), 0(28, 같은 초 반복),
2(39)입니다. 반복 다음 폴링은 기준 fix(반복 전)와 두 폴링 간격(2.0 s ± 흔들림) 떨어져 있어
2.0 s 쌍 한도에서 약 절반이 `PREVIOUS`로 거부되고 정착 streak가 0으로 돌아갔습니다
(1852-1878 s: `UTC`와 `PREVIOUS` 교대).

**변경.** `fix_pair_max_ns` 2.0 s → 2.5 s. utc 엄격 증가, utc 단계 ≤ 2 s, utc 단계와 수신 단계
차이 ≤ 1.0 s, HDOP, 10 s 정착, 60 km/h 상한(근거 없이 올리지 않음), 이전 속도, course,
변위, yaw, wheel, reverse는 그대로입니다. 같은 초 반복은 예전처럼 기준을 유지하고 streak를
끊지 않습니다.

**효과(이 주행).** 기록된 행으로 쌍·정착 규칙을 다시 계산하는 스크립트는 기존 규칙에서 3,657행
전부를 그대로 재현했습니다(불일치 0). 새 규칙에서 `PREVIOUS` 20→3, `SETTLING` 118→57이고,
30행(413-419 s 7행, 784-795 s 11행, 1858-1876 s 12행)이 새로 뒤쪽 게이트(course/변위/yaw/wheel/
reverse)에 도달합니다. 그 게이트의 결과는 persistent 기록에 없으므로, 이 주행에서 뒤쪽 게이트에
도달한 행의 통과율(950/1308 = 72.6%)을 적용한 **추정 약 22개, 상한 30개**의 앵커가 더 생깁니다
(46행은 대신 속도 게이트에서 거부). 1879.9 s GPS 끊김 직전의 마지막 앵커는 50.2 s 전
(1829.7 s)이어서 이 주행의 BETA는 한 번도 ENGAGED가 되지 않았습니다(publications 0,
`core_rejected`/`model_not_ready`). 새 규칙에서는 끊김 3 s 전까지의 fix가 앵커 후보이지만,
실제 통과 여부는 기록으로 확인할 수 없습니다. 재생 하네스(RAW 창 행만 있는 이 주행 입력)에서도
1854-1877 s의 같은 12행이 쌍·정착을 통과했습니다(재생 입력에 reverse 메시지가 없어 `REVERSE`에서
멈춤). collector 폴링을 쓰는 다른 주행(trip2)의 재생 결과는 바뀌지 않았습니다(ACCEPTED 93).

## 3. 연결 주기 fence

**증거.** 2088.5 s와 2106.8 s 뒤 POSITION 폴링이 각각 18.3 s, 16.8 s 없었고(동글 재연결),
같은 기간 log_digest의 SEND 수도 0이었지만 storage 포인터 fence의 `session_epoch`는 1로
남았습니다. 그 밖의 최대 폴링 간격은 1.76 s입니다.

**변경.** `BetaController`가 POSITION과 SEND 각각의 최신 hook 시각을 추적합니다. 3 s를 넘는
공백(진행 중 발견하거나 공백 뒤 첫 관측에서 발견)에서 `beta_cadence_fence` 행(beta_* 증거
등급)을 쓰고, adapter generation을 무효화하고, ENGAGED/SPEED_ENGAGED면 `cadence_gap`으로
WITHDRAWN(속도 overlay는 storage와 같은 5 s 정착 뒤 `rearm_after_cadence_gap`), worker가
`Pipeline::fence_beta()`로 BETA core를 미시드 상태로 다시 만들고 쌍 기준·streak·yaw 창을
지웁니다. 다시 쓰려면 새 generation의 POSITION과 10 s 정착 뒤의 새 앵커가 필요합니다. 재생
하네스와 log_rate도 같은 순서로 호출합니다. 이 주행 입력의 재생에서 2091.5 s와 2109.8 s에
fence가 났습니다(그 밖의 재생 fence는 persistent 기록에 POSITION/SEND가 빠진 구간의 재생
인공물입니다).

## 4. 메뉴 6 확인과 재검토 LOW 항목

메뉴 6은 "수집 로그도 지워진다, 먼저 3을 하라"는 안내 뒤 정확히 `6`을 한 번 더 입력해야
실행합니다. 다른 입력, 빈 줄, EOF는 아무것도 지우지 않고 취소합니다. 잠금 디렉터리에 다른
파일이 있어 마지막 `rmdir`가 실패해도 결과가 출력되고(`after_install_lock=present`,
`status=incomplete`, `Install lock STILL PRESENT`), 오래된 잠금 검사 glob에 `..?*`를
추가했습니다(이 경로는 fixture root가 없을 때만 실행되어 복제 루트 시나리오에서만 다룹니다).

## 실행한 시험 (이 브랜치)

- 호스트 전체 `make -k test`(직렬): 종료 0, Python 778개 통과, 생략 4개(packaging의 릴리즈 묶음
  미빌드: `MX5DR_RELEASE_BUNDLE`/여섯 산출물 묶음, 이 작업은 ZIP을 만들지 않음). `-j4`로 돌린 첫
  실행에서는 `test_collector`의 2 s 제한 안에서 `build/test_journal`이 부하로 시간 초과됐고,
  `test-collector` 단독(11개 통과)과 직렬 전체 실행에서는 통과했습니다.
- 새/바뀐 시험: test_beta(OEM 주기 회귀: 2.5 s 규칙 앵커 12·PREVIOUS 0, 2.0 s 규칙 앵커 0·
  PREVIOUS 9, utc 정지, 0.98 s 2초 점프), test_worker_beta cadence_gap·cadence_gap_speed(그 밖의
  9개 시나리오는 fence 행 0을 단언; fence 호출을 뺀 빌드에서 cadence_gap이 실패함을 확인),
  test_journal_ring(BULK), test_log_profile(paced drain), test_journal --writer window_burst,
  test_analyze_beta(fence 행, 표시된 창 행), test_purge·test_trial_menu(두 번째 6, 잠금 결과).
- exact-ARM(고정 도구체인, `Verified ARM build`, `release_verified=true`): `tests/run_arm_all.sh`
  종료 0(PASS 줄 227, SKIP 2: 원래 LDS cold installer는 비공개 `MX5DR_LDS_STOCK` 미설정, 내장 AA
  probe는 아래에서 별도 실행), `tests/adapter/run_arm.sh` 종료 0(PASS 109),
  `run_aa_install_probe.sh`(실제 BLM, libpatch 0.9.1) PASS 4. `mx5dr-guard` 해시는 beta.3/beta.4와
  같습니다(`248a3ef5…`). QEMU 결과는 차량 검증이 아닙니다.
- 순정 BusyBox 1.19.2 복제 루트(`tests/packaging/replica_clean_uninstall.py`, beta.4 묶음의 ARM
  산출물에 이 브랜치의 packaging 스크립트): A 64/64, D 73/73 통과. 새 검사는 오래된 잠금의
  `..stale` 항목 거부(종료 6, 아무것도 바뀌지 않음)와 두 번째 입력 `7`의 취소입니다.
- 재생: replay_beta를 이 주행(RAW 창 행만 있는 입력)과 trip2(collector 폴링)에서 수정 전후로
  돌렸습니다(출력은 비공개 작업 디렉터리에만).

## 남은 위험

- 모든 수치는 호스트 측정과 기록 재계산입니다. CMU eMMC의 실제 writer 속도와 배출 중 지연,
  fence의 실제 재연결 동작, 새 앵커의 정확도는 차량에서 검증되지 않았습니다.
- 배출 중 사건이 겹치면 창의 끝부분은 사건 뒤 최대 약 7 s 늦게 기록되고, 그 사이 CMU가 꺼지면
  남은 창 행은 잃습니다(예전에는 링에 들어간 뒤 잃음). capture 종료는 즉시 기록합니다.
- 0.97 s 만에 utc가 2 s 뛰는 쌍(이 주행 9행)은 여전히 `UTC_MONO`로 streak를 끊습니다. 허용
  오차를 1.1 s로 넓히면 15행이 더 뒤쪽 게이트에 도달하지만, 근거 없이 바꾸지 않았습니다.
- 3 s fence는 AA가 연결된 상태의 폴링 주기를 전제로 합니다. 연결 해제(주차) 때마다 한 번
  fence 행이 생깁니다.
