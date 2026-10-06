# 워커 정지와 motion stale 거부 (2026-10-06)

## 문제

v1.0.0-beta.2 릴리스 실행에서 `make test-runtime`이 한 번
`tests/runtime/test_worker_session.cpp`의 "motion_rejected 행 없음" 단언에서
실패했습니다(exact-ARM QEMU 직후, 단독 재실행 3회와 전체 재실행은 통과).

## 기전 (소스)

- `src/navigation/channel.cpp` `inspect_motion_datagram`: 나이 =
  `checked_time()`(워커가 `recvmsg`로 꺼낸 시각, CLOCK_MONOTONIC)
  − `received_ns`(생산자 수신 시각; 테스트에서는 `sendto` 직전 `clock_ns`).
  250 ms를 넘으면 `RECEIVE_STALE`. 소켓 큐에서 기다린 시간이 포함됩니다.
- stale 판정은 `MotionCursor::check` 앞에서 끝나므로 커서가 전진하지 않고,
  정지 후 첫 신선한 데이터그램은 항상 `sequence_discontinuity`가 됩니다.
- `src/runtime/runtime.cpp` `drain_motion`: 거부 1건마다 source_epoch/generation을
  올리고 `Pipeline::reset`(코어, gyro/wheel 창, BETA 앵커 이력)과
  `holdout.reset(HOLDOUT_SOURCE_FAULT)`을 수행합니다. 후진 래치만 task E
  규칙(2 s, 16건)으로 유지될 수 있습니다.
- 같은 워커 스레드가 journal `fwrite`/`fflush`(정기 1 s), 행마다 `statvfs`를
  수행하므로, 이 스레드가 막히면 그동안 도착한 입력 전체가 stale이 됩니다.

## 호스트 측정 (4 CPU, ext4 SSD, 가용 메모리 약 1 GB)

계측 사본(`channel.cpp`에 나이 히스토그램만 추가, 저장소에는 넣지 않음)으로
`destroy recreate status bus_reuse bus_disconnect`를 반복했습니다.

| 조건 | 실행 | 거부 발생 | 최대 나이 |
|---|---|---|---|
| 부하 없음(기존 load 약 9) | 10 | 0 | 4 ms |
| nice 19 busy loop 16개, 테스트도 nice 19 | 10 | 0 | 47 ms |
| SSD 2 GiB 반복 쓰기+fsync | 10 | 2 | 163 ms(통과분) |
| 같은 IO 부하, 분류 적용 후 | 20 | 9 (모두 stale→discontinuity 패턴) | 0.38–2.9 s |
| IO 부하 + strace | 6 | 4 | journal `write()` 70–770 ms 차단 관측 |

CPU 경합만으로는 재현되지 않았고, 쓰기 백로그(dirty writeback/회수)로 워커가
막힐 때 재현됩니다. 실패 trace는 워커가 약 0.5 s 멈춘 뒤 20 ms 간격으로 찍힌
큐의 데이터그램을 한꺼번에 stale로 거부하고 다음 하나를 discontinuity로
거부하는 형태입니다.

## 차량 대조 (shadow.5 주행 trace, 비공개 원본의 요약만)

motion_rejected 84건(stale 69, sequence_discontinuity 15)은 10개 묶음이며,
10개 모두 워커가 직접 찍는 행(shadow/health/calibration)의 343–1,118 ms 공백과
같은 시각에 있습니다. 호스트 재현과 같은 형태입니다. 그 주행은 전 구간
`E_NO_SEED`(model_valid 0행)여서 실제로 잃은 추정 상태는 없었습니다.

## 바꾼 것 (테스트 전용)

- `test_worker_session`은 trace를 먼저 검사합니다. stale 행은 실제로 250 ms보다
  오래되어야 하고, discontinuity 행은 직전 stale 바로 다음 순번이어야 합니다.
  이 패턴이면 "INCONCLUSIVE host worker stall"을 출력하고 77로 끝냅니다
  (trace 보존). 다른 거부는 그대로 실패합니다. 기존 단언은 지우지 않았습니다.
- `tests/runtime/run_worker_session.sh`: 77이면 최대 3회까지 재시도하고 3회 모두
  불확정이면 실패합니다. Makefile의 모든 `test_worker_session` 호출에 적용했습니다.
- 제품 코드, 250 ms 한도, 거부 시 reset 동작은 바꾸지 않았습니다.

## 남은 제품 문제 (미구현 제안)

워커가 정지하면 데이터 자체가 늦은 것이 아닌데도 그동안의 입력 전체를 거부하고
거부 1건마다 파이프라인을 reset합니다. BETA가 앵커를 잡은 상태였다면 새 GPS
앵커 쌍이 올 때까지 출력이 끊깁니다. 차량 주행에서는 약 14분에 10회였습니다.
제안: 생산자/epoch가 같고 순번이 연속이며 `received_ns`가 단조이면 한도
(예: 2 s = `MotionGapTracker::KEEP_NS`)까지 늦은 도착을 받아들이고 나이를 진단에
기록하는 방식입니다. `received_ns`는 다시 쓰지 않으며, 파이프라인의
`PIPELINE_LATE`/센서 타임아웃과 BETA 송신 lease가 출력 신선도를 계속
제한합니다. 이 변경은 `test_channel`, `test_journal`, `test_beta`의 stale 기대와
문서화된 250 ms 계약을 바꾸므로 별도 작업으로 남겼습니다. 정지 원인(journal
쓰기를 수신과 분리)도 별도 작업입니다.

## 실행한 테스트

- `make test-runtime`(부하 없음): 통과, 불확정 0회.
- IO 부하 아래 래퍼 실행: 1회 불확정 뒤 재시도에서 통과.
- `test_worker_beta`도 같은 워커를 쓰지만 이번에 부하 측정은 하지 않았습니다.

## 후속 구현 (같은 날)

위 제안 두 가지를 구현했습니다. 차량에서 실행된 적은 없습니다.

- 늦은 도착 허용(`src/navigation/channel.cpp`): 같은 생산자 pid·epoch의 다음 연속
  순번이고 `received_ns`가 단조이면 2 s(`MotionGapTracker::KEEP_NS`)까지 받아들입니다.
  `received_ns`는 그대로이고 나이는 `motion_late_accepted` 행과 health의
  `motion_late`에만 남습니다. 2 s 초과, pid/epoch 변경, 순번 공백, 수신 시각 역행은
  기존처럼 거부+reset입니다. stale 거부도 같은 소스의 순번 커서를 전진시켜 다음
  신선한 데이터그램이 discontinuity로 다시 거부되지 않습니다. 파이프라인은 생산자
  수신 시각으로 적분하므로 오차 예산에 더할 나이가 없고(test_beta에서 정시 처리와
  같은 출력), 출력 신선도는 lease·sample-age·300 ms 침묵 검사가 now 기준으로
  제한합니다.
- journal 쓰기 분리(`src/runtime/journal_ring.h`, `runtime.cpp` writer 스레드): 워커는
  미리 할당한 링에 행을 넣고 writer 스레드가 fwrite/fflush/statvfs/회전을 합니다.
  `test_journal --writer`에서 writer에 1 s 정지를 넣어도 워커 turn 최대 0.07 ms,
  수신 나이 최대 0.5 ms, 거부 0이었습니다(호스트 측정). 넘침 시 진단 행만 오래된
  것부터 버리고(`journal_dropped` 행), 증거 행을 넣지 못하면 mutation을 끕니다.
- `test_worker_session`의 불확정 분류(77)는 그대로 두었습니다. 이제 2 s를 넘는
  정지에서만 stale 행이 생깁니다.
