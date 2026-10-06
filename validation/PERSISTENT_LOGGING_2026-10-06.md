# 상시 BETA의 조용한 로그 프로파일 (2026-10-06)

이 기록은 호스트의 합성 주행 측정입니다. 차량, 폰, DHU 증거가 아니며 차량
방문이나 주행을 요청하지 않습니다.

## 문제

v1.0.0-beta.2부터 제품은 매 부팅 실행됩니다(validation/PERSISTENT_GUARD_DESIGN_2026-10-06.md).
전체 로그는 약 28-37 KB/s(validation/TRIP_SHADOW5_2026-10-05.md: 평균 27.7 KB/s,
AA 연결 중 35.8 KB/s)로, 운전 1시간에 약 110-130 MB입니다. 대부분은
motion_batch 행과 type 3 SEND 행입니다. 저장소는 마모 평준화 구조를 모르는
eMMC의 586 MB 분할영역이고, 회전 용량은 trace 120 MiB, collector 8 MiB였습니다.

## 바꾼 것

`mx5dr.conf`의 새 키 `log_profile=full|persistent`(없으면 full).

- **persistent**: 설치기의 persistent 정책(BETA 기본 설치)이 `max_log_bytes=8388608`,
  `max_log_files=2`(trace 16 MiB), `log_profile=persistent`를 씁니다. collector
  회전은 이 프로파일에서 1 MiB×2로 줄입니다.
- **full**: SHADOW 시험 번들, `--one-boot`, 기존 시험의 기본값으로 그대로입니다.

`src/runtime/log_profile.h`의 필터가 journal writer 앞에서 행을 고릅니다(워커 전용,
init 뒤 할당 없음, OEM 스레드는 닿지 않음).

| 행 | persistent에서 |
|---|---|
| boot, shadow_boot, beta_state, beta_anchor, beta_reverse_latch, beta_hold, beta_session_storage, capture_end/incomplete, shadow_disabled, shadow_session/bus, 모르는 kind | 항상 기록 |
| choice != ORIGINAL인 SEND | 항상 기록(증거) |
| POSITION | 등급(class) 전이 행, LOST/NO_FIX 등급 행, CONTEXT_UNAVAILABLE 행은 항상 기록. 나머지(FIX)는 RAW 창. BETA가 live(마지막 beta_state가 DISABLED/FAULT 아님)인 동안에는 RAW 창의 POSITION도 journal 링의 증거 등급으로 넣어 쓰기 백로그에서 버려지지 않음(2026-10-07 수정; 기록량은 바뀌지 않음) |
| beta_summary | BETA가 ENGAGED/SPEED_ENGAGED/GPS_LOST/NO_FIX면 초당 1, 그 밖에는 10 s당 1, DISABLED/FAULT는 항상 |
| health, shadow, shadow_calibration | 10 s당 1(capture 종료 health는 항상) |
| motion_rejected, shadow_input_reset, shadow_pipeline_reset, shadow_position_rejected, shadow_motion_excluded, lds 거부 상태 | 종류별 10 s당 5행, 나머지는 digest의 suppressed 카운터 |
| shadow_holdout | BEGIN/END/ABORT만, 나머지는 카운터 |
| motion_batch, ORIGINAL LOCATION SEND | RAW 창(아래), 통계는 digest |
| ORIGINAL 비-LOCATION SEND, lds_sideband | digest 카운터만 |

- **log_digest**(10 s마다, capture 종료 때 `digest:"final"`): motion 이벤트 수(센서별),
  epoch, 첫/끝 순번, 순번 공백, 최대 수신 간격, 속도 km/h min/max/mean, yaw raw
  min/max/mean과 rad/s 평균/절댓값 최대, 마지막 후진 값, 센서별 최신 수신 시각
  (`wheels_last_ns` 등), SEND 수(유형별, 변경, 0 아닌 결과), POSITION 수(mode,
  등급별), 억제 카운터, RAW 창 상태.
- **RAW 창**: 400 KiB 미리 할당 링(최근 최대 60 s의 motion_batch, ORIGINAL LOCATION
  SEND, FIX POSITION). 사건(BETA 상태 전이, hold, session storage 변경, GPS 등급
  전이, 고장/거부 묶음의 첫 행, late-accept 묶음, capture 종료) 때
  `raw_window` 표시 행 뒤에 오래된 순으로 쓰고, 이후 30 s는 raw 행을 바로 씁니다.
  표시 행에는 trigger, 행 수, 바이트, 덮어쓴 행 수와 실제로 담긴 기간
  `span_ms`가 있습니다. 실제 기간은 입력 속도로 정해집니다(2026-10-07 측정,
  log_rate `--motion-ms/--turn-ms`): 차량 속도(wheel·yaw 각 약 10 Hz, 워커가
  datagram마다 깸)에서 RAW 행 6.2 KB/s, 창 60 s(한도); 각 25 Hz에서 약 39 s;
  각 50 Hz에서 약 23 s. 처음 구현의 192 KiB 창은 차량 속도에서도 약 32 s만
  담았으므로 400 KiB로 늘렸습니다. 창 flush는 진단 등급이며 journal 링의 진단
  용량(512 KiB)보다 작습니다. 메모리: 창 400 KiB(persistent에서만) + 링 768 KiB.

분석기(`tools/analyze_logs.py`): boot 행의 `log_profile`을 세션에 기록,
`log_digest`와 `*_digest`(모르는 digest 종류·필드 포함)는 세기만, `raw_window`에서
motion 순번 연속성을 다시 시작, persistent에서 창 앞부분의 ORIGINAL SEND가 짝
POSITION을 못 찾는 것은 카운터로만 셉니다(변경된 SEND의 검사는 그대로).
`journal_dropped`(writer 넘침)는 inconclusive로 보고합니다. 모든 BETA 검사는
남아 있는 행에 그대로 적용됩니다.

`packaging/trial_status.awk`: boot 행이 persistent이면 health 창 15 s, MODEL 진단 창
12 s, 센서 최신 수신은 digest의 `*_last_ns`로도 판정합니다.
`trial_status.sh`의 설정 검사는 `log_profile=full|persistent`를 받습니다.

## 측정 방법

`build/log_rate`(tests/runtime/log_rate.cpp)는 제품 journal 경로를 모의 시간으로
실행합니다. 실제 adapter POSITION/SEND 로직과 제품 runtime Options(BETA,
libpatch 경우처럼 세션 관측 거절), 실제 행 포맷터, Pipeline/GpsHoldout/
BetaController, 실제 채널 검사를 거치는 drain_motion, Journal과 PersistentLog를
씁니다. 워커 turn 순서(관측, motion, MODEL tick, BETA tick, 1 s health)는
run_worker_association과 같습니다. 합성 주행은 tests/replay 합성 fixture와 같은
방식의 10분 주기(정지, 가속, 40/50 km/h 순항, 90°·45° 곡선, 정지 두 번), 부팅 직후
NO_FIX(저장 좌표, utc 0) 45 s, 선택적으로 10분마다 25 s GPS 끊김(mode 0, 터널)입니다.
AA 트래픽은 2026-10-05 주행에서 잰 비율(1 Hz POSITION+LOCATION, 약 11 Hz
비-LOCATION SEND)입니다. 재현하지 않은 것: LDS sideband/association 행, collector
journal, writer 스레드(바이트는 같음), 실제 타이밍. 모든 수치는 논리 바이트입니다.

검증: full 프로파일의 합성 속도 30.7 KB/s는 차량 실측 27.7-35.8 KB/s와 같은
범위입니다.

## 결과 (1시간 합성 주행)

| 시나리오 | full | persistent | 비율 |
|---|---|---|---|
| NO_FIX 없음, GPS 끊김 없음 | 약 111 MB/h (30.8 KB/s) | 2.54 MB/h (0.71 KB/s) | 1/44 |
| 부팅 NO_FIX 45 s, 끊김 없음 | 111.0 MB/h (30.8 KB/s) | 2.86 MB/h (0.79 KB/s) | 1/39 |
| 부팅 NO_FIX 45 s, 10분마다 25 s 끊김 | 111.1 MB/h (30.9 KB/s) | 7.02 MB/h (1.95 KB/s) | 1/16 |
| 부팅 NO_FIX 12분(10-05 주행과 같은 길이), 끊김 없음 | - | 5.64 MB/h (1.57 KB/s) | - |
| 부팅 NO_FIX 12분, 10분마다 끊김 | - | 9.12 MB/h (2.53 KB/s) | - |

2026-10-07 재측정: RAW 창 400 KiB(실제 60 s), health의 journal 필드, BETA live 중
POSITION 증거 등급(기록량 불변)을 반영했습니다. 처음 수치(192 KiB 창)는 각각
2.33/2.64/5.79/5.43/8.05 MB/h였습니다. 사건 하나(끊김 25 s: 등급 전이, ENGAGED,
budget 철회, GPS 복귀)의 비용은 약 0.7 MB입니다(60 s 이전 + 30 s 이후 raw, 증거
행). 12분 NO_FIX에서는 모든 NO_FIX POSITION과 초당 속도 overlay SEND가 증거로
남아 증가합니다. 사건이 10분보다 잦거나 NO_FIX가 길면 2 KB/s를 넘습니다.

persistent, 부팅 NO_FIX 45 s, 끊김 없음의 행별 기여(B/s, 192 KiB 창 때 측정): beta_anchor 221,
log_digest 98, motion_batch(사건 창) 80, beta_summary 75, shadow 55,
shadow_calibration 55, health 53, POSITION 44, ORIGINAL LOCATION(창) 30, overlay
SEND 21. full에서는 type 3 SEND 17,253, shadow 5,524, motion_batch 2,721,
LOCATION 1,729, POSITION 1,514 B/s입니다.

### 하루 쓰기 추정 (하루 1시간 운전)

| | full | persistent(사건 없음) | persistent(10분당 사건 1) |
|---|---|---|---|
| 논리 바이트/일 | 약 111 MB | 약 2.9 MB | 약 7.0 MB |
| 연간 | 약 41 GB | 약 1.0 GB | 약 2.6 GB |
| 16 MiB 링에 담기는 운전 | (120 MiB 링에 약 1.1 h) | 약 5.9 h | 약 2.4 h |

물리 flash 쓰기는 다릅니다. stdio는 1 s마다 fflush하고 fsync는 capture 종료에만
하므로, 커널 writeback(주기·dirty 만료)이 부분 페이지를 다시 쓰는 만큼 늘 수
있습니다. 예를 들어 5 s마다 마지막 4 KiB 페이지를 다시 쓰면 약 0.8 KB/s가 더해질
수 있습니다. 파일시스템 메타데이터와 eMMC FTL 쓰기 증폭은 측정하지 않았습니다.

## 시험

- `build/test_log_profile`: 종류별 유지/억제/비율 제한, RAW 창 순서와 60 s 한도,
  30 s 이후 구간, 용량 초과 시 최신 행 유지, 저장소 없는 경우, digest 통계와 주기
  초기화, capture 종료 순서(창, final digest, capture_end).
- `tests/journal/test_log_profile.py`(log_rate 1시간, 10분마다 끊김): persistent < 2 KB/s,
  채울 수 있었던 RAW 창의 `span_ms` >= 59 s,
  full > 20 KB/s; beta_state/anchor/hold/session_storage/reverse_latch와 변경 SEND가
  두 프로파일에서 바이트 단위로 같음; motion 이벤트 수가 digest 합과 같음; 분석기
  위반 0, persistent의 발견 코드가 같은 주행의 full 발견 코드의 부분집합, BETA
  집계(교체 수, overlay 수, ENGAGED 횟수, 철회 이유) 동일; 모르는 digest 종류와
  digest 사이의 교체 SEND 허용.
- `test_journal --writer`: 실제 워커+writer 스레드에서 boot 행의 프로파일,
  raw_window, final digest, capture 종료 ack 순서.
- `test_trial_status.py`: persistent health 15 s 창, digest 수신 시각, 다른 boot/미래
  값 거부, full 5 s 창 유지, 설정 키 수용/거부.
- `test_runtime`: 설정 키 파싱(full 기본, 잘못된 값·중복은 설정 무효).
- `test_persistent_beta.py`: BETA persistent 설치의 설정 내용, SHADOW 시험은 full.

## 남은 한계

- 합성 데이터와 모의 시간입니다. LDS sideband 행(full에서도 측정하지 않음)과
  collector 비율은 수치에 없습니다.
- 물리 flash 쓰기량, 파일시스템, eMMC 구조는 모릅니다.
- RAW 창은 사건 직전 최대 60 s(400 KiB 한도; 입력이 빠르면 더 짧고 `span_ms`가
  실제 값) 밖의 raw 행을 버립니다. 사건이 없는 구간의 원자료는 digest 통계뿐이며,
  이것으로 DR 재구성은 할 수 없습니다.
- 사건 없이 CMU가 reset되거나 프로세스가 죽으면, 메모리의 마지막 RAW 창과 writer
  링에 남은 행(정상 시 수 ms, 저장장치 정지 시 더 많음)을 잃습니다. 그때 진단은
  10 s digest, health, 10 s당 shadow 행과 collector journal(1 MiB x 2)에
  의존합니다.
- persistent 프로파일은 차량에서 실행된 적이 없습니다.
