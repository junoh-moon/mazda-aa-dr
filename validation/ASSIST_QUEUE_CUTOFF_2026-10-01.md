# ASSIST 큐 폐기 시 콜백 경계 보존 — 2026-10-01

폐기된 계산기의 큐에 남은 POSITION을 지울 때, 이후 기준점과 비교할 관측
시각·콜백 순번도 잃는 결함을 수정했습니다. 같은 입력이 한 처리 주기에
도착하는지 다음 주기에 도착하는지에 따라 실제 DR 발행·선택이 달랐습니다.
수정 커밋은 `9e082f76a3f96da27a9dd3936f93203139482d93`입니다.
NA 74.00.324A의 비활성 qualified ASSIST 경로이며, live ASSIST를 켜거나
물리 센서의 자격을 부여하지 않습니다. 공개 `v0.3.8-shadow.1` ZIP은
이 후속 변경을 포함하지 않습니다.

## 수정 전 실제 실패

[외부 master의 이전 세대 콜백 수정](ASSIST_STALE_CALLBACK_2026-10-01.md)을
LDS 출처 코어와 병합한 `e4fb28c4f25e22919bbdc3aea1fb3786630e2454`에서
재현했습니다. callback 시각·순번·generation은 실제 adapter 진입 경로에서
받고, 생산자 측정 시각·센서 자격과 worker 스케줄은 시험에서 공급했습니다.

먼저 기준점 없는 GPS가 계산기를 폐기합니다. 이때 완료 구간은 2.120초이며
관측 시각 2.130초의 다른 POSITION이 이미 큐에 있으면 함께 지워집니다.
이후 현재 generation의 실제 콜백은 2.140초에 진입하고, 결합된 새 기준점의
측정 시각은 2.125초입니다. 새 측정은 이전 완료 구간보다 늦으므로 공급자가
이미 완료 선언한 과거 구간에 새 입력을 끼워 넣은 재현이 아닙니다.

| 같은 POSITION의 전달 방식 | 수정 전 계산 결과 | 실제 adapter 선택 |
| --- | --- | --- |
| 같은 tick에서 큐에 들어간 뒤 폐기 | `PUBLISHED`, 발행 횟수 2 | `DR_REPLACEMENT` |
| 다음 tick에서 폐기된 generation으로 전달 | `INPUT_FAULT` / `BAD_INPUT`, 발행 횟수 1 | `ORIGINAL` |

host 소스 링크 재현에 이어, 변경하지 않은 실제 ARM 제품 DSO에서도
고정 sysroot와 순정 공유 runtime 각각에서 같은 차이를 확인했습니다.
same-tick 재현 프로그램의 종료 42는 의도한 실패이며, next-tick 대조는
종료 0입니다. 단순 진단 횟수 차이가 아니라 기존 음성 경계 계약을 어긴
발행·선택까지 확인했습니다. 특정 실차 위치의 오차를 입증한 시험은 아닙니다.

## 변경과 회귀

qualified 계산기가 같은 source/session의 큐를 비우기 전에, 남은 POSITION의
최대 관측 시각과 최대 콜백 순번만 기존 음성 경계에 보존합니다. 큐를 고정
용량 안에서 한 번 훑으며 새 잠금·I/O·대기·OEM 호출은 추가하지 않습니다.
폐기한 입력을 처리 완료로 간주하거나, 관측 시각을 생산자 측정 시각으로
바꾸지 않습니다. 같은 epoch의 init/rearm에서도 보존하고, 새 source/session과
MODEL 전환에서는 이전 경계를 분리합니다.

구현 전 공개 navigation·worker 회귀가 각각 종료 1로 실패했습니다.
worker 회귀는 두 번째 발행과 실제 DR 선택까지 검사합니다. 수정 후
navigation **3,977개**, ASSIST worker **23,775개**를 통과했습니다.
정상적으로 더 늦은 기준점은 새 BEGIN 없이 다시 계산·발행할 수 있으며,
거부된 입력 뒤 명시적인 새 BEGIN으로 복구하는 경우도 검사했습니다.
시간·순번 각각의 경계, 직접 reset/retire/fault/init/rearm, 미래 generation의
GPS 단절·native 콜백, 새 source/session, MODEL을 포함합니다.

첫 수정 후 navigation 시험의 새 epoch 사례는 필요한 센서 구간을 제공하지
않아 `WAITING`으로 실패했습니다. 실제 작성 speed/reverse/yaw 입력을 해당
구간에 추가한 뒤 통과했습니다. 제품 조건을 느슨하게 하지 않았고 이 실패
기록도 보존했습니다. 독립 코드 리뷰에서 최종 변경의 차단 사항은 없었습니다.

## 실제 제품 DSO와 전체 검증

실제 DSO probe는 해당 커밋의 헤더로 객체 크기를 정하고, 검사할 ELF에서
직접 얻은 함수 위치로 constructor/destructor/tick/stop과 adapter를 호출합니다.
worker·pipeline·core 구현 소스를 시험 실행 파일에 다시 링크하지 않습니다.
입력 공급과 tick 호출은 작성한 조건이며 실제 journal worker 스레드·OEM LDS·
차량 실행으로 세지 않습니다. 원본 public worker suite 전체도 동일한 DSO
경로에서 실행하여 정상 발행·복구와 거부 조건을 함께 대조합니다.

수정 전 DSO SHA-256은
`4ef64da1fae9c18c975833495c69fda9a9b41ceaf0c3d78e07fc9eeeb911b441`,
수정 후 새 ARM DSO SHA-256은
`fda002045a8bdf474012502a973729e4eb15200d1611ff828f369b4f3519f7ed`입니다.
고정 도구체인은 GCC 4.9.1
`61ec0343de84f6fc7c46840056df1d600d44be8a`입니다.

수정 후 DSO는 두 runtime의 same/next-tick 네 실행 모두 종료 0이며
`INPUT_FAULT` / `BAD_INPUT`, 발행 횟수 1, DR 미선택으로 일치했습니다.
수정 전·후 public worker suite 전체도 각각 두 runtime에서 통과했습니다.
DSO fixture의 집계는 원래 suite에 인자 검사 하나가 더해져 각각
19,622개·23,776개입니다. 이를 public suite의 19,621개·23,775개와
혼동하지 않습니다. 독립 리뷰에서 고정 소스·ELF 함수·객체 배치·로그 해시를
대조했으며 차단 사항은 없었습니다.

같은 수정 pin의 별도 checkout에서 다섯 ARM 산출물과 로컬 SHADOW ZIP을
새로 만들었습니다. ZIP SHA-256은
`83d99b3482025f1f834dee3877c48784cc22c31c850e8f3e07c205b873602dc3`이며
CRC·전체 manifest·고정 소스와 `source_modified=false`를 확인했습니다.
이 로컬 ZIP을 공개 릴리즈로 발행한 것은 아닙니다.

그 소스의 `make test`와 고정 ARM 전체 runner를 순차로 실행했고 모두
종료 0·생략 0입니다. host Python **401개**(41+124+28+1+10+163+34)와
C/C++를 통과했습니다. 양쪽에서 ASSIST worker 23,775개와 LDS lineage
11,732개가 실제로 실행됐습니다. ARM의 실제 제품 DSO suite는 position
8·request 14·request-wire 1·session 29·bus 31·assist 28·runtime-assist
9개를 통과했습니다. 전후 고정 소스 336개와 다섯 산출물이 같고,
두 `ARM_TEST_INPUTS`도 같으며 `release_verified=true`입니다.

| 이번 수정 pin의 실행 기록 | SHA-256 |
| --- | --- |
| 전체 host 로그 | `fdf615b38f5376c6f148c8038ae14c2ef62eb00f15188935ec3c960612c2c958` |
| 고정 ARM 전체 로그 | `7f254dbb32d1f5fa08cd2cf1bcce8fd6f6b8b9e0b13b0bbb7bbffc6391ca7d76` |
| 독립 실제 DSO 리뷰 | `c4a1f43c740b0a9b3ac0b94c1be7eccb33c3ee09a9dc00a94fb4c11fea7c8291` |

수정 전 `e4fb28c`의 별도 전체 host 실행은 collector 격리 회귀에서 기존
2초 subprocess 제한을 넘겨 실패했습니다. packaging/tools/integration
후속 구간은 실행하지 못했습니다. 같은 소스·실행 파일로 해당 사례만
다시 실행하면 제한을 늘리지 않고 통과했으나, 최초 실패의 원인을
확정하지 않습니다. 그 실패·미실행 구간과 같은 pin의 전체 ARM 통과는
후속 수정본의 결과와 구분해 보존합니다.

## 남은 범위

실제 측정 시각·센서 단위·품질, LDS 생산자와 요청별 snapshot 연결, 정상
차량 기동·복구와 폰/지도 앱의 DR 위치 수용은 여전히 미구현 또는 미검증입니다.
이번 변경은 실제 ASSIST 적용을 위한 계산 경계 수정이며 v1.0 완료가 아닙니다.
LDS 필드 출처 코어의 실제 제품 연결은
[별도 기록](LDS_OWNED_LINEAGE_2026-10-01.md)의 남은 범위를 따릅니다.

작성 fixture·명령·종료값·소스와 ELF 해시·독립 리뷰는 ignored
`evidence/assist-queue-cutoff-20261001/`에 보존합니다. 전체 빌드·검사와
임시 도구 목록은 `evidence/lds-owned-lineage-20261001/`에 보존합니다.
현재 임시 환경은 후속 AA 요청 identity 연결 작업에서도 사용 중이며,
도구 제거 완료를 아직 주장하지 않습니다. 원본 바이너리·덤프·raw 로그는
게시하지 않습니다.
