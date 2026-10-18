# G3 지하주차장 출구 방위 수정 3차 독립 적대 검토 (Fable, 2026-10-18)

대상: `2a154ee`("fix(g3): garage-exit carry third round", 12개 파일). 앞 판은 `c1f00f4`, `c75053c`; 앞 검토는 `validation/G3_GARAGE_EXIT_FIX_REVIEW_FABLE_2026-10-18.md`, `validation/G3_GARAGE_EXIT_FIX_REVIEW2_FABLE_2026-10-18.md`; 기록은 `validation/G3_GARAGE_EXIT_FIX_2026-10-17.md`("세 번째 수정" 절)와 AGENTS.md의 garage 예외 문단. 소유자 저장소는 읽기만 했고, 실행은 스크래치 클론(`git clone` + `checkout 2a154ee`, `(scratch)`)에서 했습니다. 차량·폰·DHU에서는 아무것도 실행하지 않았습니다. 표시: **CONFIRMED**(코드·실행으로 확인), **PLAUSIBLE**(조건이 맞으면 실제 영향), **찾지 못함**.

## 요약

- 제품 동작 변경(C1 예외: 디스크가 방위 있는 S이면 교체 중 30 s 간격을 적용하지 않음)에서 **결함을 찾지 못했습니다**. 조건은 맞는 상태를 읽고(`disk_`는 rename 성공 뒤와 시동 시 판독에서만 바뀜), 교체 중 fsync는 생기지 않으며(코드와 탐침 C), 송신 바이트(40, 44..47)에 닿는 파일은 이번 커밋에 없습니다(재생 바이트 동일성도 확인).
- **(C1, CONFIRMED, 문구·한도)** "출발 표시가 못 들어간 출발당 추가 쓰기 최대 1회"(`g3_heading.h:149, 1302-1303`, AGENTS 문안, 커밋 본문)는 실제 한도가 아닙니다. 실제 한도는 모든 요청에 공통인 5 s 간격(`G3_WRITE_MIN_NS`, `g3_heading.h:1291`)입니다. (a) 예외로 낸 S 요청이 게이트에서 취소되거나 우편함이 거부하면(`store_result`/`post_failed`가 `episode_done_`을 되돌림, 953-957, 978-983행) 디스크가 여전히 방위 있는 S이므로 **5 s마다 다시 예외로 요청**합니다(탐침 A: 23~58 s에 S 요청 8회, 각각 임시 파일 쓰기+unlink). (b) 표시 문턱(0.5 m / 2°) 아래이고 S "turned" 문턱(1°) 위인 기어감(예: 0.3 m, 1.5°)은 출발 표시가 아예 요청되지 않는데도 다음 5 s 정차에서 예외 S를 씁니다(탐침 B: S1 뒤 7 s에 S2). 둘 다 안전 쪽(새 S는 올바른 정차 기록, fsync 없음)이고 5 s 간격으로 묶이지만, 문구를 "요청당 5 s 간격, 표시가 계속 못 들어가는 정체에서는 정차마다 S 하나, 취소된 요청은 5 s 뒤 재시도"로 고쳐야 합니다.
- **(C2, CONFIRMED, 기록)** 변이 M4(규칙 F 가정 조건이 `g3_.reverse_unsure`를 읽음)는 **새 시험 없이도 이미 `test_beta`가 잡습니다**: `reverse_latch_cleared_in_reverse_blocks_rule_f`(`test_beta.cpp:2040`, 유효 래치 지움이 계수를 올리므로 value 0에서도 가정이 막힘). 2차 검토의 M4 목록(test_g3·test_navigation·test_model_session_input·재생)에 test_beta가 없었습니다. 새 시험 `reverse_unsure_is_observation_only`도 **단독으로** M4를 잡습니다(2600 ms에서 `af 1 1` 대 `af 0 0`, `gate 6` 대 `9`). 결함이 아니라 기록 정정 사항입니다("M4 모두 통과"라는 2차 검토·기록 문장은 test_beta를 빼고 한 말).
- **(C3, CONFIRMED, 문구)** AGENTS 문안 "0.0023 is the RMS of the 93 parked yaw zeros": 커밋 본문의 93개 RMS 3.396 카운트 × 0.000658615 = 0.00224 rad/s이고, 0.0023은 사전 등록 규칙이 올림한 값입니다(헤더 주석 `g3_heading.h:142`는 "0.00224"라고 맞게 적음). "RMS는 0.00224, 올림 0.0023"으로 고쳐야 합니다. 93·111이라는 개수 자체는 비공개 자료라 여기서 재검산하지 않았습니다(2차 검토 C2 참조).
- PLAUSIBLE: 호스트 재시도 래퍼가 간헐적인 **진짜** 저널 지연을 가릴 수 있음(P1), 분석기 계수의 과소 집계·견고성(P2·P3), AGENTS 문안의 작은 누락(P4), ARM 스위트는 새 래퍼를 쓰지 않고 모든 실패를 3회 시도함(P5).

판정: 제품 코드를 고쳐야 할 것은 없습니다. 차량 설치 전에 고칠 것은 문구 셋(C1 한도, C3 RMS, 기록의 M4 문장)이고, 릴리즈 파이프라인에는 호스트 로그의 `WORKER_BETA_TIMING_RETRY` 줄을 ARM 장부처럼 세어 두기를 권합니다(P1).

## 실행 (스크래치 사본, FACT)

1분 부하 1.3~3.9. 빌드는 `nice 10`. ARM 스위트와 전체 `make test`는 돌리지 않았습니다(릴리즈 파이프라인과 동시 실행 금지 조건).

| 항목 | 결과 |
|---|---|
| `build/test_g3` | 1,344 검사 통과 |
| `build/test_beta` | 77,768 검사 통과 |
| `tests/tools/test_analyze_g3.py` (Makefile 방식 discover) | 11개 중 10 통과, 1 건너뜀(고정물 환경 변수 없음) |
| `tests/replay/test_replay_g3_garage.py` (on/off/gap/lag) | 6개 통과 |
| 나머지 재생 시험(`test_replay_g3`, `_beta`, `_drive_rows`, `_reverse_fence`, `_frame_restart`) | 25개 통과, 2 건너뜀(비공개 주행 없음) |
| 재생 바이트 동일성 | `c75053c`의 `replay_beta`를 따로 빌드해 같은 합성 2부팅 사슬을 on/off/gap으로 두 바이너리에 돌림. `start_ns/end_ns/lat_ms`(저장 스레드 실시계)만 정규화: 모드마다 **16/16 파일 동일**(report·csv·journal·g3·sends·stdout·store·token, 부팅 A·B) |
| 변이 M4 (`pipeline.cpp:1417`에 `g3_.reverse_unsure||` 추가, 별도 worktree) | `test_beta` 전체: **2040행에서 잡힘**(기존 시험). 새 시험만 실행(main에 환경 변수 분기 추가): **2174행에서 잡힘**(위 C2) |
| 래퍼 가짜 명령 행렬 (`run_worker_beta_timing.sh`) | `exit 0`→0 즉시; `exit 1`→1 즉시; SIGABRT→134 즉시; 77·77·77→1(3회, 10 s, 부하 줄 3개 기록); 77·0→0(재시도 1회 기록); 77·134→134. `dash -n` 통과, `shellcheck -s sh` 경고 없음 |
| 래퍼 실제 실행 (`g3_garage`, `g3_cancel`, 호스트, 부하 1.5~3.6) | 둘 다 exit 0, 재시도 0회(총 160 s). g3_garage: fsync 쓰기 1(lat 3005.6 ms, 훅), 표시 M 1; g3_cancel: rename 게이트 취소 1, max_summary_gap 1021 ms, lagging 0 |
| 탐침 A~D (test_g3 하네스 복사본 `test_g3_probe.cpp`, 제품 시험 아님, 37 검사) | 아래 "(1) 저장 예외" |
| 분석기 합성 행 | 아래 "(5) 분석기" |

## (1) C1 예외 `disk_=='S' && (disk_flags_&G3_FLAG_HEADING)` — `g3_heading.h:1306-1310`

### 상태가 맞는가 (찾지 못함)

- `disk_`·`disk_flags_`는 세 곳에서만 바뀝니다: 시동 시 판독 성공(`768-769행`, 상태가 S가 아니면 flags 0), 표시(M) 발행(`965행`: flags=4, 방위 비트 없음 → 예외 꺼짐), S 발행(`968-970행`). 발행은 `r.stage==G3_IO_OK`일 때만이고 `g3_write_atomic`은 rename 실패를 stage로 보고하므로(`417행`) **rename 성공 뒤에만** 바뀝니다. 취소·실패·늦은 수거·우편함 거부(`post_failed`)는 바꾸지 않습니다. 표식(`g3-garage-off`)이면 `garage_`가 거짓이라 조건이 평가되지 않습니다.
- 복원한 S(이전 부팅)도 `disk_`에 들어가므로(`768행`), 거부된 복원(not_prev_boot, unc, chain_broken)도 예외 대상입니다. 코드가 일관됩니다: 그 S에 대해 `depart()`도 표시를 시도하고(`1348행`), 못 하면 다음 정차의 S가 덮습니다. 시동 시 판독 실패(unreadable/corrupt)면 `disk_=0`이라 예외도 표시도 없지만, 그 S는 다음 부팅에서 직전 부팅 토큰에 걸립니다(not_prev_boot). 1차부터 있던 잔여이고 토큰이 덮습니다.
- 같은 정차의 재기록 루프: 발행된 S는 `episode_done_`을 참으로 두므로(`1339행`) 같은 정차에서 다시 want가 되는 길은 60 s fsync 재기록(`sync_ok` 필요, 교체 중 거짓, `1270·1287행`)과 규칙 F 오염 재기록(따로 예외)뿐입니다. 즉 **발행된 S는 루프를 만들지 않습니다**. 루프가 되는 것은 발행되지 않은 요청(취소·실패·거부)의 재시도이고 5 s 간격입니다(위 C1(a), 탐침 A). I/O 실패는 3회 뒤 `store_on_` 끔(`983행`), 취소는 무제한입니다. 취소 사유는 정차·교체·garage에서 유휴 규칙의 게이트 재검사(`journal_lagging/no_writer/journal_draining/writer_busy`), `wheels_stale`, `rule_f_discard`뿐이라(`1378-1383행`) 요청과 게이트 사이(≤ 100 ms 폴링 + 쓰기)에 상태가 뒤집혀야 합니다. 실제 빈도는 낮다고 봅니다(INFERENCE).
- 쓰기 횟수 상한: 어떤 요청이든 `last_request_ns_` 기준 5 s(`1291·1353행`) → 분당 12, 시간당 720이 이론 상한이며 2차와 같습니다(2차에서도 표시는 간격이 없었음). 바뀐 것은 현실적 횟수입니다: 표시가 계속 못 들어가는 정체(쓰기 지연 100~300 ms 대역, 아래)에서는 **정차마다 S 하나**(2차: 30 s에 하나). 110 B 안팎의 temp+rename, fsync 없음.
- 유휴 규칙·100 ms 규칙: 예외 S는 유휴 규칙(`io_block`, writer_busy 300 ms)에는 따르고(`1294행`, 예외 검사보다 앞) 표시의 100 ms 규칙에는 따르지 않습니다. 그래서 쓰기 지연 100~300 ms 대역에서는 표시는 기다리고 S는 들어갑니다. 이것이 C1을 닫는 조건이므로(지연 150 ms인 최종 정차의 S가 들어가야 함) 의도된 비대칭이고 안전 쪽입니다. 다만 그 대역이 지속되는 정체에서는 위의 "정차마다 S 하나"가 됩니다.
- fsync: 첫 쓰기 `sync=long_stop && sync_ok`, 재기록도 `sync_ok` 필요, 둘 다 교체 중 거짓. 탐침 C: 교체 중 S1(6 s) → 표시 7~13 s `journal_draining`, 14~22 s 지연 150 ms로 막힘 → 23 s 예외 S2(`sync:0`) → 90 s까지 교체 중 정차 유지: `"sync":1` 행 0. 91 s 교체 끝 → fsync 재기록 1회(기존 동작). 요청 뒤 교체가 시작된 fsync 쓰기의 취소(`g3_cancel_ignores_replacing`이 `in_flight_sync`면 거짓)는 변경 없음.
- 30 s 간격이 남는 경우(탐침 D와 `test_g3.cpp:1573-1630`): 표시가 들어가 디스크가 M이면 36 s까지 기다림; 방위 없는 S; 기록 없음.

### 탐침 (FACT, 하네스 복사본)

- A: 교체 중 S1(6 s), 7~13 s 이동 `journal_draining`, 18 s부터 72 m 지점 정차, 모든 요청을 게이트 취소(`G3_IO_CANCELLED`)로 강제: 취소 행 18 s(표시), **23, 28, …, 58 s(S 8회)**. 디스크는 S1 그대로. 2차 코드라면 23 s는 30 s 간격에 막히고 36 s 요청 뒤 다음은 66 s였을 것입니다.
- B: S1(6 s, 방위 200°) → 7 s 0.3 m 기어감 + 1.5° 회전(표시 요청 없음: 0.5 m·2° 미만) → 8 s부터 정차, 방위 201.5°: **13 s에 S2 발행**(`mono_ms 13000`, 201.50°). 출발 표시가 정의상 없는 경우인데 예외가 적용됩니다(안전 쪽, 문구 문제).
- C: 위 fsync 항목.
- D: 표시가 7 s에 들어감(디스크 M) → 18 s 정차의 S는 36 s까지 간격 유지.

## (2) 재시도 래퍼 `tests/runtime/run_worker_beta_timing.sh`, `test_worker_beta.cpp:238-248, 1116, 1166, 1202-1208`

- 77이 나오는 길은 하나뿐입니다: `timing_inconclusive`가 참일 때 main 끝의 `return 77`(1207행). 그 변수는 `lag_check`만 쓰고 `lag_check`는 g3_cancel(1116행)과 g3_garage(1166행, 분기의 마지막 문장)에서만 불립니다. 다른 모든 단언은 `assert`(SIGABRT, 134)이고 시나리오 시간 초과는 `alarm`(142)이므로 77보다 먼저 프로세스를 끝냅니다. g3_cancel에서는 `lag_check` 호출이 분기의 마지막 단언 자리이고, 그 뒤 공통 정리(`unlink`, `rmdir` 단언)가 77보다 먼저 돕니다. g3_park의 같은 검사는 그대로 `assert`입니다(1180행).
- 래퍼는 77만 재시도하고 그 밖의 종료 코드는 즉시 그대로 넘깁니다(가짜 명령 행렬, 위 표). 문턱(lagging 없음, 요약 간격 < 1.6 s)은 바뀌지 않았습니다. POSIX sh로 문제 없습니다.
- ARM/QEMU 스위트는 새 래퍼를 **쓰지 않습니다**: `tests/run_arm_all.sh:277-279`가 11개 시나리오를 `$timing`(= `tests/run_arm_timing.sh`)으로 돌리고, 그 래퍼는 2026-10-10부터 **어떤 실패든** `MX5DR_TIMING_ATTEMPTS`(기본 3)회 시도하며 77은 실패한 시도로 세고 마지막에 1로 바꿉니다(89행). 새 "inconclusive host timing" 메시지는 그 래퍼의 `reason()` 패턴("inconclusive")에 잡혀 장부에 남습니다. AGENTS의 "ARM_TIMING_RETRIES=0"은 장부의 재시도 횟수이지 재시도 비활성이 아닙니다. 즉 호스트 쪽이 ARM보다 엄격합니다(P5).
- **P1 (PLAUSIBLE)**: lagging 검사는 호스트 CPU 문제만이 아니라 제품 성질(저장 스레드의 fsync가 저널 작성기를 막지 않음, 사전 등록 규칙의 "overlap")을 겸합니다. 간헐적인 진짜 지연(예: 3회 중 1회)은 호스트에서 재시도로 통과합니다. 완화: 재시도마다 `WORKER_BETA_TIMING_RETRY`가 stderr에 남고 실패 출력도 그대로 남으며, ARM은 그 전부터 같은 정책입니다. 권고: 릴리즈 파이프라인이 호스트 로그의 그 줄을 ARM 장부처럼 세어 기록에 적을 것.

## (3) `reverse_unsure_is_observation_only`, `replay_beta --writer-lag-ms`

- M4: 위 C2. 새 시험의 비교는 공허하지 않습니다(`assumed[0]==1 && published[0]>0`를 검사). 한계(설계상): 창 밖의 결정이 계수를 읽는 변이(예: 유효 래치 경로만 읽음)는 이 시나리오가 그 경로를 지나지 않으면 살아남습니다. 계수를 읽는 곳이 `g3_send.h:206, 301, 319, 337`뿐임은 grep으로 다시 확인했습니다.
- `--writer-lag-ms` 기본 0: `g3_io_block_reason(true, replacing&&!garage, true, true, false, 0)`은 옛 삼항식과 같은 값이고 `sample.writer_lag_ns=0`은 이전의 0과 같습니다. 바이트 동일성은 위 표(부모 바이너리 대 새 바이너리, 모드 셋, 48 파일). 인자 검사(0~60000 ms)와 usage 추가 확인.

## (4) AGENTS.md 문안 대 코드

맞는 것(모두 확인): 표식 `g3-garage-off`(메뉴 12, `packaging/trial:645`)·`g3-off`(메뉴 8, 641행), 메뉴 1 재설치가 둘 다 지움(`install.sh:175-178, 193-196`), 메뉴 4가 garage 표식 지움(`uninstall.sh:18`); 선택 6 = `BETA_BEARING_OVERLAY`(`adapter.h:24`); 플래그 4(`G3_FLAG_DEPART`); 60°(`G3_GARAGE_UNC_MAX_DEG`, 776·749행)·30°(`G3_UNC_MAX_DEG`); 2 %(`G3_GARAGE_ROTATION_PER_RAD`), 0.0023(`G3_GARAGE_YAW_ERROR_RAD_S`), BETA 10 %·0.002(`beta_profile.h:179, 203`); 20 m(`G3_SEND_UNKNOWN_LATCH_MIN_M`); 5 s(`G3_GARAGE_PARKED_NS`); 30 s(`G3_GARAGE_REPLACING_S_GAP_NS`); 100 ms(`G3_GARAGE_REPLACING_MARK_LAG_NS`); 토큰 `<root>/logs/g3-last-start`(`runtime.cpp:1697, 1706`, `beta_requested`이면 표식과 무관하게 기록 판독 전에 swap); 표식이 있으면 플래그 4 기록은 형식 오류(765행); 직전 부팅 비교는 36자 전체(`strlen==36 && strcmp`, 787-789행; 시험은 8자 뒤·안에서 다른 두 경우 추가); 표시는 이동 중 또는 정차 첫 5 s(`parked`가 되기 전 `depart()`, 1267행), 교체 중 지연 ≤ 100 ms(1356행); "any S request, also a cancelled or failed one and the 60 s fsync rewrite"(`last_s_request_ns_`는 요청 시 기록, 1339행); 규칙 F 오염 재기록 예외(`!stale`); 교체 중 fsync 없음; 20 m의 `distance_m`은 "since boot"(`pipeline.h:152`)이지만 파이프라인이 받는 바퀴 이벤트가 승인 뒤의 것뿐이고 속도 0에서는 쌓이지 않으므로 문안과 실질이 같음(2차 검토 (2) 표와 일치).

어긋나거나 빠진 것:
1. "at most one extra write per such departure" — 위 C1. 같은 문구가 `g3_heading.h:149, 1302-1303`과 기록에도 있습니다.
2. "0.0023 is the RMS of the 93 parked yaw zeros" — 위 C3(RMS 0.00224, 올림 0.0023).
3. (누락, P4) 메뉴 6(`purge.sh`)도 표식을 지웁니다(`trial:491, 537` "menu 6 removes it"). 문안에는 메뉴 1·4만 있습니다. 또 "a 110-byte write"는 합성 저장소에서 85 B, 차량 크기의 필드(utc·rt 10자리, chain·crc 8자리)면 110 B 안팎이라 근사치입니다.
4. (문구) "the departure mark is written while the car moves or within the first 5 s of a stop" 다음의 "(afterwards only the parked S record is requested)"는 맞지만, 정차 5 s 뒤 S 요청이 막혀도 표시는 다시 시도하지 않는다는 뜻이 드러나지 않습니다(2차 C1의 핵심 조건). 선택 사항입니다.

## (5) 분석기 `g3_replacing_rate_unmarked` (`tools/analyze_logs.py:3980-4004`)

합성 행(FACT): 빈 로그 → (0, []); S 없이 `unstored replacing_rate` → 0; 복원 S(`loaded`, prev 있음) 뒤 보류 정차 둘 → **2**(같은 S가 정차마다 셈: 보류된 정차 하나가 결함 하나이므로 타당하나 "stale S 개수"로 읽으면 안 됨); 거부된 복원 S(`rejected not_prev_boot`) 뒤 보류 → 1(코드의 `disk_` 의미와 일치); 세션 1의 S + M, 세션 2의 S + 보류 → 세션 2만 1; 다른 세션의 S는 영향 없음; S → `g3_since left stored` → 보류 → 1; garage 키 없는 S → 0; 오염 재기록 S(hv 0) → 0. 단위 시험의 "defect" 출력도 확인.

- **P2 (PLAUSIBLE, 견고성)**: `event=="written" && garage==1`인 행에 `st`나 `hv`가 없으면 `KeyError`로 보고서 전체가 죽습니다. 이웃 함수 `g3_unmarked_departures`도 같습니다(c1f00f4부터). 제품은 그런 행을 내지 않지만 `test_malformed_rows`는 garage 행을 다루지 않습니다. `row.get`으로 바꾸기를 권합니다.
- **P3 (PLAUSIBLE, 과소 집계)**: `g3_since unstored`의 `reason`은 **마지막** 막힌 이유입니다(`episode_block_`은 막힐 때마다 덮어씀, 1289-1309행). 정차가 30 s 동안 `replacing_rate`로 막힌 뒤 다른 이유(`journal_draining`, `writer_busy`)로 막힌 채 끝나면 세지 않습니다. 과대 집계는 보이지 않습니다. 기록의 "시동 끄기로 끝나는 최종 정차는 보이지 않음"은 맞습니다.

## (6) 두 안전 성질의 회귀 (찾지 못함)

- 교체 중 fsync 없음: 코드(위 (1))와 탐침 C, 기존 시나리오 g3_garage의 `sync:0` 단언.
- 송신 변경은 바이트 40, 44..47만: 이번 커밋이 건드린 12개 파일에 `g3_send.h`, `adapter/*`, `worker_tick.h`, `runtime.cpp`가 없고(`git show --stat`), 재생 시험 `test_sends_differ_only_in_the_bearing_of_overlaid_fixes` 통과, 부모 바이너리 대 새 바이너리의 `sends.jsonl`이 모드 셋에서 동일.

## 권고 (리드·소유자 결정 사항)

1. 문구 셋 수정: C1 한도(AGENTS, `g3_heading.h` 주석 두 곳, 기록), C3 RMS, 기록·2차 검토 인용의 "M4 모두 통과"(test_beta가 잡음).
2. 릴리즈 파이프라인: 호스트 로그에서 `WORKER_BETA_TIMING_RETRY|WORKER_BETA_TIMING_FAILED`를 세어 기록에 적기(ARM 장부와 같은 역할).
3. 선택: 분석기 `row.get`과 P3의 한계 주석; AGENTS에 메뉴 6 추가.

## 하지 않은 것

ARM/QEMU 스위트, 전체 `make test`, 스톡 BusyBox 하네스, 비공개 주행 재생(20 부팅 110 파일 동일성은 저자 주장 그대로), 0.0023의 93/111 재검산, 차량·폰·DHU 실행. 스크래치 산출물(`test_g3_probe.cpp`, M4 worktree, 부모 worktree, 합성 사슬 출력)은 스크래치 디렉터리에만 있습니다.
