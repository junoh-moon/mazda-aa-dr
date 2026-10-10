# ARM 에뮬레이션 시간 시험 안정화 (2026-10-10)

## 문제

최근 세 릴리즈에서 `tests/run_arm_all.sh`(고정 GCC 4.9.1, qemu-arm 8.2.2 user mode,
`nice -n 10`)의 첫 실행이 매번 다른 시간 단언 하나에서 멈췄습니다. 조용한 호스트에서 다시
돌리면 통과했습니다(PASS 331).

- beta.7: `runtime-lds-association-test drain_bus`의
  `source.status().positions==positions+1`, `journal-test --writer`의
  `journal_lag_bound`, `writer_age_flush`(`oldest_ns<50 ms`).
- beta.8: `writer_age_flush`(`test_journal.cpp:579`, 50 ms 잔여 검사).
- beta.6 코드: `steady_max<450 ms`, `journal_lag_bound`.

이 단언들은 실제 제품 계약입니다(기록 스레드 250 ms 자체 flush, flush 뒤 잔여 50 ms 미만,
1.5 s 지연 가드와 0.5 s 해제, 워커 턴·idle 게이트). 제품 코드와 상수, 단언 임계값은 바꾸지
않았습니다.

## 측정 방법

- 호스트: 4코어 x86-64, 다른 작업(k3s, 다른 에이전트 빌드)이 함께 돌아 부하를 통제할 수 없었습니다.
  그래서 실행마다 시작 시 1분 부하 평균(`/proc/loadavg`)을 기록했습니다.
- 바이너리: `run_arm_all.sh`와 같은 명령으로 fff1021에서 빌드했습니다. 부하 측정의
  `journal-test`·`runtime-lds-association-test`는 단언 바로 앞에 값 출력 한 줄만 넣은 계측
  사본입니다(저장소에 넣지 않음).
- 조건(시험은 모두 `nice -n 10`, 측정 흐름 2개 병렬):
  - quiet: 추가 부하 없음(측정 중 부하 평균 0.9–7.8, 평균 2.5–2.9).
  - L4(요청한 조건): `yes` 바쁜 루프 4개를 시험보다 nice 10 낮게(부하 평균 평균 11–13).
  - L2: 바쁜 루프 2개와 주변 부하(부하 평균 평균 4.4–7.1).
- 매 실행 전 보정 프로브(ARM 바이너리, QEMU에서 `usleep(1000)`·`usleep(20000)` 초과분과
  condvar 깨움 지연 p50/p99/max)를 돌렸습니다(journal과 association 사례).

## 실패 횟수

| 시험(사례 수 × 반복) | quiet | L2 | L4 |
|---|---|---|---|
| `journal-test --writer` | 1/30 | 9/10 | 6/6 |
| `journal-test`(인자 없음), `--storage` 5개 | 0/180 | 0/60 | 0/36 |
| `runtime-lds-association-test` 10개 | 0/300 | 3/100 | 0/60 |
| `drain_bus`+`drain_session` 원본, 60회씩 | – | 6/120 | – |
| 같은 두 사례, 수정한 시험, 60회씩 | – | 0/120 | – |
| `worker-session-test` 24개 | 0/360 | 0/360 | 34/34 |
| `worker-lds-test` 6개, `worker-lds-source-test` 9개 | 0/225 | 0/225 | 0/15 |
| 제품 DSO `runtime-assist` 9개(측정 흐름 1개, 부하 평균 평균 2.2 / 3.5) | 0/270 | 3/270 | – |

`--writer`에서 실패한 단언:

- quiet 1회: `window_burst_lag` 느린 기록 스레드(90 rows/s)의 `max_lag<1 s`, 측정값 1026 ms.
- L2 9회: `journal_lag_bound` 미회복 5, `window_burst_lag` 2, `window_burst`의 `flush_wait`(10 s) 1,
  `writer_age_flush`의 1.6 s 정지 `lowered_after>=1200 ms`(1143 ms) 1.
- L4 6회: 모두 `journal_lag_bound`. 2.5 s 정지 뒤 8 s 동안 따라잡지 못했습니다(`caught up at 0 ms`).
- `worker-session-test`(L4): 34회 모두 자체 `alarm(20)`에 걸렸습니다(SIGALRM, quiet 8.3 s → 20.3 s).
- `runtime-assist`(L2) 3회: `drive()`의 `missing runtime output`(`publication` 2, `journal_failure` 1).
  창마다 워커 출력을 16 × 5 ms = 80 ms 동안만 기다립니다. 전체 스위트 측정 중에도
  `recovery`에서 한 번 실패했습니다(아래 new-quiet-4). 처음 분류할 때는 빠뜨렸던 시험입니다.

통과한 실행의 여유(quiet 30회, 괄호는 단언 범위):

| 값 | 최소 | p50 | p90 | 최대 |
|---|---|---|---|---|
| 첫 자체 flush ms [240, 600) | 250 | 250 | 251 | 251 |
| 정상 상태 지연 최대 ms (<450) | 242.0 | 244.1 | 262.0 | 262.5 |
| 400 ms fflush 지연 최대 ms [400, 1500) | 586 | 606 | 609 | 626 |
| 1.6 s 정지 후 가드 하강 ms [1200, 1700] | 1332 | 1352 | 1373 | 1450 |
| 2.5 s 정지 후 provenance 보류 ms [1400, 1700] | 1501 | 1513 | 1517 | 1518 |
| 복귀 − 따라잡음 ms [0, 60] | 0 | 0 | 0 | 0 |
| 대기 틈 flush 지연 ms (<700) | 199 | 200 | 200.5 | 207 |
| 90 rows/s 창 배출 최대 지연 ms (<1000) | 362 | 403 | 508 | 1026 |

L2에서는 정상 상태 지연 최대가 386 ms, 1.6 s 정지 하강이 1143 ms까지 벌어졌습니다.

QEMU 수면 초과분(프로브):

| 조건 | `usleep(1 ms)` p99 평균 | `usleep(1 ms)` 최대 | `usleep(20 ms)` 초과 최대 | condvar 깨움 최대 |
|---|---|---|---|---|
| quiet (360) | 0.6 ms | 22.8 ms | 17.9 ms | 17.7 ms |
| L2 (121) | 3.0 ms | 62.8 ms | 67.5 ms | 46.7 ms |
| L4 (73) | 5.1 ms | 86.4 ms | 113 ms | 110 ms |

## 근본 원인

세 종류였습니다.

1. **시험 관측 경쟁(시험 결함, 수정함).**
   - `writer_age_flush` 50 ms 검사: 기록 스레드는 `flush_count`를 fflush **시작 전에** 올리고
     그다음 `busy_since`를 둔 채 `fflush`, `ring.flushed()`, `flushed` 저장을 합니다. 진행 중인
     fflush가 지연으로 보이는 것은 의도된 동작입니다. 시험은 `flush_count`가 바뀌자마자(1 ms 폴링)
     지연을 읽으므로, 호스트가 그 사이에서 기록 스레드를 멈추면 250 ms 행 나이를 읽습니다.
     재현: 제품의 `flush_count` 증가 직후에 5 ms 선점을 넣은 변형에서 원래 시험은 3회 모두
     `:579`에서 실패했고, 수정한 시험은 첫 시도에 통과했습니다(첫 flush 250 ms, 정상 지연 248.6 ms).
     부하 측정 5회에서는 이 창에 걸리지 않았습니다(완료 후 잔여 0 µs). 창이 짧아 드물게만 맞습니다.
   - `drain_bus`/`drain_session`: idle 게이트(1→2)는 요청 뒤 **첫** idle 수면에서 멈춥니다.
     그 턴이 이미 큐 비우기를 지난 뒤였다면 마지막 `wait_match` 콜백의 POSITION이 큐에 남고,
     경계 턴이 두 개를 셉니다. 계측 결과 실패 9건이 모두 `positions_delta=2`, 통과 143건이 모두 1이었습니다.
2. **일시적 스케줄링 지연(재시도 대상).** 20 ms 턴 루프나 기록 스레드가 수백 ms 밀리면
   `steady_max<450 ms`, `lowered_after` 범위, 창 배출 `max_lag<1 s`를 넘습니다. quiet에서도
   생깁니다(1/30). 프로브 수면 초과분은 부하와 함께 늘지만(p99 0.6→5 ms) 실행 단위 예측력은 없었습니다.
   quiet 실패 실행의 프로브는 무난했고(p99 0.16 ms, 최대 0.44 ms), 통과 실행에서 최대 23 ms가
   나온 경우도 있었습니다. 부하 평균이 더 나은 조건 지표입니다.
3. **지속적 CPU 몫 부족(재시도로 해결되지 않음).** 에뮬레이션된 기록 스레드는 CPU 하나를 다 써도
   약 90 rows/s입니다. `journal_lag_bound`는 50 rows/s 생산을 따라잡아야 하고, `window_burst`의
   `flush_wait`는 10 s, `worker-session-test`는 20 s 안에 끝나야 합니다. nice 10 시험이 nice 0
   작업 넷과 코어 4개를 나누면 CPU 몫이 약 1/10이 되어 매번 실패합니다. 제품 계약은 맞게
   동작한 것입니다. 기록이 따라잡지 못하면 BETA는 계속 보류되어야 합니다.

## 변경

- `tests/run_arm_timing.sh`(POSIX sh, `run_worker_session.sh`와 같은 방식): 시간 계열 시험만
  최대 3회(`MX5DR_TIMING_ATTEMPTS`) 시도하고 시도 간격은 5 s(`MX5DR_TIMING_RETRY_PAUSE`)입니다.
  한 번이라도 통과하면 통과합니다. 매 시도의 전체 출력 뒤에
  `ARM_TIMING_ATTEMPT name=... attempt=k/3 exit=... load=... assertion=...`을 남깁니다.
  재시도한 시험과 세 번 모두 실패한 시험은 ledger에 첫 실패 단언·부하와 함께 기록합니다.
  exit 77(호스트 정지 판정 불가)도 실패한 시도로 셉니다.
- `tests/run_arm_all.sh`: `journal-test --writer`, `runtime-lds-association-test` 10개,
  `worker-lds-test`, `worker-lds-source-test`, `worker-session-test` 전 사례(환경 변형 포함)와
  제품 DSO `runtime-assist` 묶음(`run_unwind_dso.py --suite runtime-assist`)만 wrapper로 돌립니다.
  `runtime-assist`는 스위트 측정 4회차의 실패를 보고 추가했습니다. 이 묶음의 정적 검사(export 숨김,
  소스·DSO 해시)도 결정적이라 함께 재시도해도 판정이 바뀌지 않습니다. 나머지 시험은 한 번만 돌립니다. EXIT trap이 성공·실패 모두에서
  `== ARM timing-class flaky report`, `ARM_TIMING_RETRIES=n`, `ARM_TIMING_FAILED=n`을 출력합니다.
  `alarm()`만 있는 무한 대기 감시 시험(handoff, LDS tap, association channel 등)과 python fixture는
  시간 계열에 넣지 않았습니다.
- `tests/runtime/test_journal.cpp` `writer_age_flush`: `flush_count` 변화로 첫 자체 flush 시각을
  재는 것은 그대로 둡니다. 50 ms 잔여 검사만 같은 flush가 `flushed`에 완료를 게시한 뒤에 하고,
  그것이 요청 없는 그 한 번의 flush인지(`flush_count==flushes+1`) 확인합니다. 임계값은 그대로입니다.
- `tests/adapter/runtime_lds_association_test.cpp`: idle 게이트를 두 단계로 바꿨습니다
  (1 → `IDLE_ARMED` → 2). 요청 뒤 **시작한** 완전한 턴이 끝날 때만 멈추므로 시험 주석의 전제
  "이전 행은 모두 비워짐"이 실제로 성립합니다. `positions==positions+1`, `retirements+1`은 그대로입니다.

## 결함을 숨기지 않는다는 확인

모두 wrapper로 실행했습니다(제품 변형은 검증 사본에서만 만들었습니다).

| 변형 | 결과 | 첫 실패 단언 |
|---|---|---|
| m1 `JOURNAL_FLUSH_MAX_AGE_NS` 250→700 ms | 3/3 실패 | `first_flush_ms<600`(2번째 시도는 대기 틈 `flush_latency<700 ms`) |
| m2 지연 가드 하강(1.5 s) 제거 | 3/3 실패 | `journal_lag_bound` `lowered_after` 범위 |
| m3a 기록 스레드가 행마다 200 ms 지연 | 3/3 실패 | `journal_lag_bound` 미회복 |
| m3b fflush마다 200 ms 지연 | 3/3 실패 | `journal_lag_bound` 미회복 |
| m4 자체 flush가 행 시간을 지우지 않음 | 3/3 실패 | `journal_lag_bound` 미회복 |
| m5 POSITION 내부 재확인이 source를 리셋하지 않음 | 3/3 실패 | `drain_bus` `retirements==retirements+1` |
| p1 `flush_count` 직후 5 ms 선점(원래 시험) | 3/3 실패 | `:579` `oldest_ns<50 ms` |
| p1 같은 선점(수정한 시험) | 1회 통과 | – |
| 주입한 flaky(3회 중 첫 회, steady 구간 fflush 300 ms 정지) | 2번째 통과, `RETRIED` 기록 | `steady_max<450 ms`(544 ms) |

결정적 결함은 세 시도 모두 실패해 스위트를 실패시킵니다. 3회 중 1회 실패하는 주입 사례는
통과하지만 ledger와 flaky report에 남습니다.

## 전체 스위트 결과

같은 조건의 두 트리를 300 s 간격으로 겹쳐 돌렸습니다(fff1021 원본 = old, 이 변경 = new).
조건 이름은 추가 부하이고 괄호 안은 시작 부하 평균입니다.

| 실행 | old(재시도 없음) | new(wrapper와 시험 수정) | new 재시도 |
|---|---|---|---|
| quiet 1 | 무효: 간격 없이 동시에 시작해 두 AA 설치 probe가 겹침(`the worker did not arm BETA`) | 통과 (1.43) | 0 |
| quiet 2–6 | 통과 3, 실패 2(`window_burst_lag` 느린 기록 1, `journal_lag_bound` 1) | 통과 4, 실패 1 | 0 |
| loaded2 1–5 (바쁜 루프 2개, 3.4–6.0) | 통과 3, 실패 2(`window_burst_lag` 1, `journal_lag_bound` 1) | 통과 5 | 1 |

- new-quiet-4의 실패는 당시 wrapper 밖에 있던 `runtime-assist` `recovery`의 `missing runtime output`
  하나였습니다. 그래서 이 묶음을 시간 계열에 넣었고, 5·6회차부터 적용했습니다.
  시간 계열 시험 50(+1)개는 6회 모두 첫 시도에 통과했습니다.
- new-loaded2-1에서는 `journal-test --writer`가 1회차 `writer_age_flush` `provenance(...)`(부하 4.94),
  2회차 `steady_max<450 ms`에서 실패하고 3회차에 통과했습니다. flaky report에
  `RETRIED: journal-test --writer attempts=3 first_failure_load=4.94 ...`와 `ARM_TIMING_RETRIES=1`이 남았습니다.
- 통과율(유효한 짝 10회): old 6/10, new 9/10(실패 1은 위 분류 누락이며 추가 뒤에는 통과), quiet 1을 더하면 new 10/11,
  new 재시도 합계 1. 표본이 작습니다. old의 `--writer`는 new의 무거운 후반부와 겹쳤습니다.
  따라서 두 시험 수정이 `window_burst`·`lag_bound` 실패를 줄였다는 증거로 읽지 않습니다.
  그 두 실패는 재시도 대상입니다.
- L4(바쁜 루프 4개)에서는 스위트를 돌리지 않았습니다. 측정에서 `--writer` 6/6과
  `worker-session-test` 34/34가 결정적으로 실패했으므로 old와 new 모두 실패합니다.
  new는 세 시도 모두 실패한 뒤 `ARM_TIMING_FAILED`로 보고합니다.
- 변경한 두 시험은 호스트 빌드에서도 확인했습니다(`test_runtime_lds_association` 10개 사례와
  `drain_bus`/`drain_session` 각 10회, `test_journal`, `test_journal --writer` 통과).
  호스트 `make test` 전체는 돌리지 않았습니다.

## 한계와 잔여 위험

- 재시도는 1/3 미만 확률로 실패하는 **실제** 경쟁 조건을 가릴 수 있습니다(3회 독립 시도라면
  실패 확률 p가 p³로 줄어듭니다. p=0.3이면 2.7%). 탐지 방법:
  - 릴리즈 노트에 `ARM_TIMING_RETRIES`를 매번 적고 흐름을 봅니다. 같은 시험·같은 단언이 연속
    릴리즈에서 재시도되거나, 부하 평균 약 2 이하에서 재시도되면 조사합니다.
  - wrapper가 모든 시도의 전체 출력을 남기므로 첫 실패 증거는 그대로 남습니다.
    릴리즈 evidence의 `arm-tests.txt`를 덮어쓰지 않습니다.
  - 의심되면 `MX5DR_TIMING_ATTEMPTS=1`로 해당 시험을 조용한 호스트에서 30회 반복합니다.
- 지속적인 CPU 몫 부족(4코어가 nice 0 작업으로 가득 찬 채 nice 10 실행)은 재시도로 고쳐지지 않으며
  고쳐서도 안 됩니다. 스위트는 실패하고 flaky report가 첫 실패 단언과 부하를 보여 줍니다.
  호스트가 조용할 때 다시 실행합니다.
- 측정 호스트의 주변 부하를 통제하지 못했습니다. 조건 이름보다 실행마다 기록한 부하 평균이
  더 정확합니다. QEMU 결과는 차량 검증이 아닙니다.
- `writer_age_flush` 50 ms 경쟁은 자연 부하에서 재현하지 못했습니다(quiet 30, L2 5).
  선점 모델 변형으로 기전을 보였습니다.
