# v1.0.0-beta.14 후보: 기록 전용 주행 데이터 행 (2026-10-13)

대상: beta.13(`6a88059`)과 `src/`·`packaging/`이 같은 origin/master 위의 변경입니다. **호스트 시험과 비공개 주행 기록의 오프라인 재생만 수행했고 차량·폰·DHU·ARM/QEMU에서는 실행하지 않았습니다.** 이 기록은 릴리즈 승인이나 차량 방문 요청이 아닙니다. 두 번째 판(v2)은 독립 검토 두 건(Opus, Fable)의 지적을 반영했습니다.

## 문제

2026-10-13 오프라인 연구(`EXIT_JUMP_BLEND_2026-10-13.md`와 `_FABLE_`, `YAW_DEADBAND_2026-10-13.md`와 `_FABLE_`, `GPS_RETURN_QUALITY_2026-10-13.md`, `PHONE_IMU_FEASIBILITY_*`)는 모두 '데이터 필요'로 끝났습니다. 영구 저널은 100 ms 요·바퀴와 어댑터 위치 행을 raw window 안(손실 전 54 s, 복귀 후 29 s)에만 남기고, digest의 GPS 침로는 1도 정수이며, 터널 출구가 네이버 점프 규칙을 만족했는지는 어디에도 기록되지 않습니다. 차량 주행은 드물기 때문에 다음 주행이 이 자료를 모으도록 **새 행 종류만** 추가합니다. 제품이 보내거나 결정하는 것은 바꾸지 않습니다.

## 추가한 행 (`src/runtime/drive_record_log.h`, persistent 프로파일에서만)

- **`fix1`**: 주행 중(픽스 속도 또는 바퀴 평균 > 5 km/h) 새 순정 픽스(디코드된 mode 1/2/3, utc > 0, 같은 픽스의 재폴링 제외)마다 한 행. `"d"` 배열 13개 정수: 수신 시각 ms, mode, 위도·경도 e7, 속도 mm/s, 방위 e6(1도 정수가 아님, 360°로 반올림되면 0), 같은 호출 LOCATION의 정확도 mm(없으면 -1), 같은 호출 GPS_SATELLITE의 사용 위성 수(진단 빌드에서만 관측, 아니면 -1), 아래 합계의 구간 ms, 요 합계(샘플당 카운트−2048의 합)와 샘플 수, 바퀴 평균 km/h×10과 이벤트 수. 요·바퀴 합계는 직전 순정 픽스 이후 수신분입니다.
- **`exit_jump`**: GPS 상실 에피소드 안에서 LOCATION이 한 번이라도 나갔다면, 그 뒤 첫 순정 픽스(FIX 또는 NATIVE_DR)에서 한 행. 기준점은 **그 픽스 전에 마지막으로 나간 LOCATION(종류 무관)**입니다. BETA가 철회한 뒤에는 순정 LOCATION이 진입 때 좌표로 계속 나가므로 폰이 마지막으로 받은 것은 그 값이고, R1/R2도 그것과 비교합니다(111 s 터널처럼 30 s 뒤 철회된 경우). 필드: 에피소드 번호, 픽스 POSITION 후킹 시각 `fix_ms`와 그 호출 LOCATION의 후킹 시각 `fix_send_ms`(짝이 없으면 null), 상실 시작부터 ms, 대체 횟수(`reps`)·기간, 에피소드 안 순정 LOCATION 수(`stock`)와 마지막 대체 뒤 순정 LOCATION 수(`stock_after`), 마지막으로 나간 LOCATION의 choice(`sent_choice`, 3 = BETA 대체, 0 = 원본)와 시각(`sent_ms`), `dt_ms`(= `fix_ms` − `sent_ms`), 점프 거리와 보낸 방위 기준 종/횡 성분(횡은 오른쪽 +), 보낸 방위·픽스 방위·차이, 양쪽 속도와 정확도(`sent_acc`는 나간 바이트의 정확도로 **권위 있는 보고 정확도**), `honest`, 그리고 0절의 R1(직전 보고 정확도 0 이상 30 m 미만, 500 ≤ dt ≤ 1500 ms, 거리 ≥ 50 m, 거리 > 2×max(속도 m/s))과 R2(dt ≤ 3 s에서 환산 속도 > 200 km/h)의 판정과 각 입력 판정. 같은 호출 LOCATION을 기다리고 1.5 s 안에 없으면 POSITION 값으로 씁니다.
  - `dt_ms`는 후킹 수신 시각의 차이입니다. 네이버 `getTime()`의 의미는 확인하지 못했으므로 분석기는 `fix_send_ms`로 두 번째 정의(`dt_send_ms`, `r1_dt_send`)도 계산합니다.
  - `honest`는 마지막 대체 송신을 worker가 읽을 때의 최신 발행 정직 예산입니다. 그 송신이 쓴 값보다 한 틱 새것일 수 있습니다. 분석기는 `sent_acc`를 보고 정확도로 씁니다.
- **`yaw_learn`**: `YAW_DEADBAND_FABLE_2026-10-13.md`의 G W=40 S=10 r1 ≥ 10 km/h 추정. 새 FIX마다 간격 ≤ 2.5 s로 이어진 ≥ 10 km/h 픽스를 40 s 이상 거슬러 올라가 현재 침로와의 편차가 10° 이하이고, 그 구간 안 100 ms 요 구간이 90 % 이상 덮이면 영점 = 평균 카운트 − (침로 변화/구간)/모델 부호 있는 계수(−0.000658615 rad/카운트)입니다. |영점−2048| > 10이면 버리고 직전 추정을 유지합니다. 침로가 유효하지 않은 픽스는 사슬을 끊지 않고 비교에서만 빠집니다. 행은 GPS 상실 시작마다(`ev`=lost, 추정 없으면 null)와 유효 추정이 있을 때 60 s마다(periodic). **DR에는 넣지 않습니다.** 정차 요 평균은 기존 `yaw_stop`이 이미 기록합니다.
- **`rec_drop`**: 게이트가 거부한 행 수(종류별)와 이유(`why`: journal, no_writer, lag, queue), 속도 제한이 막은 행 수(`fix1_rate`, `exit_rate`, `lost_rate`). 기록되면 60 s 대기, 거부되면 1 s 뒤 재시도(자기 거부도 셈).

## 지연·버림 규칙

행은 기존 writer 스레드로만 갑니다(fsync 없음). 각 행 직전에 `Journal::record_gate`가 이유를 돌려줍니다. 저널 실패나 `journal_current` 0이면 journal, **writer 스레드가 없으면(시작 실패, 동기 저널) no_writer로 worker에서 쓰지 않습니다**, 진단+증거 링이 64 KiB를 넘으면 queue, prompt 행 지연이 500 ms 이상이면 lag. 거부된 행은 큐에 넣지 않고 버리고 셉니다. writer가 없을 때는 `rec_drop`도 쓸 수 없으므로 그 부팅에는 새 행이 하나도 없습니다. 행은 진단 등급(`ROW_RAW`)이며 RAW window에 들어가지 않습니다.

## 예산 (DATA, 상한 포함)

- 기존 영구 저널(비공개 trace 파일의 부팅별 바이트/시간): 부팅 시간당 3.3–8.9 MB, 주행 시간(digest 평균 > 5 km/h)당 4.7–12.8 MB.
- 속도 제한(토큰 버킷): `fix1` 1 s당 1(버스트 8), `exit_jump`와 상실 시작 `yaw_learn` 각 60 s당 1(버스트 3), 주기 `yaw_learn` 60 s당 1, `rec_drop` 60 s당 1.
- **최악의 경우**: 행 최대 길이(`fix1` 130 B, `exit_jump` 640 B, `yaw_learn`·`rec_drop` 360 B, 단위 시험에서 극단값으로 확인)와 위 제한으로 576 KB/h(`worst_case_bytes_per_hour()`, 600 KB/h 미만). 1시간 동안 1 s마다 LOST/FIX가 바뀌고 매 호출 LOCATION이 나가는 합성 시험: 222 KB(`fix1` 1800, `exit_jump` 62, `yaw_learn` 62, `rec_drop` 61행; 제한된 `exit_jump` 1737, 상실 행 1738).
- 합성 주행(`build/log_rate`, 1 h, 10분마다 터널): 1977.9 → 2064.4 B/s(+86.5 B/s = 311 KB/h), stop-and-go +56.0 B/s.

## 불변식과 증명 (무엇을 덮고 무엇을 덮지 않는가)

- **runtime 경로의 바이트 동일성**: `test_log_profile.py DriveRows`가 `build/log_rate`(runtime.cpp의 Journal·PersistentLog·BetaController·실제 어댑터를 시뮬레이션 시계로 돌림)를 행 켬/끔으로 1시간 두 번 돌립니다. 대체가 있는 GPS 상실과 출구가 6번 있고(`exit_jump` 6행, 모두 `sent_choice` 3), 새 종류를 빼면 trace 파일의 행이 **바이트 단위로 같습니다**(boot 행의 pid만 정규화). 덮지 않는 것: writer 스레드와 실제 시간(이 하네스는 동기 저널이며 no_writer 이유만 하네스 게이트가 통과시킵니다), 실제 worker 스레드의 턴 순서.
- **PersistentLog 단위**: `test_drive_rows`에서 새 종류를 빼면 행·순서·등급이 같고, 게이트를 닫으면 완전히 같습니다. 새 행은 같은 입력의 기존 행 **뒤에** 나옵니다(출구 픽스의 SEND 행 바로 다음).
- **writer 게이트**: `test_journal --writer`의 `writer_record_gate`. 지연 > 500 ms에서 `fix1` 3행 거부(이유 lag, 큐 0행), 같은 호출의 POSITION/SEND 6행은 전처럼 큐에 들어감, 회복 뒤 `rec_drop` 1행. 지연 < 500 ms이지만 링이 64 KiB를 넘으면 queue로 거부. writer가 없으면 no_writer로 새 행 0, 관측 행 6.
- **비공개 재생(별도 출력)**: `replay_beta`는 제품 BETA 경로를 돌리고 새 행은 별도 `DriveRecordLog`와 별도 파일로 씁니다. 그래서 이 비교는 **새 코드가 대체 LOCATION·BETA 상태·G3 행을 바꾸지 않는다**는 것만 보이고, 저널 안에서의 상호작용은 보이지 않습니다(위 runtime 시험이 덮음). 결과는 아래 '재생' 절.
- 손으로 한 변이 시험: v1의 5건(R1 정확도 경계, 이동 게이트, 영점 변화율 보정, 프로파일 게이트 무시, runtime 게이트 항상 열림)과 v2의 5건(링 64 KiB 조건 삭제, ±10 카운트 거부 삭제, 새 행을 기존 행 앞으로, 기준점을 대체 송신으로만, writer 없음 게이트 열림) 모두 해당 시험이 실패했습니다.

## 재생 (DATA, 오프라인)

비공개 주행 17개 부팅(07987d33, 843d1bbf, 65258c5f, 82d37f8a, 322800ef, 094c62b5, e5584f62, 1c1de788, 0914e18f, e792f96d, 5d6260d5, 62bb2476, 7ce61caf, 5a9f9b2b, 41945267, ef2c2e67, 7cb3b7dc; `--real-only --g3 on`): beta.13 빌드, v2 빌드, v2 + `--record-out`의 report/CSV/BETA journal/G3/stdout/stderr 102개 파일이 바이트 일치했습니다. 새 행 값: 5a9f9b2b의 60 s 터널(908.2 s) 출구는 점프 50.3 m, R1 참. 111 s 터널(758.8 s)은 재생에서 30회 대체 뒤 철회되고, 이제 기준점이 그 뒤의 순정 LOCATION입니다(아래 표의 `stock_after`).

| 부팅 | 픽스 s | 상실 s | reps | stock_after | sent_choice | dt ms | 점프 m | sent_acc | R1 | R2 |
|---|---|---|---|---|---|---|---|---|---|---|
| 322800ef | 1558.6 | 14 | 13 | 0 | 3 | 1015 | 19.0 | 25.0 | 거짓 | 거짓 |
| 41945267 | 2649.6 | 53 | 52 | 0 | 3 | 973 | 35.1 | 25.0 | 거짓 | 거짓 |
| 5a9f9b2b | 135.3 | 3 | 2 | 0 | 3 | 1090 | 4.3 | 25.0 | 거짓 | 거짓 |
| 5a9f9b2b | 758.8 | 111 | 30 | 80 | 0 | 962 | 1181.6 | null | 거짓 | 참 |
| 5a9f9b2b | 908.2 | 60 | 59 | 0 | 3 | 996 | 50.3 | 25.0 | 참 | 거짓 |
| 5a9f9b2b | 943.4 | 2 | 1 | 0 | 3 | 1020 | 38.1 | 25.0 | 거짓 | 거짓 |
| 5a9f9b2b | 2398.0 | 14 | 13 | 0 | 3 | 983 | 30.8 | 25.0 | 거짓 | 거짓 |
| 5d6260d5 | 2398.3 | 14 | 0 | 14 | 0 | 1023 | 233.5 | null | 거짓 | 참 |
| 62bb2476 | 2398.3 | 14 | 0 | 14 | 0 | 1023 | 233.5 | null | 거짓 | 참 |
| 65258c5f | 2123.6 | 244 | 29 | 26 | 0 | 16778 | 1923.2 | null | 거짓 | 거짓 |
| 65258c5f | 2221.8 | 93 | 29 | 59 | 0 | 998 | 1624.7 | null | 거짓 | 참 |
| 7ce61caf | 2398.3 | 14 | 0 | 14 | 0 | 1023 | 233.5 | null | 거짓 | 참 |
| 82d37f8a | 879.8 | 17 | 16 | 0 | 3 | 1004 | 17.3 | 25.0 | 거짓 | 거짓 |
| 82d37f8a | 2024.8 | 4 | 3 | 0 | 3 | 1006 | 7.8 | 25.0 | 거짓 | 거짓 |
| e792f96d | 2398.3 | 14 | 0 | 14 | 0 | 1023 | 233.5 | null | 거짓 | 참 |

111 s 터널(5a9f9b2b 758.8 s)은 대체 30회 뒤 순정 LOCATION 80회가 진입 좌표로 나갔고, 출구 점프는 1181.6 m, R2 참입니다(재생 값이며 차량에서 네이버가 무엇을 했는지는 이 기록이 보이지 않음). 짧은 부팅 4개(5d6260d5, 62bb2476, 7ce61caf, e792f96d)는 같은 collector 위치 기록을 재생해 대체가 없는(reps 0) 순정 출구 행을 냅니다. 65258c5f 2123.6 s는 마지막 LOCATION이 출구 16.8 s 전이었습니다(원인 미확인).

## 변경 파일

`src/runtime/drive_record_log.h`(새), `log_profile.h`(연결, 게이트), `runtime.cpp`(게이트 이유, 정직 예산 전달), `beta_controller.h`(읽기 전용 접근자 `last_honest_accuracy()`), `tools/replay_beta.cpp`(`--record-out`), `tools/analyze_logs.py`(`drive_data` 절, 새 행이 있을 때만), 시험: `tests/runtime/test_drive_rows.cpp`(새), `test_journal.cpp`, `test_yaw_rows.cpp`, `log_rate.cpp`(`--record-rows`, 하네스 게이트), `tests/journal/test_log_profile.py`, `tests/replay/test_replay_drive_rows.py`(새), `tests/tools/test_analyze_drive.py`(새), `Makefile`. 운전자 절차는 바뀌지 않아 `docs/FIELD_PROCEDURE_BETA_KO.md`는 고치지 않았습니다.

## 한계와 기록만 하는 사항

- 차량 미검증. ARM은 `runtime.o` 컴파일만 확인했고 ARM/QEMU/BusyBox 파이프라인은 실행하지 않았습니다.
- 위성 수는 진단 빌드에서만 관측됩니다(비진단 빌드는 OEM 스레드 어댑터 변경이 필요해 하지 않음).
- 새 행도 `Journal::emit`에서 writer 상태를 확인하므로 writer 실패가 최대 약 1 s 일찍 감지될 수 있습니다(fail-closed 방향).
- NATIVE_DR(mode 3)에서 utc가 0이면 그 호출은 출구로 보지 않아 `exit_jump`가 다음 FIX로 미뤄집니다. 장치가 그런 값을 보내는지는 확인하지 못했습니다.
- 진단 빌드의 DIAG_SAT 군 중첩 문제는 여기에 해당하지 않습니다(새 행은 type 21을 관측만 함).
- `yaw_learn`은 수신 시각 100 ms 빈을 써서 커브 진입 직후 영점을 1 카운트가량 틀릴 수 있습니다(합성 재생에서 관측).
- writer 통계(`health`의 `unwritten_rows`, `high_water`)에는 새 행도 포함됩니다. 48 MiB 저널 고리가 담는 시간은 약 3–7 % 줄어듭니다.
- **D(`shadow_outage`) 생략**: worker에 Pipeline 인스턴스를 하나 더 두고 가상 mode-0 호출을 넣어야 해 작고 격리된 변경이 아니며, ARM CPU 비용을 측정하지 않았습니다. `fix1`의 1 s 요·바퀴 합계로 오프라인 1 Hz 적분 근사가 가능합니다.

## 정정: 5a9f9b2b 111초 터널의 재생 점프 값 (2026-10-13)
이 기록의 재생 표에서 111초 터널의 점프가 1181.6 m(대체 30회 뒤 순정 LOCATION 80회, R2 참)로 나오는 것은 **재생 인공물로 봅니다.** 해당 터널의 영속 기록에는 raw 창 안에 21초의 모션 행 공백(677.7~698.8 s)이 있어, 재생이 678.0 s에 `sensor_silence`로 BETA를 철회합니다(이전 두 연구 기록이 같은 사실을 적었습니다). 실제 차량 기록에서는 DR이 터널 끝까지 이어졌고 **출구 점프는 120.6 m**(진행 +22.5 m, 횡 +118.5 m)입니다. 이 터널의 점프 판정은 120.6 m를 기준으로 해야 합니다(R1과 R2 모두 해당). 60초 터널(점프 50.3 m, R1 참)과 짧은 터널의 값은 재생과 차량 기록이 일치합니다. `exit_jump` 행 자체의 철회 뒤 출구 처리(마지막으로 나간 위치 송신 기준)는 합성 시험으로 검증되었고, 이 인공물은 행 구현의 오류가 아니라 기록 공백의 결과입니다.

## 파이프라인 결과 (커밋 76d702e, ZIP sha256 6a9df577fe20373916c908e74cc4af1ecfaf2885dd50450723ac883af215a431)
ARM 전체 PASS 332, FAIL 0, `ARM_TIMING_RETRIES=0`, 실제 BLM+libpatch probe(0.10.0, 0.9.1) 통과. 호스트 `make -k test` 종료 0, PASS 310줄. 패키징 380건 통과. 순정 BusyBox: 다중 부팅 71/71, 터치 A 76/76 · B 77/77, 삭제 복제 64/64 · 73/73, 업그레이드 통과. `libmx5dr.so` `1dbe9a52…`. 초안 릴리즈로 업로드했으며 공개 여부는 소유자 결정입니다.
