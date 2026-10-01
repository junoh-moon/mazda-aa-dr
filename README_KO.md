# mazda-aa-dr

**AA = Android Auto, DR = Dead Reckoning(추측항법).** 2019 MX-5 ND2의 1세대 Mazda Connect **NA 74.00.324A**에서 차량 위치 전달과 터널 내 추측항법을 연구한다.

**현재 소스는 OBSERVE와 실제 센서 SHADOW 계산을 포함한 시험 후보**입니다. OFF 로딩, 폴링 프로세스 분리, 일회성 기동 보호를 구현했습니다. 실차 시험을 수행했지만 회수 archive에 수집 기록이 없어 기동·저장 경로를 조사 중입니다. 센서 동작과 휴대폰 수용은 입증되지 않았습니다. ASSIST는 비활성이며 터널 내비게이션이 완성된 상태가 아닙니다. [빈 기록 조사](validation/EMPTY_CAPTURE_2026-10-02.md)를 참조하십시오.

**2026-10-01에 사용자가 v1.0 전 실차 설치·시험 기회를 한 번 허용했습니다.**
가능한 펌웨어·오프라인 검증을 먼저 마치고 [한 번의 통합 시험](docs/FIELD_TRIAL_KO.md)을
준비합니다. 이전 펌웨어 파일만 사용하라는 제한을 해당 기회에 한해 갱신하며,
반복 방문이나 폰/동글 탁상 시험의 추가 승인이 아닙니다.
[완료 조건](docs/V1_READINESS_KO.md)을 유지하며 물리 센서·폰/앱 수용·복구는
아직 미검증입니다.

[과거 첫 시험 절차](docs/FIRST_TRIAL_KO.md) · [통합 검증](validation/INTEGRATION_2026-09-28.md). 영구 설정에는 우리 preload를 남기지 않으며 명시적으로 예약한 한 번의 부팅에만 적용한다. [실제 SM 실행 기록](validation/SM_RETRY_2026-09-29.md)은 명시적 서비스 재시작과 지연 종료 정책을 다룬다. 물리 watchdog과 실제 CMU 복구는 미검증이다. PR 병합 상태와 해당 브랜치의 구현 상태를 구분한다.

현재 공개판은 [v0.3.9-shadow.3 설치 ZIP](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.9-shadow.3)입니다.
순정 CMU 재부팅 요청과 전후 boot ID 대조, 전체 설치 폴더·기동 진단의
USB 회수를 추가했습니다. ACC·엔진이 꺼진 ON·실제 엔진 가동을 구분한
절차와 [새 발행 검증](validation/RELEASE_V039_SHADOW3_2026-10-02.md)을 확인하십시오.
원본 AA 연결 등록에서 서버 주소 GUID와 client 고유 이름을 소유 복사하고,
실제 raw 송신이 같은 연결을 사용했는지 기록합니다. 식별자가 없어도 원시 위치를
보존하며, 생산자 측정 시각이나 센서 자격으로 인정하지 않습니다.
원본 위치 아홉 필드와 GPS holdout의 호출·generation 연결, 누락·중복 진단,
기존 MODEL 비교를 유지합니다. 기준점·콜백 및 큐 폐기 경계 수정도 포함합니다.
실제 자격 입력 공급부와 LDS producer의 프로세스 간 연결은 미완료이므로
**ASSIST는 계속 비활성**입니다.

**v0.3.9-shadow.1의 회수 실패 복구:** 핫픽스로 USB 파일을 교체하고 아래
한 줄과 `3`만 사용하십시오. 이미 제거했어도 남은 로그를 회수하며 재설치·
재주행은 필요하지 않습니다. 정상 `/proc/mounts` 링크를 거부한 결함과
[핫픽스 검증](validation/TRIAL_EXPORT_HOTFIX_2026-10-02.md)을 확인하십시오.
실제 후속 회수는 성공했지만 trace·collector 기록은 없었습니다.
[빈 기록 분석](validation/FIELD_V039_EMPTY_CAPTURE_2026-10-02.md)을 확인하십시오.
사용자는 설치 후 차량 점화 OFF/ON과 무선 AA 동글·S25를 이용한 주행을
확인했으며, 실제 CMU Linux 새 부팅은 확인하지 못했습니다. 공개 `.3`은
guard 표식 진단, 메뉴 `5`의 순정 CMU 재부팅 요청과 확대 회수를 포함합니다.
재부팅 요청 수락과 실제 새 부팅 여부는 별도로 확인합니다.

[이전 릴리즈 검증](validation/RELEASE_V039_2026-10-01.md)에 고정 소스·ZIP 해시,
최종 host/ARM·순정 BusyBox 검사와 공개 재다운로드 대조를 기록했습니다.
선행 원본 라이브러리 실행과 최종판의 검증 범위도 구분합니다.
[v0.3.8](validation/RELEASE_V038_2026-10-01.md)과
[v0.3.7](validation/RELEASE_V037_2026-10-01.md)의 검증 이력은 별도로 보존합니다.

ZIP의 내용물을 FAT32 USB 최상위에 복사하십시오. **주차 중 기존에 작동하는
진단 셸을 열고**, 다음 한 줄로 숫자 메뉴에 들어가 최초 설치에만 `1`과 Enter를 누르십시오.
필요하면 USB 경로의 글자만 바꾸십시오. Shift 입력은 필요하지 않습니다.

```sh
sh /tmp/mnt/sda1/trial
```

별도로 허용된 다음 시험에서는 [상세 설치 안내](packaging/USB_START_KO.md)에
따라 중립·주차브레이크·실제 엔진 가동을 유지하고 `1` 설치 뒤 `5`로 CMU
재부팅을 요청하십시오. 시동 버튼을 누르거나 USB를 빼지 마십시오.
화면 복귀 후 USB·셸로 돌아와 `2`에서 새 boot ID, 현재 부팅의 가드 소비와
collector 기록을 확인하십시오. `0`으로 종료한 뒤 추가 시동·CMU 재부팅 없이
USB를 AA/동글로 교체하십시오.
AA 사용 중 셸을 열 필요는 없습니다. 시험 후
주차한 다음 AA를 분리하고 USB·셸로 돌아와 같은 메뉴의 **`3`으로 바로 회수**하십시오.
셸 복귀에 정상 재부팅이 필요해도 재설치·재무장 없이 이전 부팅의 보존 파일을
회수합니다. `finish_exit`가 0이 아니면 현재 부팅의 완료 미확인이고,
`export_exit=0`은 확보 가능한 파일의 회수 성공이며 시험 성공이 아닙니다.
archive·체크섬·`trial-result.txt`를 함께
가져오십시오. 운전 중에는 명령을 입력하거나 USB를 바꾸지 마십시오.
[사용 안내](packaging/USB_START_KO.md)와 [통합 시험 절차](docs/FIELD_TRIAL_KO.md)를
따르십시오.

현재 묶음은 BusyBox 1.19.2, `sha256sum` 부재, UID 0 계정 이름,
이중 저장소 심볼릭 링크와 읽기 전용 마운트를 처리합니다.
[이전 설치 수정·검증](validation/USB_INSTALL_2026-09-29.md)은 별도 이력입니다.
실제 차량에서의 수집·계산과 ASSIST 적용은 아직 미검증입니다.

**master의 추가 변경(PR #12):** 실제 VIM 센서 콜백 → 시간 정렬 → DR 코어 → LOCATION 미리보기의 SHADOW 계산을 구현했다. [기능·계약·남은 조건](docs/LIVE_SHADOW_2026-09-29_KO.md). 배포된 `v0.2.0-observe.2`에는 이 기능이 없으며 live ASSIST는 계속 차단된다.

**후속 SHADOW 기능:** [정차 자이로 영점 보정과 GPS 비교](docs/SHADOW_CALIBRATION_KO.md)를 추가했다. 보정값을 실제 MODEL 계산에 적용하고, 별도 10초 계산에서는 GPS를 비교 기준으로만 사용한다. 기존 설치 기본값과 ASSIST 차단은 유지한다.

## 문서 읽는 순서

후속 [GPS 기준점 검사와 휠 거리 보정](docs/GPS_WHEEL_MODEL_KO.md)은 GPS와 차량 입력의 불일치를 검사하고, 충분한 전진 직진 구간에서만 ±5% 이내의 MODEL 보정을 적용한다.

1. [현재 상태와 인계](docs/STATUS_KO.md): 목표, 증거 수준, 구현 범위, 남은 일.
2. [2026-09-28 리뷰와 해결 조건](docs/REVIEW_2026-09-28_KO.md): 실제 결함과 가설의 구분.
3. [구현 설명](docs/IMPLEMENTATION_REVIEW_KO.md) 및 `src/`: 현재 코드의 데이터 계약.
4. [과거 상세 설계](docs/archive/DESIGN_V1_KO.md): 설계 배경. 현재 상태 문서가 우선한다.
5. [검증 기록](docs/VALIDATION.md), [공개 이관 검증](validation/PUBLIC_IMPORT.md), [의사결정 기록](docs/DECISIONS_KO.md).
6. [릴리즈 생성 절차](docs/RELEASING_KO.md): 커밋 고정, 빌드·검증, 설치 ZIP·체크섬, 태그·게시·다운로드 재검증.
7. [SHADOW 로그 형식](docs/COMPACT_SHADOW_LOGS_KO.md): 원본 필드를 보존하는 배치 기록, PC 복원, 기록량과 보존 한계.

## 구현 상태

| 구성 | 현재 상태 |
| --- | --- |
| OBSERVE | 일회성 주차 시험 패키지 준비. 실제 차량 실행은 미검증 |
| SCRUB | 원래 mode=0인 캐시 LOCATION의 speed/bearing 유무 필드 제거. 좌표는 그대로이며 개선·악화 모두 미검증 |
| SHADOW | master에서 실제 센서 입력과 MODEL 계산을 연결. 위치·속도·방향·LOCATION 미리보기 기록, 순정 송신 유지. 실차 미검증 |
| ASSIST | 비활성. 설정 변경만으로 켤 수 없음 |
| 공통 DR 코어 | SHADOW 모델과 검증된 입력이 같은 적분 코드를 사용. MODEL 출력을 ASSIST로 승격하지 않음 |
| DROP | 비교 실험 후보. 미구현이며 폰 fallback 성공을 보장하지 않음 |

네이버 지도가 우선 대상이다. TMAP·카카오맵·카카오내비는 각각 별도의 검증 대상이며, 다른 차량의 후기를 이 차량에서의 지원 확인으로 취급하지 않는다. 모든 차량 조작은 주차 중에 하고, 주행 중 CMU 제어를 요구하지 않는다.

## 개발과 공개 범위

Linux 호스트에 C/C++ 컴파일러, make, Python 3, Git, pkg-config, D-Bus 개발 파일을 준비한 뒤 `make test`를 실행한다. 원본 펌웨어가 필요한 설치 fixture는 공개하지 않으며, 없으면 해당 시험은 skip으로 표시된다. 상세 방법은 [CONTRIBUTING.md](CONTRIBUTING.md)와 [ARM 도구체인](docs/toolchain.md)에 있다.

소스·설계·검증 기록을 공개한다. 원본 펌웨어·라이브러리·지도·디스어셈블 덤프·개인 공유 링크·실차 이동 로그·빌드된 설치 묶음은 포함하지 않는다. 과거 설치 문서는 검토용 기록이며 현재 설치 안내가 아니다. 프로젝트 라이선스는 아직 선택하지 않았다.
