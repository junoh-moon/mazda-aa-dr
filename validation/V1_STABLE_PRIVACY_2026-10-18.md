# v1.0.0 안정판 — 개인정보 사실 (2026-10-18)

범위: 1세대 Mazda Connect NA 74.00.324A용 `mazda-aa-dr` v1.0.0 후보 소스(작업 A 변경 적용 후). 이 기록은 **코드에서 읽은 사실만** 적습니다. 비공개 주행 자료·좌표·주소·블루투스 주소는 보지도 옮기지도 않았습니다.
표시: **FACT** = 코드에서 직접 확인(파일 경로 명시), **INFERENCE** = 해석.
다른 작업(문서 D 등)이 이 파일에 절을 더할 수 있습니다. 아래 절은 작업 A(제품 소스)가 쓴 것입니다.

## 1. 제품 소스가 CMU에 남기는 것 (작업 A, 코드 사실)

### 1.1 위치가 들어 있는 파일

모두 `/data_persist/mx5-aa-dr/` 아래이며 차량의 영구 저장소(eMMC)에 남습니다.

| 파일 | 위치 정보 | 정밀도 | 근거 (FACT) |
|---|---|---|---|
| `logs/trace.0..2.jsonl` (제품 기록, persistent 프로파일 16 MiB × 3, 찬 파일만 회전) | `position` 행의 `lat`/`lon`(순정 POSITION 입력), `send` 행의 `original_hex`/`outgoing_hex`(48바이트 LOCATION, 바이트 8..15가 위경도 1e-7도), `fix1` 행의 `d`(위경도 e7), `shadow`/`shadow_holdout` 행의 `lat`/`lon`(MODEL 계산값과 기준) | **전체 정밀도** (double `%.17g` 또는 1e-7도 정수) | `src/runtime/runtime.cpp`(position·send·shadow 행 서식), `src/runtime/drive_record_log.h`(fix1), `src/runtime/shadow_log.h`(shadow_holdout), 기본 설정 `packaging/common.sh`(설치기가 쓰는 `max_log_bytes=16777216`, `max_log_files=3`, `log_profile=persistent`) |
| `logs/collector.0..1.jsonl` (수집기, persistent에서 1 MiB × 2) | `position_poll` 행의 `lat`/`lon` (LDS `GetPosition` 1 Hz 폴링) | **소수 셋째 자리로 반올림**(남북 약 111 m, 동서는 위도에 따라 그보다 작음). 360을 넘는 값·NaN·무한대는 `null` | `src/collector/collector.cpp` `json_coordinate()`; 시험 `tests/collector/test_journal.cpp`, `tests/collector/test_collector.py` (`test_position_poll_coordinates_rounded_to_three_decimals`) |
| `logs/g3-heading.v1` (G3 정차 방위 저장, 한 줄) | 마지막 저장 시점의 위경도 e7 | 전체 정밀도(1e-7도) | `src/runtime/g3_heading.h` 기록 형식(`<lat_e7> <lon_e7>`) |

- **FACT**: 제품 기록(trace)의 좌표는 반올림하지 않습니다. 비공개 재생(`tools/replay_beta.cpp`)이 trace의 `position`/`send` 행을 입력으로 쓰기 때문입니다. 반올림은 수집기 행(`collector.*.jsonl`)에만 적용됩니다.
- **FACT**: persistent 프로파일에서 FIX 상태의 `position` 행과 ORIGINAL `send` 행은 평소에는 메모리 RAW 창(최근 60 s 이하)에만 있고, 사건(BETA 상태 변화, GPS 등급 전환, 고장, 캡처 정지 등) 때 앞뒤 구간이 기록됩니다. LOST/NO_FIX 등급의 `position`, ORIGINAL이 아닌 `send`(교체·덮어쓰기)는 항상 기록됩니다(`src/runtime/log_profile.h` 머리 주석). `fix1`은 주행 중 약 1 Hz입니다(기록 건강 조건과 속도 제한 있음).
- **INFERENCE**: 따라서 trace만으로도 주행 경로의 상당 부분(특히 터널·지하 구간과 사건 전후)이 전체 정밀도로 복원됩니다. 수집기 반올림은 1 Hz 연속 위치의 정밀도를 낮출 뿐, CMU에 남는 위치 이력 전체를 없애지는 않습니다.
- **FACT**: 수집기 행의 반올림은 `tools/replay_beta.cpp`의 "trace에 위치 행이 없는 오래된 기록이면 수집기 `position_poll`로 대체" 경로(그 경로는 `position_source: collector_position_poll`)와 `tools/map_aid_eval.py`(작업 A 소유 아님)의 수집기 고정점 사용에 영향을 줍니다. 앞으로의 기록에는 trace 위치 행이 있으므로 재생 입력은 trace입니다. 이미 내보낸 과거 기록은 바뀌지 않습니다.

### 1.2 위치 외 식별 정보

- **FACT**: 부팅 식별자(`/proc/sys/kernel/random/boot_id`, 무작위 UUID)가 trace·collector·`logs/g3-last-start`·`logs/g3-worker-boot`에 기록됩니다(`src/runtime/boot_id.h`, `src/collector/collector.cpp` `collector_boot` 행, `src/runtime/g3_heading.h`). 차량·사람 식별자가 아니라 CMU 부팅마다 바뀌는 값입니다.
- **FACT**: 수집기 `owner_poll` 행은 `com.jci.lds.data` 소유자의 D-Bus 고유 이름, PID, 프로세스 이름(`/proc/<pid>/comm`)을 적습니다(`src/collector/collector.cpp` `owner_log()`).
- **FACT**: GPS 기반 UTC(`utc_s`)와 CMU 단조 시계가 행마다 들어 있어 주행 날짜·시각이 복원됩니다.
- **FACT (블루투스)**: 제품 소스는 블루투스 장치 주소를 어디에도 쓰지 않습니다. 폰 IMU 수신기 행(`logs/pimu.0..1.jsonl`, `pimu_link`/`bt_watch` 등)은 서비스 번호·연결 상태 숫자·PTY 경로·D-Bus 오류 이름만 적고, BCA 신호의 장치 번호 인수도 기록하지 않습니다(`src/collector/phone_imu_rx.h`의 행 서식). 설치기가 만드는 `bt/BdsConfiguration.xml`은 순정 `/jci/bds/BdsConfiguration.xml`에 serialPort 한 줄을 더한 것이며, 순정 파일(펌웨어 템플릿)에는 장치 주소 형식의 문자열이 없습니다(읽기 전용 순정 루트에서 패턴 검색 0건). 폰 블루투스 채널은 v1.0.0에서 기본 끔입니다(`bt-off` 표식, 작업 B).
- **FACT**: 폰 앱(pimu-sender)의 진단 로그는 폰에만 있고 CMU 제품 소스와 무관합니다(감사 기록 1.5절).

## 2. 메뉴 3이 USB로 내보내는 것 (코드 사실, 포장 스크립트 읽기만)

- **FACT**: 메뉴 3(`packaging/trial` `finish_and_export`)은 `packaging/export_logs.sh`를 실행해 USB에 `mx5dr-logs-<날짜시각>-<pid>.tar`를 만듭니다. tar에는 **설치 디렉터리 전체**(`/data_persist/mx5-aa-dr`: 위 1절의 trace·collector·pimu·G3 파일, 설정, 표식, 바이너리)와 `usr/bin/autostart`, `jci/sm/sm.conf`, `jci/sm/sm_WCP.conf`, `jci/version.ini`, 진행 중인 시험 디렉터리, 그리고 순정 로그(`/var/log/running_log`, `/data_persist/log/error_logs`)의 파일당 마지막 128 KiB가 들어갑니다(`export_logs.sh`의 tar 인수와 `oem_log_directory`).
- **FACT**: 내보내기 스크립트는 끝에 "Keep it private."를 출력합니다.
- **INFERENCE**: 메뉴 3의 tar는 전체 정밀도의 위치 이력(trace, G3 저장)을 담습니다. 순정 로그 꼬리의 내용은 이 프로젝트가 정하지 않으며 무엇이 들어 있는지 확인하지 않았습니다. 공유할 때는 tar 자체가 아니라 `tools/analyze_logs.py`의 요약을 쓰고, 저장소에 원본을 올리지 않는 규칙(AGENTS.md)이 그대로 적용됩니다.

## 3. 남은 위험과 한계

- 수집기 반올림은 새 기록에만 적용됩니다. 이미 CMU에 있는 이전 빌드의 `collector.*.jsonl`(1 MiB × 2 회전)은 새 행으로 밀려날 때까지 전체 정밀도로 남습니다(INFERENCE: 1 Hz 기준 수 시간 주행이면 교체됨, 측정 안 함).
- trace의 위치 정밀도는 줄이지 않았습니다(재생 근거 유지가 우선). 위치 기록을 끄는 소유자 스위치는 없습니다.
- 이 기록은 차량·폰에서 확인한 것이 아니며, 내보낸 실제 tar를 열어 보지 않았습니다.
