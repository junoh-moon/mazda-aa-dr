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

## 후속 구현 (같은 날, 2026-10-07 정정)

위 제안 두 가지를 구현했습니다. 차량에서 실행된 적은 없습니다.

**실제 완화책은 journal 쓰기 분리입니다.** CMU의 motion 소켓 큐는 순정
`net.unix.max_dgram_qlen` = 10입니다(validation/SHADOW_RUNTIME_2026-09-30.md:36).
datagram 간격이 10-20 ms면 워커가 110-220 ms만 멈춰도 큐가 차고, 그 뒤
datagram은 생산자 쪽에서 EAGAIN으로 **사라집니다**. 2026-10-05 차량 속도(wheel·yaw
각 약 10 Hz, 약 50 ms 간격)에서도 약 0.5 s면 찹니다. 따라서 2 s 늦은 도착 허용이
의미 있는 경우는 드물고, 손실을 막는 것은 워커가 I/O로 멈추지 않게 한 writer
스레드입니다.

- journal 쓰기 분리(`src/runtime/journal_ring.h`, `runtime.cpp` writer 스레드):
  워커는 미리 할당한 링에 행을 넣고 writer 스레드가 fwrite/fflush/statvfs/회전을
  합니다. `test_journal --writer`에서 writer에 1 s 정지를 넣어도 워커 turn 최대
  0.1 ms, 수신 나이 최대 0.6 ms, 거부 0이었습니다(호스트). 넘침 시 진단 행만 오래된
  것부터 버리고(`journal_dropped` 행), 증거 행을 넣지 못하면 mutation을 끕니다.
  writer가 1.5 s 넘게 밀리면 BETA provenance를 거두고(`beta_journal_lag`), 따라잡으면
  되돌립니다.
- 늦은 도착 허용(`src/navigation/channel.cpp`): 같은 생산자 pid·epoch의 다음 연속
  순번이고 `received_ns`가 단조이면 2 s(`MotionGapTracker::KEEP_NS`)까지 받아들입니다.
  `received_ns`는 그대로이고 나이는 `motion_late_accepted` 행과 health의
  `motion_late`에만 남습니다. 큐가 넘친 정지에서 하는 일은 reset 횟수를 줄이는
  것뿐입니다: 예전에는 큐의 10개가 각각 stale 거부+reset이고 첫 신선한 것이
  discontinuity여서 정지 한 번에 약 11회 reset이었지만, 이제 큐의 10개는 late로
  받아들여지고 잃은 datagram 뒤의 첫 것 하나만 `sequence_discontinuity`로 거부+reset
  됩니다(1회). stale 거부도 같은 소스의 순번 커서를 전진시킵니다. 파이프라인은
  생산자 수신 시각으로 적분하므로 오차 예산에 더할 나이가 없습니다.
- `Pipeline::CAPACITY` = 128: 한 receive turn에서 drain 전 이벤트가 128을 넘으면
  129번째가 `PIPELINE_OVERFLOW`로 MODEL reset(`shadow_pipeline_reset` OVERFLOW 행)이
  되고 raw 기록은 모두 남습니다(`test_journal`의 pipeline_capacity_overflow).
  큐가 10개라 정지 backlog로는 이 한도에 닿지 않습니다.
- `test_worker_session`의 불확정 분류(77)는 그대로 두었습니다. 이제 2 s를 넘는
  정지에서만 stale 행이 생깁니다. 이 호스트의 max_dgram_qlen은 512이고 시험 sender는
  EAGAIN에 최대 100 ms 재시도하므로, 호스트 시험은 CMU의 큐 손실을 재현하지 않습니다.

## 재검토 후속 수정 (2026-10-07, v1.0.0-beta.3 이후)

v1.0.0-beta.3(`0dc5a9b`) 독립 재검토가 제안한 다섯 가지를 구현했습니다
([릴리즈 기록](RELEASE_V100_BETA3_2026-10-07.md)의 "알려진 제한과 다음 빌드
과제"). 위 절들은 beta.3 시점 기록으로 그대로 둡니다. 차량에서 실행된 적은
없습니다.

- **지연 측정에 stdio 버퍼 행 포함**: `JournalRing`이 같은 mutex 아래에서
  writer가 꺼냈지만 아직 쓰지 않은 행(held)과 마지막 `fflush` 이후 stdio에 넘긴
  행 중 가장 오래된 push 시각(unflushed)을 기록합니다. writer는 행마다
  `row_written()`, `fflush` 성공 뒤 `flushed()`를 부릅니다.
  `journal_writer_lag`는 대기 행, held, unflushed, 진행 중인 `fflush` 시작
  시각 중 가장 이른 값을 씁니다. stdio가 버퍼 일부를 먼저 내보냈어도 이 값은
  지연을 크게 볼 뿐 작게 보지 않습니다. 이것만으로는 정상 동작에서도 지연이
  worker의 1 s flush 요청 주기만큼 올라가 1.5 s 한도와의 여유가 약 0.5 s뿐이었으므로
  아래 250 ms 자체 flush를 함께 넣었습니다.
- **250 ms 자체 flush**(`JOURNAL_FLUSH_MAX_AGE_NS`): 마지막 `fflush` 뒤 stdio에
  넘긴 행 중 가장 오래된 것이 250 ms가 되면 writer가 스스로 `fflush`하고, 대기
  시간도 그때까지로 줄입니다. worker의 1 s flush 요청은 그대로입니다
  (`flush_wait`, health 주기). fsync 정책은 바뀌지 않았습니다(capture 종료에만).
  정상 지연은 약 250 ms 이하로 유지되고 guard 한도까지 1.2 s 이상 남습니다.
- **pop과 `busy_since` 사이 틈 제거**: held 시각을 `pop()`의 임계 구역 안에서
  저장하므로 꺼낸 행이 지연 계산에서 사라지는 순간이 없습니다. 행 단위
  `busy_since`는 없앴고 `fflush` 시작 시각에만 씁니다.
- **flush·stop 요청 유실 방지**: `JournalRing::wait()`가 mutex 아래에서 조건
  함수를 평가합니다. writer 조건은 `stopping` 또는
  `flush_target > flushed && written >= flush_target`입니다. 요청 쪽은 값을
  저장한 뒤 `notify()`(mutex 사용)하므로 writer가 자기 검사 뒤 wait 전에 온
  요청을 놓치지 않습니다.
- **lag guard 동안 FIX POSITION 진단 등급**: `journal_current`가 0이면
  `provenance()`가 BETA를 거부하므로 치환이 없습니다. 이때 FIX 등급 POSITION
  행은 진단 등급으로 넣어 긴 저장장치 정지에서 오래된 것부터 버려지고 증거
  링을 채워 journal을 실패시키지 않습니다. full 프로파일은
  `Journal::observation_line`, persistent 프로파일은
  `PersistentLog::set_journal_current()`(매 호출 전 동기화)로 처리하며 RAW 창
  행도 기록되는 시점의 guard 상태를 따릅니다. LOST/NO_FIX, 등급 전환,
  CONTEXT_UNAVAILABLE 행은 계속 증거입니다. guard가 복구되면 즉시 증거로
  돌아갑니다.
- **분석기**: `journal_dropped` 구간은 이제 worker 시각 종류이면서 구간 시작
  이후인 행으로만 닫힙니다. POSITION/SEND(hook 시각), motion(생산자 시각),
  `raw_window` 표지 뒤 `rows`개 행, 시작보다 이른 행은 구간을 닫지 않습니다.
  digest `suppressed` 합계와 `journal_not_durable` 횟수는 보고서
  `lower_bounds`와 `suppressed_counts`/`profile_counts` = `lower_bound`, 요약
  문구 "at least"로 하한임을 표시합니다(해당 행이 진단 등급이라 버려질 수
  있고, 리셋·프로세스 사망 시 마지막 digest가 사라짐). `trial_status.awk`는 이
  값을 표시하지 않아 바꾸지 않았습니다.
- OEM 스레드 경로(adapter hook, `JournalQueue`, `provenance()`)는 바뀌지
  않았습니다. 바뀐 것은 worker·writer 스레드와 분석기뿐입니다.

### 정상 지연과 쓰기 횟수 (실시간 재생, 2026-10-08)

`build/writer_lag`(tests/runtime/writer_lag.cpp, `make test`에는 없음)는 `log_rate`의
10분 합성 주행(부팅 NO_FIX 45 s, 25 s GPS 끊김 1회)이 만든 journal을 실제 시간으로
제품 Journal과 writer 스레드에 다시 넣습니다. 10 ms마다 worker처럼 지연을 재고
1 s마다 flush를 요청하며 `journal_lag_guard`를 돌립니다. 쓰기 횟수는
`/proc/self/io`의 syscw/wchar입니다. 호스트 SSD에서 네 실행을 동시에 돌렸으며,
CMU eMMC 측정은 아닙니다.

| 프로파일 | flush | 지연 p50 / p99 / 최대 (ms) | fflush/s | write 호출/s | 바이트/write |
|---|---|---|---|---|---|
| full (30.8 KB/s) | 요청만(1 s) | 474 / 976 / 1001 | 0.99 | 7.97 | 3861 |
| full | 250 ms 자체 | 101 / 242 / 505 | 3.97 | 9.63 | 3194 |
| persistent (2.5 KB/s, 사건 포함) | 요청만(1 s) | 71 / 868 / 1006 | 0.99 | 1.39 | 1788 |
| persistent | 250 ms 자체 | 0 / 236 / 353 | 1.28 | 1.64 | 1523 |

어느 실행에서도 guard가 내려가지 않았습니다. full의 최대 505 ms는 p99.9(246 ms)
밖의 한 번이며 동시 실행의 호스트 지연으로 보입니다. persistent는 이론상 최대
초당 4회 쓰기이고 실제 1.64회입니다.

### 실행한 테스트 (호스트, 이 브랜치)

- `test_journal_ring`: held/unflushed 회계; 한 스레드에서 "검사 → 요청+notify →
  wait" 순서를 강제하면 조건 없는 wait는 50.1 ms 타임아웃까지 자고 조건 있는
  wait는 0.001 ms에 돌아옴; 검사와 wait 사이에 인위적 틈을 넣은 소비자 스레드에
  요청 3000건, 최악 0.23-2.21 ms.
- `test_journal --writer`: stdio에만 쓴 행의 지연 301 ms, flush 뒤 0 ms;
  wait 직전 300 ms 틈(`inject_wait_gap_ns`) 안에 넣은 flush 요청 199 ms,
  stop 201 ms에 처리(유실 시 1000 ms); 증거 링 512 B와 정지된 writer에서
  lag 중 FIX 98행은 실패 없이 진단 행 버림(`journal_dropped` 1개), current
  FIX와 lag 중 LOST는 예전처럼 journal 실패. `journal_lag_bound`는 worker
  루프처럼 1 s마다 flush를 요청하게 했습니다(stdio 행이 지연에 포함되므로).
  결과: 1510 ms에 철회, 2515 ms에 따라잡고 복구.
- `test_log_profile` lag_guard_classes: 직접/RAW 창 행, 복구, BETA 비활성 경우.
- `tests/tools/test_analyze_beta.py`: 이른 시각 행 네 종류와 RAW 창 행이 구간을
  닫지 않음, 나중 worker 행은 닫음, 하한 표시와 요약 문구. 두 새 시험은 이전
  분석기에서 실패함을 확인했습니다.
- 전체 호스트 `make -k test`(2026-10-08): 종료 0, PASS 309줄, make 오류 0,
  Python 746개 통과. packaging 340개 중 4개 생략(릴리즈 묶음
  `MX5DR_RELEASE_BUNDLE`/여섯 산출물 묶음 미빌드 — 이 작업은 ZIP을 만들지
  않음).
- ARM(고정 도구체인 `tools/build_arm.py`, `Verified ARM build`,
  `release_verified=true`): `tests/run_arm_all.sh` 종료 0, PASS 323/FAIL 0,
  SKIP 1(내장 AA probe는 private 입력 미설정으로 생략, 아래에서 별도 실행).
  QEMU에서 ring wait 50.2 ms 대 0.154 ms, wait 틈 flush 199.9 ms·stop 215.5 ms,
  lag 중 FIX 98행 실패 없음, lag bound 1501 ms 철회·4568 ms 복구(에뮬레이션에서
  backlog 쓰기가 느림). `tests/adapter/run_arm.sh` PASS 109, 실제 BLM+`libpatch`
  0.9.1 `run_aa_install_probe.sh` PASS 4. QEMU 결과는 차량 검증이 아닙니다.
- `mx5dr-guard` 해시는 beta.3와 같습니다(`248a3ef5…`).

### 250 ms 자체 flush 뒤 재실행 (2026-10-08, master `e3b9427` 병합 후)

- `test_journal --writer` writer_age_flush: 요청 없이 250 ms에 첫 자체 flush; 20 ms
  주기 정상 지연 최대 242 ms; `fflush` 한 번이 400 ms 걸려도 최대 583 ms이고
  `journal_current`는 내려가지 않음; 1.6 s 정지는 그 정지의 첫 행부터 약 1.33 s에
  내림(stdio에 남아 있던 더 오래된 행이 함께 세어짐), 정지 뒤 복구. 기존 lag bound
  시험(2.5 s 정지, 1505 ms 철회)은 바꾸지 않았습니다. 분석기와 verdict는 바뀌지
  않았습니다.
- 전체 호스트 `make -k test`: 종료 0, PASS 309줄, Python 763개 통과, packaging
  356개 중 4개 생략(위와 같은 릴리즈 묶음 미빌드 이유).
- ARM 새 빌드(`release_verified=true`): `run_arm_all.sh` PASS 323/FAIL 0/SKIP 1(내장
  AA probe, 별도 실행), QEMU writer_age_flush 정상 최대 243 ms·400 ms flush 626 ms·
  1.6 s 정지 1353 ms 철회; `run_arm.sh` PASS 109; `run_aa_install_probe.sh`
  (libpatch 0.9.1) PASS 4. QEMU·호스트 결과는 차량 검증이 아닙니다.
