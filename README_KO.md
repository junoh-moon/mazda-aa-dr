# mazda-aa-dr

**AA = Android Auto, DR = Dead Reckoning(추측항법).** 2019 MX-5 ND2의 1세대 Mazda Connect **NA 74.00.324A**에서 차량 위치 전달과 터널 내 추측항법을 연구한다.

**현재 통합 브랜치는 주차 상태의 첫 OBSERVE 시험을 위한 0.2 후보**다. OFF 로딩, 폴링 프로세스 분리, 일회성 기동 보호를 구현하고 호스트·합성 ARM 검증을 통과했다. 차량·휴대폰 검증은 아직 수행하지 않았다. ASSIST는 차단되어 있으며 터널 내비게이션이 완성된 상태가 아니다.

[첫 시험 절차](docs/FIRST_TRIAL_KO.md) · [통합 검증](validation/INTEGRATION_2026-09-28.md). 영구 설정에는 우리 preload를 남기지 않으며 명시적으로 예약한 한 번의 부팅에만 적용한다. 같은 실행 중인 SM의 재시도와 실제 CMU 복구는 미검증이다. PR 병합 상태와 해당 브랜치의 구현 상태를 구분한다.

이 브랜치의 설치 묶음은 CMU root 셸에서 압축을 푼 뒤 `sh ./install.sh` 한 줄로
OBSERVE 설치·검사·일회성 예약을 수행한다. [한 줄 설치 설명](docs/ONE_COMMAND_INSTALL_KO.md).
이미 배포한 `v0.2.0-observe.1` ZIP에는 아직 이 변경이 없으며, 그 ZIP의 명령은
`sh ./install.sh --remount`다. 설치 성공 후 정상적인 전원 종료·다음 기동이 필요하다.

**이 Draft 브랜치의 추가 변경:** 실제 VIM 센서 콜백 → 시간 정렬 → DR 코어 → LOCATION 미리보기의 SHADOW 계산을 구현했다. [기능·계약·남은 조건](docs/LIVE_SHADOW_2026-09-29_KO.md). 배포된 `v0.2.0-observe.2`에는 이 기능이 없으며 live ASSIST는 계속 차단된다.

## 문서 읽는 순서

1. [현재 상태와 인계](docs/STATUS_KO.md): 목표, 증거 수준, 구현 범위, 남은 일.
2. [2026-09-28 리뷰와 해결 조건](docs/REVIEW_2026-09-28_KO.md): 실제 결함과 가설의 구분.
3. [구현 설명](docs/IMPLEMENTATION_REVIEW_KO.md) 및 `src/`: 현재 코드의 데이터 계약.
4. [과거 상세 설계](docs/archive/DESIGN_V1_KO.md): 설계 배경. 현재 상태 문서가 우선한다.
5. [검증 기록](docs/VALIDATION.md), [공개 이관 검증](validation/PUBLIC_IMPORT.md), [의사결정 기록](docs/DECISIONS_KO.md).

## 구현 상태

| 구성 | 현재 상태 |
| --- | --- |
| OBSERVE | 일회성 주차 시험 패키지 준비. 실제 차량 실행은 미검증 |
| SCRUB | 원래 mode=0인 캐시 LOCATION의 speed/bearing 유무 필드 제거. 좌표는 그대로이며 개선·악화 모두 미검증 |
| SHADOW | 이 Draft에서 실제 센서 입력과 MODEL 계산을 연결. 위치·속도·방향·LOCATION 미리보기 기록, 순정 송신 유지. 실차 미검증 |
| ASSIST | 비활성. 설정 변경만으로 켤 수 없음 |
| 공통 DR 코어 | SHADOW 모델과 검증된 입력이 같은 적분 코드를 사용. MODEL 출력을 ASSIST로 승격하지 않음 |
| DROP | 비교 실험 후보. 미구현이며 폰 fallback 성공을 보장하지 않음 |

네이버 지도가 우선 대상이다. TMAP·카카오맵·카카오내비는 각각 별도의 검증 대상이며, 다른 차량의 후기를 이 차량에서의 지원 확인으로 취급하지 않는다. 모든 차량 조작은 주차 중에 하고, 주행 중 CMU 제어를 요구하지 않는다.

## 개발과 공개 범위

Linux 호스트에 C/C++ 컴파일러, make, Python 3, pkg-config, D-Bus 개발 파일을 준비한 뒤 `make test`를 실행한다. 원본 펌웨어가 필요한 설치 fixture는 공개하지 않으며, 없으면 해당 시험은 skip으로 표시된다. 상세 방법은 [CONTRIBUTING.md](CONTRIBUTING.md)와 [ARM 도구체인](docs/toolchain.md)에 있다.

소스·설계·검증 기록을 공개한다. 원본 펌웨어·라이브러리·지도·디스어셈블 덤프·개인 공유 링크·실차 이동 로그·빌드된 설치 묶음은 포함하지 않는다. 과거 설치 문서는 검토용 기록이며 현재 설치 안내가 아니다. 프로젝트 라이선스는 아직 선택하지 않았다.
