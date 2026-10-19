# v1 시험 강화: 손 변이로 살아남던 안전 장치 고정 (2026-10-19)

대상: master `a7756da`. 2차 독립 검토가 "장치를 지워도 실패하는 시험이 없다"고 지적한 여섯 장치(M4, M17, M11, M16, M18, P3)에 대해 시험만 추가·보강했습니다. **제품 코드(src/, packaging/)는 바꾸지 않았습니다.** 모든 빌드·실행·변이는 스크래치 `git clone` 사본에서만 했고, 변이는 하나씩 적용해 시험한 뒤 `git checkout`으로 되돌렸습니다. 차량·폰·DHU는 쓰지 않았습니다. 표시: **FACT**(실행·코드에서 직접 확인), **INFERENCE**(해석).

## 바뀐 시험 파일

- `tests/adapter/adapter_test.cpp` (`beta_hold`): `before_fail`을 시험 시작 시의 `generation()`이 아니라 싱크 안에서 실제로 게시된 후보의 세대(`position_enter`의 모드 −1→0 전환 폐기 **뒤**의 값)로 잡습니다. 이제 `generation()>before_fail`은 보류 경로의 `invalidate()`만 만족시킬 수 있습니다.
- `tests/runtime/test_worker_beta.cpp` (g3_send, g3_send_conflict, g3_send_fence, g3_send_abort 공통): 창이 열린 뒤 첫 FIX 호출의 송신은 덧씌우지 않고(순정), 첫 덧씌운 호출은 그보다 뒤이며, 종료 행의 `stock`이 1 이상임을 단언합니다.
- `tests/runtime/log_rate.cpp`: `--probe generation|silence|silence_arg|wheel_gap` 추가(모의 시계, 제품 어댑터·Pipeline·BetaController 그대로). 옵션이 없으면 기존 실행과 같습니다(`oem_position`이 교체 여부를 돌려주는 것 외 동작 변화 없음).
- `tests/journal/test_beta_gates.py`(새 파일): 위 네 탐침을 실행해 결과 행을 단언합니다. 기존 `make test-motion-journal`의 `unittest discover -s tests/journal`이 자동으로 찾습니다(Makefile 변경 없음).
- `tests/recovery/test_guard.py` (`Editor.test_branch_execution_and_fallback`): 상태 `failed_path` 추가. 가드 대역이 올바른 형태의 시험 경로(`/tmp/mx5dr-trial-ABC123/sm.conf`)를 출력하고 **0이 아닌 코드로 끝나면** SM은 순정 설정을 받아야 합니다. timeout 있음/없음 두 분기와 두 보드 모두.

## 변이 표 (FACT)

| ID | 변이(단독 적용) | 이전 시험 | 새 시험 결과(변이) | 실제 코드 |
|---|---|---|---|---|
| M4 | `adapter.cpp` 보류 경로의 `invalidate();` 주석 처리 | `beta_hold` 통과(생존). 단 기존 `beta_overlay`(507행 `generation()>g`)는 이미 실패 | `beta_hold` 635행 `generation()>before_fail` **실패**, `beta_overlay`도 실패 | 25개 사례 통과 |
| M17 | `g3_send.h` `publish`의 `last_generation_!=generation ||` 삭제 | 4개 g3_send 시나리오 통과(생존). 덧씌움 16 대 15, `stock` 0 대 1 | 4개 시나리오 모두 838행(첫 FIX 호출 덧씌움) **실패** | 4개 통과 |
| M11 | `beta_controller.h` `publish`의 `position_generation_!=generation` 항 삭제 | `test_worker_beta` main, contradict, silence, budget, fault, cadence_gap, withdrawn_class 통과(생존) | `log_rate --probe generation` **실패**(rc 3, `replaced_after_unprocessed_return`) | `stock` |
| M16 | `pipeline.cpp:1845` `now-e.time>beta_.lease_ns` 삭제 | **이미 잡힘**: `test_beta` 1468행 `!p.speed_publication(T(3000)+600 ms).ok` 실패(rc 1) | `log_rate --probe wheel_gap` **실패**(`still_engaged`, 650 ms) — 제어기 경계에서도 고정 | `speed_unavailable`, 550 ms |
| M18 | `beta_controller.h` `publish`의 300 ms 수신 침묵 조건 삭제(`(void)last_motion_ns;`로 경고만 피함) | `test_worker_beta` silence, main 통과(생존) | `--probe silence_arg` **실패**(`published`). `--probe silence`는 통과(아래) | 둘 다 `sensor_silence` |
| P3 | `edit_autostart.awk` 11행(timeout 분기) 또는 13행(else 분기)의 `|| MX5DR_TRIAL=''` 삭제 | `Editor` 5개 통과(생존: 기존 `failed` 상태는 `garbage`를 출력해 case 패턴에 원래 안 맞음) | 11행 변이: `failed_path` × 두 보드, timeout 있음 **실패**. 13행 변이: timeout 없음 **실패** | `Editor` 5개 통과 |

## 장치별 해석

- **M18과 실제 침묵 (FACT + INFERENCE)**: 모든 운동 입력(바퀴·요)을 2 s 끊는 `silence` 탐침은 실제 코드와 M18 변이 모두에서 같은 회차(트리거 뒤 350 ms)에 `sensor_silence`로 철회됩니다. 변이에서는 파이프라인의 `PIPELINE_MISSING_SENSOR`(MODEL 표본 나이 250 ms) 경로가 같은 이유 문자열을 내기 때문입니다. 즉 이 합성 주행에서는 두 경로가 같은 50 ms 회차에 걸리며, 300 ms 수신 침묵 조건은 그 상황에서 관측상 중복입니다. `silence` 탐침은 이유와 350 ms 상한을 고정하고, `silence_arg` 탐침은 tick 계약(파이프라인 입력은 계속 흐르는데 `last_motion_ns`만 299 ms → 유지, 301 ms → `sensor_silence` 철회)을 직접 고정해 변이를 잡습니다. 수신 시각과 측정 시각이 갈라지는 실제 상황(수신 중단은 있으나 프레임 앞단은 새로운 경우)에서 300 ms 조건이 유일한 방어가 되는지는 이 기록에서 시험하지 않았습니다(INFERENCE: 그런 경우에만 독립적으로 의미가 있습니다).
- **M11 (FACT)**: `generation` 탐침은 ENGAGED 상태(합성 터널, 205.7 s)에서 워커 회차 사이에 OEM이 GPS 복귀(FIX)와 새 상실(LOST)을 보고하게 하고(세대 +2 확인), 워커가 이를 꺼내기 전에 BETA tick을 돌린 다음 LOST 호출 하나를 더 합니다. 그 호출은 어댑터의 호출별 분류(LOST)·세대 검사를 통과하므로 변이에서는 현재 세대로 게시된 후보가 그대로 덧씌워집니다. 실제 코드는 `generation_unobserved`로 게시를 미루고 순정이 나갑니다.
- **M16 (FACT)**: 검토의 "생존" 판정과 달리 같은 줄 변이를 `tests/navigation/test_beta.cpp` 1468행이 이미 잡았습니다(rc 1). 그래도 요청대로 제어기 경계 시험(`wheel_gap`: NO_FIX 속도 덧씌움 중 바퀴만 2 s 끊고 요는 계속, 300 ms 수신 침묵에 걸리지 않음)을 더했고 이 변이를 잡습니다. 어댑터의 `valid_until` 임대가 없어도 제어기는 500 ms + 한 회차 안에 SPEED_ENGAGED를 떠나야 한다는 것을 상태 전이로 단언합니다.
- **M4 (FACT)**: 기존 `beta_overlay`가 이미 이 변이를 잡았습니다(검토의 "어떤 시험도 실패하지 않는다"와 다름. 검토가 다른 형태의 변이를 썼을 가능성이 있습니다 — INFERENCE). `beta_hold`도 이제 독립적으로 잡습니다. `beta_bearing`은 이 변이에서 통과합니다(클래스 전환이 세대를 올리기 때문).
- **M17 시간 의존 (INFERENCE)**: 새 단언은 "첫 FIX 호출의 POSITION 관측을 워커가 처리하고 게시하는 일이 같은 호출의 송신보다 늦다"에 기대지 않습니다 — 실제 코드는 그 호출의 POSITION을 워커가 처리하기 전에는 그 세대의 후보를 게시하지 않으므로, 순서상 같은 호출의 송신(enter 직후 바로 실행)이 앞섭니다. 다만 워커가 `position_enter`와 송신 사이 수 μs 안에 관측을 꺼내 게시하면 이론상 덧씌울 수 있습니다. 호스트 스위트 1회 + 별도 5회, ARM 1회(실제 코드)에서 실패는 없었습니다.

## 실행 (FACT)

- 호스트(`nice -n 10`, `-j1`, `HOST_DBUS_*` 지정, `MX5DR_STOCK_ROOT` 읽기 전용): `make -k test-adapter test-runtime test-navigation test-recovery test-motion-journal` 종료 코드 0. 새 `test_beta_gates` 4개 통과, g3_send 계열 단언 포함 `test_worker_beta` 전 시나리오(g3_garage·g3_cancel 시간 시험 포함)와 `g3_send_identity.py` 통과, `test_guard.py` `Editor` 통과. 건너뛴 시험 없음(로그에 SKIP 없음).
- M17 시간 의존 확인: 실제 코드에서 `test_worker_beta g3_send`를 추가로 5회 반복, 5회 모두 통과.
- ARM: `tests/runtime/test_worker_beta.cpp`는 ARM 스위트(`worker-beta-test`)에 들어 있으므로 고정 도구 사슬(GCC 4.9.1)로 그 파일만 다시 컴파일해 qemu-arm으로 g3_send 4개 시나리오를 돌렸습니다(결과 아래). `adapter_test.cpp`, `log_rate.cpp`, `test_guard.py`, `test_beta_gates.py`는 ARM 스위트에 없습니다.

ARM(qemu-arm, 고정 GCC 4.9.1, `run_arm_all.sh`와 같은 플래그로 `worker-beta-test`만 컴파일): g3_send 15/12, g3_send_conflict 19/8, g3_send_fence 3/16, g3_send_abort 5/22(창 종료 전 덧씌움/종료 뒤 호출) 모두 rc 0. 전체 ARM 스위트는 돌리지 않았습니다.

## 발견한 제품 결함

없습니다. 제품 코드는 고치지 않았습니다.

## 한계

- 모두 합성·모의 시계·QEMU 시험입니다. 차량 검증이 아닙니다.
- M18의 300 ms 조건이 유일한 방어가 되는 수신/측정 시각 분리 상황은 재현하지 않았습니다.
- 각 변이는 단독으로만 적용했습니다(조합 변이 없음).
