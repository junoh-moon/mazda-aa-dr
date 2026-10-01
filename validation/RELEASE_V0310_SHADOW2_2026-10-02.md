# v0.3.10-shadow.2 발행·기동 판정 검증 — 2026-10-02

고정 소스 `be96c6de6619d8e9053a327fd16e9df718384ca4`의
[SHADOW 시험판](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.10-shadow.2)을
pre-release로 발행하고 ZIP·체크섬을 다시 내려받아 대조했습니다. 대상은
1세대 Mazda Connect **NA 74.00.324A**입니다. 이는 차량 승인을 받은
v1.0이나 실제 보정 위치 송신판이 아닙니다. live ASSIST는 비활성입니다.

## 변경과 실패 조건

이전 `v0.3.10-shadow.1`의 LDS 관측 제품은 유지합니다. 새 가드는 설치한
Linux 부팅에서 SM이 다시 시도해도 다음 부팅 예약을 소비하지 않고,
`last-boot` 원자적 게시 뒤 디렉터리 동기화가 실패하면 가시 표식을
철회합니다. 철회의 지속성도 확인되지 않으면 별도 종료 코드 3을
반환합니다. 상태 진단은 v3 `consumed`에 묶인 설정 해시와 현재 설정을
대조하고, 달라진 설정으로 과거 선택을 양성 판정하지 않습니다.

`guard_committed_after_new_boot`는 가드가 다른 Linux 부팅에서 소비·
부팅 표식을 확정했다는 제한된 뜻입니다. 가드가 stdout에 시험 경로를
전달하려던 뒤의 실패나 SM 수신은 표식만으로 판별할 수 없습니다.
`config_mode`도 현재 파일 해석값이지 AA 실행 모드의 증명은 아닙니다.
손상 표식은 재설치·재무장으로 덮지 않고 회수해 조사하도록 안내합니다.

## 고정 빌드와 전체 검사

GCC 4.9.1 고정 ARM 도구체인 커밋
`61ec0343de84f6fc7c46840056df1d600d44be8a`로 여섯 제품을 빈
빌드 디렉터리에서 생성했습니다. `arm-build.json` SHA-256은
`0fad2731c900d4615a8c541fc0f6892a2f66cfbb7b1eaa14a2972ed40bb5805b`입니다.
ARMv7 Cortex-A9 NEON softfp와 각 ELF 의존성·GLIBC 범위를 빌더가
검사했습니다. 실제 변경된 제품은 `mx5dr-guard`이며 SHA-256은
`a4b893dd444765ecc26b225eb5292c560009b3fd4dcb023fbcc2749bbfe38cca`입니다.

| 검사 | 이 고정 소스의 실행 결과 |
| --- | --- |
| `make test` | 종료 0, Python 576개와 C/C++ 통과, 생략 0 |
| `tests/run_arm_all.sh` | 종료 0, Python 105개와 C/C++ 통과, 생략 0 |
| 실제 제품 DSO | 8개 suite·145개 사례 통과 |
| 원본 LDS 설치기 | 원본 공유 runtime을 사용하는 별도 ARM fixture 71개 통과 |
| 최종 ZIP의 순정 BusyBox | 설치·회수·제거 후 회수, 메뉴 `1 → 5 → 2`, 실제 ARM guard 선택과 동일 부팅 거부 통과 |

ARM runner 시작·종료의 `ARM_TEST_INPUTS`는 동일한 여섯 제품 해시와
`release_verified=true`를 기록합니다. ZIP 에뮬레이션의 `/proc`, mount,
재부팅 뒤 boot ID, collector poll은 작성한 입력이고 CPU·BusyBox·guard는
원본 ARM 실행입니다. 메뉴 시험에서 같은 부팅의 선택 거부, 작성한 새
boot ID 뒤 일회성 소비, 설정 변경·NUL 표식·`od` 오류의 보수적 판정을
확인했습니다. 이 실행은 OEM PID 1·SM·AA·VBS 정상 전체 기동이나
물리 플래시 전원 단절을 재현하지 않습니다.

Claude CLI의 읽기 전용 적대적 리뷰는 공개 소스 변경만 보았으며,
비공개 펌웨어·차량 로그는 전달하지 않았습니다. 지적한 동일 부팅
선택, 판본 표현, 손상 표식 안내와 도구 오류 처리를 수정하거나 범위를
명확히 했습니다. Codex의 독립 읽기 전용 리뷰 네 갈래와 최종 재검토는
가드 저장 실패·설정 결합·SM 경로 수신의 한계를 확인했습니다. 리뷰는
실차 실행의 대체가 아닙니다. 소스 수정과 발행은 Codex 주체로 수행했습니다.

## ZIP과 게시 확인

- 설치 파일: `mazda-aa-dr-v0.3.10-shadow.2.zip`, 1,597,913 bytes,
  최상위 entry 40개
- SHA-256: `76d6049db04631352ee7b37cb5f4f0294ad9b36e123dc8ad05b062a1a59d4148`
- 내부 `build-info.json`: 위 소스 pin, `source_modified=false`,
  `default_mode=SHADOW`, 대상 `NA 74.00.324A`
- ZIP CRC·내부 `SHA256SUMS`, MP3/JS 진입 파일·정적 해시 도구 확인

2026-10-01 20:26:12 UTC에 태그를 SSH로 게시했습니다. 원격 annotated
tag의 대상은 위 고정 커밋이고, GitHub 릴리즈는 draft가 아닌 pre-release
입니다. 첨부 파일은 설치 ZIP과 `.zip.sha256` 두 개뿐입니다. 공개 파일을
재다운로드하여 sidecar SHA-256, 바이트 단위 `cmp`, ZIP CRC와 내부
`build-info`를 확인했습니다. 원본 펌웨어, 개인 공유 링크, 차량 로그는
첨부하지 않았습니다.

과거 v0.3.9 차량 회수에는 trace·collector JSONL이 없어서 실제 CMU
새 부팅, 센서 수신·시각·품질, MODEL 계산, 정상 AA 실행, 물리 복구 및
Galaxy S25·무선 AA 동글·지도 앱 수용을 이 릴리즈의 성공으로 추정할 수
없습니다. 승인된 한 번의 현장 기회는 이미 사용됐습니다. 새 방문을
요청하지 않으며 [v1.0 완료 조건](../docs/V1_READINESS_KO.md)은 남습니다.
