# ASSIST 이전 세대 콜백 경계 — 2026-10-01

대상은 NA 74.00.324A의 **비활성 qualified ASSIST** 계산 경로입니다. 공개
`v0.3.8-shadow.1` ZIP에는 이 후속 소스 수정이 들어 있지 않습니다. 그 ZIP의
설치 기본값은 SHADOW이고 live ASSIST는 차단되어 있습니다. 이번 변경은 실차
입력이나 앱 수용의 자격을 새로 얻은 것으로 해석하지 않습니다.

## 재현과 수정

이미 폐기된 계산기의 이전 generation POSITION이 한 tick 늦게 도착하면,
같은 입력 묶음에 있었을 때와 달리 worker가 `INPUT_FAULT`로 새 BEGIN을
요구했습니다. 폐기된 계산기에 발행 가능한 seed도 대기 중인 ANCHOR도
없고, 콜백 순번이 아직 처리한 POSITION보다 클 때만 `STALE_INPUT`으로
무시하도록 바꾸었습니다. 같은 폐기 입력의 중복과 서로 뒤집혀 도착한
폐기 입력도 seed나 적용 순번을 갱신하지 않습니다.

첫 수정본의 재검토에서는 더 중요한 반례가 나왔습니다. 마지막 *게시*
후보는 철회됐어도 새 계산기가 ACTIVE/READY로 살아 있으면, 오래된 GPS를
무시한 뒤 센서가 채워질 때 잘못된 기준점으로 새 후보를 게시할 수
있었습니다. 살아 있거나 큐에서 살아날 수 있는 후보 앞의 이전 세대
POSITION/ANCHOR는 계산기 자체를 철회하도록 보완했습니다. 수정 전 작성한
worker 회귀는 발행 전 ACTIVE 후보에서 실패했고, 수정 뒤 통과했습니다.

또한 이전 세대 콜백을 폐기한 **뒤** 더 이른 기준점이 도착할 수 있습니다.
adapter는 콜백 진입 순번을 먼저 부여하고, 모드·generation과 관측 시각은
그 뒤에 기록하므로 동시 호출의 순번과 관측 시각이 서로 뒤집힐 수
있습니다. 폐기한 POSITION의 최대 관측 시각과 콜백 순번을 같은
source/session epoch에서 음성 경계로 보존합니다. 이후 ANCHOR의 원본
측정 시각·결합 순번과 POSITION의 관측 시각·순번이 각각 이 경계보다
커야 합니다. 하나라도 이전이면 철회합니다. 관측 시각이나 순번을
producer 측정 시각 또는 새 기준점의 자격으로 승격하지 않습니다.
qualified 재무장에서는 경계를 보존하고 새 source/session epoch에서
지웁니다. 이는 동시 콜백의 애매한 순서에서 가용성을 낮출 수 있는
보수적 거부 조건입니다.

## 독립 검토와 검증 범위

Claude Code CLI의 읽기 전용 검토는 처음에 도착 시각 역전 사례를
안전 결함으로 지적했습니다. 후속 논의에서는 그 시각이 콜백의 실제
GPS 결정 시각이 아니라는 점을 인정했고, 대신 **이전 세대 콜백의
순번이 새 GPS보다 큰 경우**에 새 기준점이 부당하게 살아날 수 있는
반례를 제시했습니다. 그 반례를 수정 전 실패·수정 후 통과하는 회귀로
추가했습니다. 별도 Codex 독립 리뷰도 살아 있으나 미게시된 seed와
미래에 도착할 ANCHOR 경계를 다시 검토했습니다. 이 리뷰들은 소스
검토이며 제품 실행이나 차량 시험으로 세지 않습니다.

최종 targeted host 회귀는 navigation 3,358개와 ASSIST worker 19,621개
검사를 통과했습니다. 같은 최종 소스로 새 SHADOW bundle을 만든 뒤
펌웨어 fixture와 함께 실행한 `make test`는 종료 0, Python **375개**
(`Ran 1 test` 행 포함)와 C/C++ 검사 통과, 생략 0입니다. 고정 GCC 4.9.1
ARM 도구체인(`61ec0343de84f6fc7c46840056df1d600d44be8a`)으로 다섯
결과물을 새로 만들고 해당 제품 DSO로 전체 ARM runner를 실행했습니다.
ARM 종료 0, 생략 0이며 시작·끝 `ARM_TEST_INPUTS`의
`release_verified=true`와 입력 해시가 일치합니다. 제품 DSO의 ASSIST
28개와 runtime-assist 9개 사례도 통과했습니다.

| 비공개 실행 기록 | SHA-256 |
| --- | --- |
| 최종 host 전체 로그 | `28b6cae6a35c595a8cc3b8a025a85095b24d2c157023f9a92f3ff00df0b02e42` |
| 최종 ARM 전체 로그 | `2be45a9faa162497921f7be25607671d39c6fec234c4df7182f58de257625cdd` |
| 이번 소스의 ARM 제품 DSO | `3a8ea9af262e24cd9e288dcb11c431bbce035758c6cf97ea559896918d106f65` |

로그·바이너리와 Claude의 실제 읽기 전용 응답은 ignored
`evidence/assist-batching-20261001/`에만 보존했습니다. 시험의
POSITION·ANCHOR·센서 자격은 작성한 입력입니다. 원본 펌웨어 VM·실차·
폰에서는 이 후속 변경을 실행하지 않았습니다.

## 공개 v0.3.8과 통합한 소스의 재검증

원격 `v0.3.8-shadow.1`의 holdout 원본 연결·위치 기록 보완과 이 수정은
`d3c6fa1e81fa16046ebbd1f200c22ffb3c3eda6a`에서 병합했습니다.
고정 GCC 4.9.1 도구체인으로 다섯 ARM 산출물을 새로 빌드했고, 로컬
SHADOW ZIP의 `build-info.json`은 해당 커밋과 `source_modified=false`를
기록합니다. 로컬 ZIP SHA-256은
`4560926ace8b60187db448d9c5f8667fd5ad25b5f5896c69dc3ee43621d4ec31`이고,
제품 DSO SHA-256은
`4ef64da1fae9c18c975833495c69fda9a9b41ceaf0c3d78e07fc9eeeb911b441`입니다.
이 로컬 ZIP을 새 공개 릴리즈로 게시하지 않았습니다. 공개 v0.3.8 ZIP은
`40051f8` 소스·SHA-256 `19edb8b72faba813e169543ff6e1697a945954f13f76d5ee1346803b81168686`으로
그대로이며, GitHub에서 다시 받은 파일의 SHA·CRC·`build-info.json`을 대조했습니다.

첫 통합 `make test`는 종료 0이었으나 컨테이너에 순정 fixture가 없어
설치기 검사 20개를 생략했습니다. 기존 비공개 OEM VM 이미지에서 설치기용
원본 파일 여덟 개만 추출하고 네 바이너리의 SHA-256을
`packaging/firmware.sha256`과 대조한 뒤 전체를 다시 실행했습니다.
최종 `make test`는 Python **401개**(41+124+28+1+10+163+34)와 C/C++를
종료 0·생략 0으로 통과했습니다. 고정 ARM/QEMU 전체도 종료 0·생략 0이며
시작·종료 `ARM_TEST_INPUTS` 두 기록이 같고 `release_verified=true`입니다.
ARM의 실제 제품 DSO suite는 position 8, request 14, request-wire 1,
session 29, bus 31, assist 28, runtime-assist 9개를 통과했습니다. 별도
ARM 시험 실행 파일의 navigation 3,358개, ASSIST worker 19,621개,
holdout 7,258개와 formatter를 소비한 Python 51개도 통과했습니다.

| 비공개 통합 실행 기록 | SHA-256 |
| --- | --- |
| 순정 fixture를 포함한 최종 host 전체 로그 | `21a3b3d63a9d4f24cea13e1bdee70a872c4b65c0265c204a6fddb49522b9f9e9` |
| 최종 고정 ARM 전체 로그 | `876e733cfc2cf604b865a613cf9365ed478b2360a2ea3256ca6c9ce70435bf50` |

Claude Code CLI와 별도 Codex의 병합 소스 읽기 전용 검토에서는 확인된
P1/P2 회귀를 찾지 못했습니다. Claude가 제시한 큐 포화와 동시 callback
순서 가설은 제품 재현으로 확정한 결함이 아닙니다. 리뷰어들은 시험을
실행하지 않았습니다. 그 뒤 같은 로컬 ZIP을 넣은 원본 userspace
[VM 관측](INTEGRATED_VM_2026-10-01.md)을 수행했습니다. 이 VM은 진단용
PID 1과 무 GPS/CAN 입력의 제한된 환경이므로 qualified ASSIST 경로,
순정 BusyBox의 별도 직접 설치, 실차·폰 검증을 대체하지 않습니다. 공개
v0.3.8의 별도 설치·배포 검증은 [그 판의 기록](RELEASE_V038_2026-10-01.md)을
따르십시오.

## 남은 조건

실제 센서의 단위·측정 시각·품질, LDS의 생산자→snapshot→응답 자격,
OEM send 완료 순서와 선택 직전 경합, 정상 부팅·물리 복구, 폰/지도 앱의
보정 위치 수용은 여전히 미구현 또는 미검증입니다. SHADOW 관측만으로
live ASSIST를 켜거나 v1.0을 완료 처리하지 않습니다.
