# v0.3.11-shadow.2 발행·공개 파일 재검사 — 2026-10-02

[v0.3.11-shadow.2](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.11-shadow.2)을 2026-10-02 10:38:27 UTC에 SHADOW 개발 사전 릴리즈로 발행했습니다. `769f6b75a1cce559485a446e7164abecbab9878f`를 가리키는 annotated tag를 고정했고, 태그와 공개 파일을 확인한 뒤 설치 ZIP과 체크섬 파일을 다시 내려받아 로컬 후보와 바이트 단위로 대조했습니다.

| 항목 | 확인한 값 |
| --- | --- |
| 소스 커밋 | `769f6b75a1cce559485a446e7164abecbab9878f` |
| annotated tag object | `65d647d29675d4b588badf0a9f4d78e27f46c14c` |
| 설치 ZIP | `mazda-aa-dr-v0.3.11-shadow.2.zip` |
| ZIP 크기 | 1,618,702바이트 |
| ZIP SHA-256 | `9848e0b2d2d5a5c06b30499567e5b95148d485e92e0e2fdb5149de5b0f039ccc` |
| 체크섬 파일 SHA-256 | `fbb284cdcb9f3fc5c6b5f30ebfe257875ab6ec909635db38b8ff478a522ea028` |
| 공개 상태 | `draft=false`, `prerelease=true`; latest 승격 안 함 |
| 기본 모드 | SHADOW, live ASSIST 비활성 |
| 배포 형태 | 이미 승인된 진단 셸 사용자를 위한 shell-only ZIP. 셸 진입 수단은 포함하지 않음 |

공개 자산은 위 ZIP과 99바이트 체크섬 파일 두 개뿐입니다. 원격 tag object가 위 커밋을 가리킴을 확인했습니다. 공개 노트의 바이트와 두 자산 이름·크기도 확인했습니다. 공개 ZIP 검증기는 다시 받은 파일에서 CRC, 고유 파일 34개, 내부 manifest 33개, pinned source 입력 102개와 ARM 컴파일 입력 81개의 해시를 확인했습니다. 모든 여섯 제품은 `5eec1fa`의 전체 검증 빌드와 바이트가 같고 `source_modified=false`, 기본 모드 SHADOW이며 셸 진입 페이로드는 포함하지 않습니다.

## 연결되는 검증

아래 전체 제품 검사는 같은 바이너리 입력을 사용한 `5eec1faf5b642f3ea86357841b97ffe29d646686`에서 완료했습니다. 발행 시점에 전체 제품 검사를 다시 실행한 횟수로 합산하지 않습니다.

- [전체 제품·원본 실행](LDS_RMC_PRODUCT_2026-10-02.md): host Python 605개, ARM Python 131개와 C/C++, 실제 AA DSO 13개 suite/206사례, 원본 LDS 설치기 96사례가 생략 없이 통과했습니다. 원본 reader 실행은 위치·송신 9쌍, 원시 값 81개와 A/A/V/V/V/V 상태 상속을 확인했습니다.
- 현재 배포 커밋에서 여섯 ARM 제품을 새로 만들었습니다. 새 제품 6개, 컴파일 입력 81개와 ARM 빌드 기록이 `5eec1fa` 검증본과 일치했습니다. 전체 host/ARM 제품 시험을 이 커밋에서 다시 실행하지는 않았습니다.
- 셸 전용 포장 회귀 6개가 생략 없이 통과했습니다. 최종 ZIP의 순정 ARM BusyBox 검사에서는 메뉴 설치·재부팅 요청·상태·회수 흐름, 계정 회귀, 실패 경계 세 경로가 모두 종료 0으로 끝났습니다. runner는 stock alias 88개와 계정 경로 alias 90개를 확인했습니다. launcher는 514.566511526초에 종료 0, 최종 `test_exit=0`, `cleanup_exit=0`, saved readback은 0.332495204초에 종료 0입니다.

BusyBox 경로는 작성한 reboot/mount 작업과 synthetic process 증거를 사용했습니다. 정상 전체 SM/AAPA 기동, 실제 CMU 전원 전환, 실차 센서 자격, 휴대폰·지도 앱 수용을 실행한 결과가 아닙니다. 원본 자동 기동·복구와 간헐적 epoch 요청 정체의 원인은 여전히 미해결입니다. 실제 관성항법 위치가 차량·AA에 적용되는 v1.0은 미완료이며 live ASSIST는 비활성입니다. 이번 발행은 새 차량 방문이나 phone/dongle 시험 허가가 아닙니다.

## 배포 전제

이 ZIP은 재부팅 뒤에도 다시 접근할 수 있는 기존 승인 셸을 전제로 합니다. 이미 동봉된 설치 안내인 [`SHELL_START_KO.md`](../packaging/SHELL_START_KO.md)를 사용하십시오. USB 한 개를 유지하고, 주차 상태에서 안내된 엔진 상태를 확인하며, `1 → 5 → 2`와 AA 전환·주차 후 회수 순서를 따르십시오. 차량 점화 OFF/ON은 CMU Linux 재부팅 확인을 대체하지 않습니다. 메뉴·설치 자동화가 통과한 사실은 기존 셸 재접속을 제공하거나 전체 차량 기동을 보증하지 않습니다.

이전 [v0.3.11-shadow.1 발행](RELEASE_V0311_2026-10-02.md), 사용 중단한 `.10` ZIP의 경고, 모든 과거 검사 기록은 그대로 유지합니다.
