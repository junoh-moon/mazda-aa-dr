# mazda-aa-dr

**AA = Android Auto, DR = Dead Reckoning(추측항법).** 2019 MX-5 ND2의 1세대 Mazda Connect **NA 74.00.324A**에서 차량 위치 전달과 터널 내 추측항법을 연구한다.

**현재 소스는 OBSERVE와 실제 센서 SHADOW 계산을 포함한 시험 후보**다. OFF 로딩, 폴링 프로세스 분리, 일회성 기동 보호를 구현하고 호스트·합성 ARM 검증을 통과했다. 차량·휴대폰 검증은 아직 수행하지 않았다. ASSIST는 차단되어 있으며 터널 내비게이션이 완성된 상태가 아니다.

**실차 기회가 한두 번뿐일 수 있다는 전제로 준비한다.** [통합 시험 계획](docs/FIELD_TRIAL_KO.md)에 따라 한 방문에서 주차 중 기동 확인, 자동 순정 관측·SHADOW·원본 수집, 종료·회수와 다음 부팅 복귀 확인을 묶는다. 새 후보에는 원본 진단 보존과 주차 중 상태/종료 도구를 추가했다. [검증 결과와 생략 항목](validation/FIELD_TRIAL_2026-09-29.md). 기존 `v0.2.0-observe.2`에는 이 기능이 없다.

[첫 시험 절차](docs/FIRST_TRIAL_KO.md) · [통합 검증](validation/INTEGRATION_2026-09-28.md). 영구 설정에는 우리 preload를 남기지 않으며 명시적으로 예약한 한 번의 부팅에만 적용한다. [실제 SM 실행 기록](validation/SM_RETRY_2026-09-29.md)은 명시적 서비스 재시작과 지연 종료 정책을 다룬다. 물리 watchdog과 실제 CMU 복구는 미검증이다. PR 병합 상태와 해당 브랜치의 구현 상태를 구분한다.

USB 설치 ZIP의 내용물을 FAT32 USB 최상위에 복사하면 MP3/JS로 진단 셸에
진입할 수 있다. 해당 USB 디렉터리에서 `sh install.sh`를 실행한다.
[사용 안내](packaging/USB_START_KO.md) · [설치 수정과 검증](validation/USB_INSTALL_2026-09-29.md).
현재 소스는 CMU의 BusyBox 1.19.2, `sha256sum` 부재, UID 0 계정 이름,
이중 저장소 심볼릭 링크와 읽기 전용 마운트를 처리한다.
이 수정을 포함한 [v0.3.1-shadow.1 설치 ZIP](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.1-shadow.1)을 게시했습니다.
공개 파일 재다운로드와 전체 host/ARM·순정 BusyBox 설치 검사는
[최종 릴리즈 검증](validation/RELEASE_2026-09-30.md)에 기록했습니다.
기존 `v0.3.0-shadow.1` ZIP은 이 수정을 포함하지 않습니다.

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
