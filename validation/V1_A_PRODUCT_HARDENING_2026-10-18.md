# v1.0.0 안정판 작업 A — 제품 소스 강화 (2026-10-18)

대상: 1세대 Mazda Connect NA 74.00.324A용 `mazda-aa-dr`, 기준 HEAD `6bbac15`. 근거: `V1_STABLE_AUDIT_2026-10-18.md`(감사), `V1_STABLE_DECISIONS_2026-10-18.md`(리드 결정, 작업 분담 A).
표시: **FACT** = 코드·실행 결과에서 직접 확인, **INFERENCE** = 해석·추정.
이 기록은 차량·폰·DHU를 쓰지 않았습니다. 호스트·정확한 ARM 빌드·QEMU·비공개 재생만 실행했으며, 실차 검증이 아닙니다. 릴리스·차량 방문·주행을 승인하거나 요청하지 않습니다. 표식(`g3-garage-off`, `bt-off`)의 기본값은 설치기(작업 B)가 만들며, 이 작업은 런타임·가드의 표식 의미를 바꾸지 않았습니다.

## 1. 바꾼 것

### 1.1 VIM 탭: 해석 실패 시에도 원본으로 통과 (`src/sensors/vim_tap.cpp`)

- 문제(FACT, 감사 1.1): `VIMC_AddClient` 래퍼는 원본 함수 해석이 실패하면 원본을 부르지 않고 102를 돌려 줬습니다. 순정 VIM 등록 결과를 우리가 지어내는 경로였습니다(실차에서 관측된 적 없음).
- 바꾼 동작(FACT): 원본 해석을 네 단계로 했습니다. (1) `dlsym(RTLD_NEXT)`, (2) `dlopen("/jci/lib/libjcivim_api.so", RTLD_NOLOAD)` 뒤 `dlsym`(기존 두 단계 그대로), (3) **호출자 자신의 링크 범위**: `__builtin_return_address(0)`를 `dladdr`로 호출 객체 경로로 바꿔 `RTLD_NOLOAD`로 연 핸들에서 `dlsym`(그 객체와 DT_NEEDED 의존성만 찾음, 새 매핑 없음), (4) 앞선 호출이 처음 찾은 원본(한 번 저장, 지우지 않음). 어느 단계든 탭 자신의 내보낸 함수는 "못 찾음"으로 셉니다. 찾은 원본은 같은 인수로 정확히 한 번 부르고 그 반환값·errno를 그대로 돌려줍니다.
- 남은 102 경로(FACT): 네 단계가 모두 실패하면(프로세스 어디에도 탭 아닌 `VIMC_AddClient`가 없고 앞서 찾은 적도 없음) 부를 원본이 존재하지 않으므로 102(VIM의 기존 일반 실패값)를 돌려줍니다. 순정 VBS 모듈 중 `libjcivim_api.so`를 DT_NEEDED로 가진 것은 `libjcimod_can.so`이며(읽기 전용 순정 루트의 `readelf -d`), 그 모듈이 호출하는 순간 라이브러리는 이미 적재되어 있습니다. **INFERENCE**: 따라서 (2)나 (3)이 실패하는 일은 정상 펌웨어에서 생기지 않으며, 102 경로는 "원본이 없는" 경우로만 좁혀졌습니다. 과제 문구의 "언제나 원본 통과"는 원본이 존재하는 모든 경우에 대해 성립합니다.
- 범위: 결정 기록의 "탭(한 줄 수정 제외) 동결"보다 큰 수정(약 40줄)입니다. 등록 경로의 해석 함수만 바뀌었고 콜백·관찰·측면 채널 코드는 그대로입니다. 시험 이음새(`MX5_VIM_TAP_TESTING`)로 각 단계를 대체합니다.
- 시험(FACT): `tests/sensors/test_vim_tap.cpp` 끝에 해석 실패 4경우(적재 라이브러리 단계로 통과, 호출자 범위 단계로 통과(전달된 호출자 주소가 0이 아님), 앞서 찾은 원본으로 통과, 아무것도 없으면 102·errno 보존)와 자기 함수 배제를 넣었습니다. 옛 코드(해석 실패 = 102)는 (1)~(3)에서 137 대신 102를 돌려 실패합니다(변이 V1~V6, 5절).

### 1.2 터널 교체 방위 정규화 (`src/adapter/adapter.cpp`, src/adapter 안의 유일한 변경)

- 문제(FACT, 감사 1.2 기타): `encode_beta_location`(터널 교체, choice 3)은 방위를 [0, 360)에서 받아 e6로 반올림하고 360000000만 0으로 바꿨으므로 정확히 0이나 0에 가까운 값을 보낼 수 있었습니다. G3 덮어쓰기(`encode_bearing_overlay`)는 (0.01, 359.99]만 보냅니다.
- 바꾼 동작(FACT): 이동 중(`!stopped`) 교체의 방위 e6가 `BEARING_OVERLAY_MIN_E6`(10000) 이하이거나 `BEARING_OVERLAY_MAX_E6`(359990000) 초과이면 359990000(359.99°)으로 보냅니다. G3 덮어쓰기와 같은 상수·같은 규칙입니다. [0, 360) 밖·비유한 방위의 거부와 정지 교체(바이트 40 = 0, 값 0)는 그대로입니다. 4줄 변경(주석 2줄 포함)이며 어댑터의 다른 코드는 건드리지 않았습니다.
- 시험(FACT): `tests/navigation/test_beta.cpp` `beta_location_bearing_normalised()` — 경계값 17개(0, 1e-9, 0.005, 0.0099994, 0.01, 0.0100004, 0.0100005, 0.010001, 1, 180, 359.98, 359.99, 359.9900004, 359.9900005, 359.995, 359.9999996, 359.99999999)의 바이트 44..47, 모든 결과가 (10000, 359990000] 안이고 float32로 0이나 360이 되지 않음, 바이트 0..7·24..31 불변, 거부 5개, 정지. 새 시험을 HEAD 어댑터와 함께 빌드하면 첫 경우(0 → 0)에서 실패합니다.
- 송신 영향(FACT): 비공개 재생의 교체 송신 299개는 방위가 7.887~354.775°라 바뀐 범위에 닿지 않았고, 재생 출력은 HEAD와 같습니다(4절). **INFERENCE**: 바뀌는 송신은 방위가 0.01° 이하이거나 359.99° 초과인 경우(원 둘레의 약 0.0056 %)뿐이며 그때 최대 0.02° 다르게 보냅니다.

### 1.3 수집기: 시간 제한 있는 서비스 버스 등록과 좌표 반올림 (`src/collector/*`)

- 문제(FACT, 감사 1.4·5절 9): 수집기 주 스레드의 `connect_bus()`가 `dbus_bus_register()`를 써서, 소켓은 받지만 SASL에 답하지 않는 데몬에서 무제한, Hello에 답하지 않으면 25 s를 기다렸습니다. 그동안 위치 폴링과 행 쓰기가 멈춥니다(제품 송신과는 무관).
- 바꾼 동작(FACT): 수신기의 제한 있는 등록(2 s SASL을 50 ms 단계로, Hello 1 s, 고유 이름 저장)을 새 헤더 `src/collector/bus_register.h`의 `bounded_bus_register()`로 옮겨 수신기(`phone_imu_rx.h`, PTY 배수·PING을 단계 사이에서 계속)와 주 스레드가 함께 씁니다. 수신기의 오류 이름·문구·순서는 그대로입니다(`mx5dr.AuthTimeout` "receiver stopping"/"no SASL answer within 2000 ms", `mx5dr.AuthFailed`, `mx5dr.HelloFailed`, Hello 시간 초과 시 libdbus NoReply). 주 스레드는 SIGTERM/SIGINT(`stop_requested`)가 오면 핸드셰이크를 끊습니다. 실패하면 그 표본은 `position_poll_error`(`unavailable`)를 쓰고 다음 표본이 다시 연결합니다.
- 좌표(FACT): `position_poll` 행의 `lat`/`lon`을 소수 셋째 자리로 반올림해 `%.3f`로 씁니다(`json_coordinate()`; 절댓값 360 초과·NaN·무한대는 `null`). 다른 숫자(`heading`, `kmh` 등)와 제품 trace(`trace.*.jsonl`)는 전체 정밀도 그대로입니다. 분석기(`tools/analyze_logs.py`)는 이 필드의 숫자형만 검사하므로 바꾸지 않았습니다(시험에서 `partial_record` 없음 확인). 개인정보 사실은 `V1_STABLE_PRIVACY_2026-10-18.md`에 적었습니다.
- 시험(FACT): `tests/collector/test_collector.py`에 4개 — 침묵 소켓(SASL 무응답)과 Hello 무응답 소켓에서 표본 3개가 각각 2.6 s/1.6 s 안에 끝나고 `position_poll_error` 3개·`collector_stop`(samples 3), 침묵 핸드셰이크 중 SIGTERM 뒤 1.5 s 안에 종료(reason `signal`), 비공개 dbus-daemon과 가짜 LDS(`tests/collector/fake_bca.c --lds`, 합성 값)로 `lat` 12.3456789 → 12.346, `lon` -45.6784999 → -45.678, 원래 자릿수 문자열 없음, 분석기 통과. `tests/collector/test_journal.cpp`에 반올림 경계 13개. 이 4개 파이썬 시험을 HEAD 수집기로 돌리면 4개 모두 실패(시간 초과 3, 반올림 1)합니다. 기존 수신기 시험(`test_phone_imu_rx.py`, 침묵/Hello 무응답 HMI 버스 포함)은 공유 헤더로 바꾼 뒤에도 통과했습니다.

### 1.4 메뉴 9 진단 분기 제거

- 바꾼 동작(FACT): `src/runtime/diag_arms.h`를 지웠습니다. `acc-cap-off` 표식 함수는 내용 그대로 새 `src/runtime/acc_cap.h`로 옮겼습니다(메뉴 10 동작과 25 m 상한 동작 불변). `BetaController`에서 진단 상태·`set_diag`·`diag_episode` 행·CONTROL 분기를 지웠고 발행 프로파일은 항상 `profile_`입니다. `runtime.cpp`는 `diag` 파일을 더 이상 읽지 않으며(`diag_boot` 행 없음, 오래된 `diag` 파일은 아무 행 없이 무시), `send` 행의 `diag` 필드 서식 코드를 지웠습니다(진단이 켜진 적 없는 부팅에서 빈 문자열이었으므로 행은 같음). `tools/replay_beta.cpp`에서 `--diag`/`--diag-out`과 합성 GPS_SATELLITE 송신을 지웠습니다.
- 남긴 것(FACT): 동결된 어댑터의 진단 관찰(`adapter::set_diag`, `Observation::diag_*`, `DiagArm` 이름, `adapter.h`의 `diag_arms.h`를 가리키는 주석)과 그에 기대는 `log_profile.h`·`drive_record_log.h`의 분기는 그대로입니다. v1.0.0의 어떤 호출자도 `set_diag(true)`를 부르지 않아 도달하지 않습니다(`log_profile.h` 주석에 적음). 분석기는 바꾸지 않아 옛 로그의 `diag_boot`/`diag_episode` 행을 계속 읽습니다.
- 시험(FACT): `tests/runtime/test_diag.cpp` → `tests/runtime/test_acc_cap.cpp`(acc-cap-off 경우 그대로 + 오래된 `diag` 파일은 끔 표식이 아님), `tests/runtime/test_diag_episode.cpp` 삭제. `tests/runtime/test_worker_beta.cpp`에 `budget_stale_diag` 시나리오(예전 켬 값 `on\n`인 `diag` 파일이 있는 budget 시나리오: trace에 `diag_boot`·`diag_episode`·`"diag":` 없음, 교체 송신 84개 모두 ≤ 25 m). 이 시나리오를 HEAD 런타임으로 빌드하면 `diag_boot` 검사에서 실패합니다. `tests/tools/test_analyze_diag.py`는 지운 C++ 고정물 대신 HEAD 고정물이 쓰던 `diag_episode` 행 8개(합성)를 상수로 담아 분석기 판독을 계속 확인합니다. `tests/replay/test_replay_beta.py`의 진단 분기 시험은 "제품 25 m 상한이 묶이고 `--acc-cap off`가 40 m를 되살림, 위치·속도·방위·정직 예산 동일"과 "`--diag`가 이제 거부됨"으로 바꿨고, `test_replay_drive_rows.py`는 `--diag` 변형 대신 `--acc-cap off` 변형으로 출력 동일성을 봅니다.

## 2. 소유 범위 밖 파일 (FACT)

과제가 지정한 목록 밖에서 고친 파일: `Makefile`(수집기 헤더 의존성, `test_diag`/`test_diag_episode` → `test_acc_cap`, `budget_stale_diag` 사례, `test-tools`의 고정물 변수 제거), `tests/tools/test_analyze_diag.py`, `tests/replay/test_replay_beta.py`, `tests/replay/test_replay_drive_rows.py`. 진단 분기 제거가 이 파일들의 빌드·시험을 깨기 때문입니다. `packaging/**`, `tests/packaging`, `src/guard/**`는 건드리지 않았습니다. `tests/packaging`의 메뉴 9 관련 시험(`test_trial_menu.py` 등)은 작업 B의 몫입니다.

## 3. 실행한 시험 (FACT)

- 호스트 `make -k test`(D-Bus 헤더·라이브러리는 지시된 스크래치 경로, `MX5DR_STOCK_ROOT` 읽기 전용): **종료 0**, 재시도 0회(`run_worker_beta_timing.sh` 재시도 줄 없음). unittest 합계 1,151건 통과, 건너뜀 17건(고정 ARM 컴파일러 미지정 6, 릴리스 번들 미지정 4, 비공개 자료 변수 미지정 4, 선택 도구 없음 3; 이 변경과 무관). 이 클론의 포장 파일은 HEAD 그대로이므로 `test-packaging`도 통과했습니다(작업 B 변경과 함께 돈 것은 아님). C++ 시험: `test_beta` 77,859 검사, `test_vim_tap`, `test_collector_journal`, `test_acc_cap`, `test_worker_beta` 전 사례(`budget_stale_diag` 포함) 통과. 1분 부하 3.3~6.7(다른 작업 병행).
- 그 뒤 `test_vim_tap.cpp`를 강화(1.1의 (1)(2) 앞에서 저장된 원본을 비움; 변이 V1이 살아남아서)했고 그 시험만 다시 돌려 통과했습니다.
- 정확한 ARM: 고정 도구체인(m3-toolchain `61ec034`, GCC 4.9.1)으로 `make arm` **종료 0, 경고 0**. `libmx5dr-vimtap.so`의 정의된 동적 기호는 `VIMC_AddClient`(와 `_init`/`_fini`)뿐, 새 가져오기는 `dladdr`(기존 `dlopen`/`dlsym`과 함께 libdl).
- ARM QEMU 시험: 6절.

## 4. 비공개 재생 (FACT)

20개 비공개 부팅(재생 가능 14, 위치 행 없음 6 — 이 6개는 모든 변형에서 같은 종료 2), 하나의 G3 저장 디렉터리로 연쇄(`--real-only --g3 on --g3-send on`), 저장 디렉터리를 뺀 110개 파일(report, csv, journal, g3 행, send 기록, stdout, stderr), 저장 스레드 시각 필드(`lat_ms`, `start_ns`, `end_ns`)만 정규화:

- 새 코드 대 HEAD `6bbac15`, 표식(`--g3-garage off`): **110/110 동일**, 부팅별 종료 코드 동일.
- 새 코드 대 HEAD, 표식 없음(`--g3-garage on`): **110/110 동일**.

비공개 자료와 결과물은 스크래치에만 있고 이 기록에 수치 외 내용은 옮기지 않았습니다.

## 5. 손 변이 (FACT)

각 변이는 별도 작업 사본에 한 곳만 바꿔 해당 시험을 빌드·실행했습니다. "잡힘" = 시험 실패.

| ID | 변이 | 시험 | 결과 |
|---|---|---|---|
| V1 | 적재 라이브러리 단계 끔 | test_vim_tap | 잡힘(강화 후; 강화 전에는 저장된 원본이 대신 통과시켜 살아남음) |
| V2 | 호출자 범위 단계 끔 | test_vim_tap | 잡힘 |
| V3 | 저장된 원본 대체 끔 | test_vim_tap | 잡힘 |
| V4 | 자기 함수 배제 끔 | test_vim_tap | 잡힘 |
| V5 | 호출자 주소 대신 0 전달 | test_vim_tap | 잡힘 |
| V6 | 최종 102 → 0 | test_vim_tap | 잡힘 |
| A1 | 방위 정규화를 옛 코드로 | test_beta | 잡힘 |
| A2 | 하한 `<=` → `<` | test_beta | 잡힘 |
| A3 | 대체값 359.99 → 0.01 | test_beta | 잡힘 |
| A4 | 상한 359.99 → 360 | test_beta | 잡힘 |
| C1 | 주 스레드 등록을 `dbus_bus_register`로 되돌림 | test_collector(침묵, Hello) | 잡힘 |
| C2 | SASL 한도 2 s → 20 s | test_collector(침묵) | 잡힘 |
| C3 | Hello 한도 1 s → 25 s | test_collector(Hello) | 잡힘 |
| C4 | 주 스레드 정지 확인 끔 | test_collector(SIGTERM) | 잡힘 |
| C5 | 수집기 행에 전체 정밀도 | test_collector(반올림) | 잡힘 |
| C6 | `%.4f` 원값 | test_collector_journal | 잡힘 |
| C7 | 360 초과 → null 검사 끔 | test_collector_journal | 잡힘 |
| C8 | 반올림 → 내림 | test_collector_journal | 잡힘 |
| D1 | acc-cap-off: 없음 아닌 오류를 "없음"으로 | test_acc_cap | 잡힘 |
| D2 | acc-cap-off 표식 이름을 `diag`로 | test_acc_cap | 잡힘 |
| D3 | 런타임이 `diag` 파일을 상한 끔으로 읽음 | test_worker_beta budget_stale_diag | 잡힘 |

변이 시험에서 수집기 파이썬 시험은 선택된 시험이 하나 이상 실제로 돈 경우만 "잡힘"으로 셌습니다(필터가 아무것도 고르지 못하면 살아남음으로 처리).

변이 21개 모두 잡힘(V1은 시험 강화 뒤). 별도로 새 시험을 옛 코드에 돌린 결과: `test_beta`의 방위 시험(HEAD 어댑터: 0 → 0에서 실패), 수집기 파이썬 시험 4개(HEAD 수집기: 4개 모두 실패), `test_worker_beta budget_stale_diag`(HEAD 런타임: `diag_boot` 검사에서 실패). VIM 탭 새 시험은 새 시험 이음새가 필요해 HEAD 소스로는 빌드되지 않으며, 옛 동작은 변이 V1~V3·V6로 대신 보였습니다.

## 6. ARM QEMU 결과 (FACT)

- 산출물: 3절의 `make arm` 산출물(`libmx5dr.so` 등 6개)을 `MX5DR_ARM_LIBRARY`로 지정. 1분 부하가 4 미만이 될 때까지 기다린 뒤 시작(시작 시 3.71).
- `tests/run_arm_all.sh` **종료 0**: `ARM_TIMING_RETRIES=0`, `ARM_TIMING_FAILED=0`, 시간 민감 시험 68회 모두 첫 시도 통과, `VIM tap registration routing, forwarding, ...`(1.1의 새 경우 포함한 `tests/sensors/test_vim_tap.cpp`)와 수집기 저널(`tests/collector/test_journal.cpp`, 반올림 경계 포함) 통과.
- `tests/adapter/run_arm.sh` **종료 0**(어댑터 사례 전부, `PASS` 줄 합계 340은 두 스크립트 합).
- 방위 경계: `tests/navigation/test_beta.cpp`는 GCC 4.9 libstdc++에 `std::remainder`가 없어(기존 시험 코드) ARM으로 빌드되지 않습니다. 그래서 새 함수 `beta_location_bearing_normalised()`만 그대로 떼어 어댑터 소스와 함께 고정 도구체인으로 빌드해 QEMU에서 돌렸고 91개 검사가 통과했습니다(softfp float32 판정 포함). 이 임시 구동 파일은 결과물에 넣지 않았습니다.
- AA 설치 프로브(`run_aa_install_probe.sh`)는 돌리지 않았습니다(어댑터 설치 경로를 바꾸지 않았고, 순정 루트 `/tmp/jcidlg.IUmV3A`는 쓰기 금지 대상이라 이 작업에서 쓰지 않음). 게이트 G4는 작업 C/F의 몫입니다.

## 7. 위험과 한계

- 실차·폰·DHU 미검증입니다. VIM 탭의 새 해석 단계 (3)(4)는 정상 펌웨어에서 실행되지 않는 경로라 QEMU·호스트 이음새로만 확인했습니다(INFERENCE: 1.1).
- VIM 탭 변경은 결정 기록의 "한 줄 수정" 범위를 넘습니다. 리드가 한 줄 범위를 고집하면 (3)을 빼고 (4)만 두는 축소가 가능합니다(그러면 첫 호출에서 (1)(2)가 모두 실패할 때 102).
- 수집기 `dbus_connection_open_private()` 자체(연결 단계)는 여전히 시간 제한이 없습니다. 유닉스 소켓에서 듣는 쪽이 백로그가 가득 찬 채 accept하지 않으면 막힐 수 있습니다(감사의 HMI 버스 잔여와 같은 종류, 이번에 고치지 않음).
- 침묵 버스에서 수집기 표본 간격은 약 1 s에서 약 3 s(2 s 핸드셰이크 + 표본 대기)로 늘어납니다. 제품 송신과 trace에는 영향이 없습니다.
- 좌표 반올림은 `tools/map_aid_eval.py`(작업 A 소유 아님)의 수집기 고정점과 `replay_beta`의 수집기 대체 입력 정밀도를 낮춥니다. 새 기록은 trace에 위치 행이 있어 재생 입력이 trace입니다.
- 동결된 어댑터에 진단 관찰 코드와 지워진 헤더를 가리키는 주석 한 줄이 남아 있습니다(도달 불가). 정리는 어댑터 동결이 풀릴 때의 일입니다.
- `tests/packaging`의 메뉴 9 시험과 설치기의 `diag` 표식 정리는 작업 B가 맞춰야 합니다. 이 클론에서는 B 변경 없이 돌렸습니다.
