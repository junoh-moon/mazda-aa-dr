# v0.3.8 이후 통합 SHADOW 소스의 원본 VM 관측 — 2026-10-01

이 기록은 [이전 세대 콜백 경계 수정](ASSIST_STALE_CALLBACK_2026-10-01.md)을
공개 v0.3.8 소스와 병합한 **로컬 시험 후보**의 추가 검증입니다. 공개
`v0.3.8-shadow.1` ZIP을 바꾸거나 차량에 설치한 기록이 아닙니다.
대상 소스 `d3c6fa1e81fa16046ebbd1f200c22ffb3c3eda6a`로 만든 ZIP의
`build-info.json`은 `source_modified=false`, 기본 모드 `SHADOW`, 고정
도구체인 `61ec0343de84f6fc7c46840056df1d600d44be8a`를 기록합니다.
ZIP SHA-256은 `4560926ace8b60187db448d9c5f8667fd5ad25b5f5896c69dc3ee43621d4ec31`,
제품 `libmx5dr.so`는 `4ef64da1fae9c18c975833495c69fda9a9b41ceaf0c3d78e07fc9eeeb911b441`입니다.
VM 이미지의 bundle manifest에도 동일한 제품 해시가 있습니다.

## 실행 경계

원본 NA 74.00.324A rootfs·커널에 시험 ZIP을 넣고
`tests/packaging/oem_system_emulation.py`의 격리된 `cmu`/`shadow`/`location`
경로로 실행했습니다. 원본 커널을 변경하지 않았고, 가상 보드의 machine ID는
진단 runner가 부팅 레지스터에서 조정했습니다. **진단용 PID 1**이 원본
userspace 서비스를 개별 시작했습니다. 순정 init 전체 부팅, 물리 CMU 장치,
GPS/CAN 입력, 호스트 네트워크·장치·공유 디렉터리는 없었습니다. VM에서
계정 이름·UID/GID는 보존하고 credential 필드는 대체했습니다. 이 이미지와
원본 console·journal은 비공개 `evidence/`에만 보존합니다.

VM 안에서 `(cd /validation/usb && sh install.sh)`가 성공 메시지를 냈고
일회성 guard 선택은 `RC=0`이었습니다. collector는 UID/GID 1001로 시작했고
협조 종료 요청 `RC=0` 뒤 `collector_stop` 기록이 남았습니다. 원본
VBS·AA·LDS·navi launcher가 시작됐으며 AA 프로세스에 위 제품 DSO,
VBS 프로세스에 같은 ZIP의 VIM tap DSO가 매핑됐습니다. 원본 LDS 위치·상태
DBus 질의는 provider 등장 전 `RC=1`, 등장 후와 뒤의 재질의는 `RC=0`이었습니다.
이는 응답 경로의 존재만 확인합니다. 실제 GPS 값이나 새 측정의 근거가 아닙니다.

제품 trace 460행의 `boot`는 `install=ok`, `assist_ready=false`였고,
`shadow_boot`는 MODEL 영역의 관측 활성화를 기록했습니다. health 46행은
모두 `hook_installed=true`, `dropped=0`, `audit_fault=0`,
`computation_active=false`, `assist_ready=false`였습니다. collector 66행에는
위치 poll 성공 22건이 모두 `mode=0`으로 남았고 위치 poll 오류 5건,
일반 poll 27건, 정상 `collector_stop` 1건이 있었습니다. 수신기 관측 5건도
모두 0이었습니다. 원본 위치 payload 완전 쌍, 유효 GPS 위치, motion,
live AA session은 없었습니다. holdout은 `bus_reset`으로 ABORT했고 비교는
0건입니다. 이 조건에서는 계산이 꺼져 있는 결과를 결함이나 위치 정확도
증거로 해석할 수 없습니다.

현재 `tools/analyze_logs.py --json` 결과는 종료 **2**, `status=inconclusive`입니다.
사유는 session·bus 관측 불가, `holdout_aborted`, `shadow_bus_reset`,
`no_location_samples` 다섯 가지입니다. `install_counts.ok=1`은 기록된
제품 boot의 설치 상태이고 차량 설치 성공 판정이 아닙니다. VM runner는
240초 제한으로 종료됐고, 진단 shell 도달을 확인했지만
`timed_out=true`, `verdict=observation_only`, `timeout_is_success=false`입니다.
QEMU 자체의 종료 코드 0도 PASS로 취급하지 않습니다.

| 비공개 증거 | SHA-256 |
| --- | --- |
| 진단 initramfs | `b725d59d6a248213a98f91246771d4a8831821fe09cffabd25e3d9460b7fa556` |
| VM console | `7d0b8b5ca413f80767bd612afe19277c50de97cf26fc8600eae779c2a5651a50` |
| 분석 JSON | `f3295a582e9d239fca8540914877fece53b53c3e978ee92c5a0f9eae70be1629` |

기존 [통합 host·ARM 검사](ASSIST_STALE_CALLBACK_2026-10-01.md)는 이 VM과
별개의 합성·원본 runtime 검사입니다. 이번 VM은 새 ASSIST 경계의 qualified
GPS·센서 입력을 실행하지 않았고 정상 전체 차량 기동·한 번의 실제 AA
session·원본 LOCATION 송신·폰/지도 앱 수용·물리 복구를 확인하지 않았습니다.
live ASSIST는 계속 비활성입니다.
