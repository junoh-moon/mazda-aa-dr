# 평탄화 전 D-Bus 요청·응답 연결 — 2026-10-01

구현은 `3812fe6`과 `fe052be8722e155e3a10d3cfb9872abf23afe2e0`입니다.
NA 74.00.324A 전용이며 작업 브랜치는 `feat/session-observation`입니다.
아래 검사는 실제 차량이나 폰을 사용하지 않았습니다. 기존 검증 기록을
새 결과로 덮어쓰지 않으며 공개 설치 ZIP의 발행과도 구분합니다.

## 해결한 문제

이전 원본 실행에서는 공개 method serial getter가 0을 반환하고 reply getter도
번호를 얻지 못했습니다. 별도 버스 monitor에 번호가 보여도 제품 request token과
직접 연결할 수 없었습니다. 이제 원본이 JCIDBUS 응답을 평탄화하기 전에
실제 raw 헤더를 복사합니다.

- 실제 제출 scope의 method와 원본 builder가 반환한 메시지를 연결합니다.
  raw send가 성공하고 실제 pending을 반환했을 때 살아 있는 메시지의 serial을
  같은 request token에 저장합니다. 경로 문자열·좌표·시각 유사성은 키가 아닙니다.
- 원본 pending handler의 살아 있는 node에서 method/pending 일치를 확인하고,
  원래 steal 호출이 반환한 메시지의 헤더를 동기 scope 안에서 복사합니다.
  원본 handler는 notify 이후까지 method를 보유하고 검증된 getter는 재진입하지
  않습니다. OEM node·userdata·참조 수를 변경하거나 별도 pending map을 만들지 않습니다.
- 기존 getter 결과와 별도인 `request.wire`를 POSITION/SEND까지 보존합니다.
  raw 응답 serial, reply_serial, 종류, sender, 오류와 관측 시각을 기록합니다.
  로컬 NoReply의 serial 0·sender 부재도 보존하며 원격 응답으로 세지 않습니다.
- 중첩된 미관측 호출은 바깥 scope를 가립니다. 반복 전송의 두 번째 호출이
  실패해도 conflict를 기록합니다. 보조 metadata의 trylock 실패는 기존 요청
  관측 epoch를 무효화하지 않으며 원본 호출·반환·errno는 유지합니다.
- 설치기에 검증된 raw API와 네 GOT 슬롯을 추가했습니다. 기존 모듈 전체 해시,
  주소·실행 구간·함수 바이트·원래 호출 대상 검사를 유지합니다. 총 20개 슬롯이며
  지원 함수가 LDS 제출 후크보다 먼저 공개됩니다. JCIDBUS 코드 페이지는 패치하지 않습니다.

분석기는 이전 wire 없는 로그를 읽고 충돌·reply_serial 불일치를 보고합니다.
최대 escape fixture에서 NUL 포함 request 4,415바이트·SEND 행 4,995바이트를
확인했습니다. request 버퍼를 4,096에서 5,120바이트로 늘리고 outer 5,120은
유지했습니다. 정확한 용량·한 바이트 부족·메모리 경계와 종료 배출을 검사했습니다.
기존 저장 공간 상한·회전·원시 입력 우선 기록은 유지합니다.

## 원본 라이브러리 실행

제공 펌웨어의 원본 JCIDBUS/libdbus를 고정 GCC 4.9.1 ARM caller와 QEMU user로
실행했습니다. 서버는 작성한 진단 서버이며 원본 LDS가 아닙니다. 같은 실행의
server 기록과 독립 dbus-monitor를 unique sender별로 분리하여 대조했습니다.

| 항목 | 결과 |
| --- | --- |
| 실제 요청 / callback / 취소 | 29 / 28 / 1 |
| 동일 본문 Pair | 두 요청을 동시 대기하고 역순 응답, token별 serial 일치 |
| 원격 응답 | Pair 2 + Echo 24 + 오류 1, 총 27개 대조 |
| 로컬 timeout | NoReply 1개, raw serial 0·sender 부재·해당 reply_serial 보존 |
| 원본 method 주소 재사용 | 26회 |
| 종료 시 요청·worker·관측 loss | 모두 0 |
| wire 관측 비활성 baseline | callback 순서·token·본문·공개 getter·완료 집계 동일 |

공개 getter는 28개 callback 모두 serial unknown/0입니다. monitor의 error 행은
응답 serial을 표시하지 않으므로 그 serial은 server와 caller 사이에서만
대조했습니다. baseline에 없는 raw serial을 추정해 비교하지 않았습니다.

초기 caller에는 공개 uint32 getter의 이름·반환형 오류가 있었고 수정했습니다.
bare connection과 불완전한 dispatch 준비에서는 timeout 검사가 실패했습니다.
최종 실행은 원본 create/connect, timeout attach/detach, JCIDBUS dispatch와
raw dispatch, 원본 connection free를 사용했습니다. 초기 실패는 보존하며
차량 결함이나 제품 회귀로 세지 않습니다.

## 실제 제품 설치와 data-client 연결

제품 DSO를 startup preload한 뒤 원본 BLM·JCIDBUS·data-client·libdbus·AA
interface를 적재하고 실제 `install_v74`를 호출했습니다. 전체 파일 해시 다섯 개,
cold lease, GOT 20개·BLM 진입점 네 개의 연결과 원본 prologue 나머지 바이트,
정상 초기 hook 상태와 재설치 거부를 확인했습니다. 초기 late-load 실험은
static TLS 공간 부족으로 실패했으며 정상 startup preload 순서에서 통과했습니다.

같은 설치 뒤 원본 `LDS_DATA_GetPosition` 네 요청을 실행했습니다. 작성한 서버의
동일한 9필드 응답 두 개가 역순으로 도착해도 product token별 serial이 일치했고,
오류 응답 한 개와 connection free로 끝난 미응답 요청 한 개도 구분했습니다.
종료 시 요청·worker·loss는 모두 0입니다. worker 소비는 작성한 fixture이며
새 실행이 전체 BLM WorkerQueue·SM·LDS 센서 기동까지 검증한 것은 아닙니다.

## 회귀 검사

- 전체 `make test`: Python 374개와 C/C++ 통과, 생략 0입니다.
- 고정 ARM 전체 runner: 정상 종료 0, 생략 0입니다. 처음/끝 build input 기록의
  다섯 바이너리와 도구체인 해시가 같고 `release_verified=true`입니다.
- 실제 제품 DSO: position 8, request 14, wire contract 1, session 29,
  bus 31, assist 15, runtime-assist 9개를 포함합니다. wire contract 한 프로세스는
  역순·재사용·중첩·미관측·실패·중복·pending 불일치·예외 복구를 함께 검사합니다.
  최종 wire fixture는 host와 고정/순정 공유 runtime에서도 별도 통과했습니다.
- generic trace 16그룹, Observer 10그룹, handoff 6그룹, 상태 2,000회는 host와
  고정 ARM에서 통과했습니다. supplemental 경합의 epoch 유지도 검사했습니다.
- cold patch는 29개 보호 호출 실패 지점, 기존 대상 재검사와 rollback을 통과했습니다.
  중복 send 실패의 conflict 누락과 JSON 용량 부족은 수정 전 실패를 재현했습니다.

| 검증 대상 | SHA-256 |
| --- | --- |
| 제품 `libmx5dr.so` | `598e004b50bd7172318b14bd5eec272cf070893a3dfcb83b237dd3261f414b6c` |
| 최종 wire fixture 소스 | `94b1ca81dd6f406bdd9a3190ae342d199cb60c2de03b887b4d19138e2b4350dc` |
| 전체 host 로그 | `d77f2ea1137871750c5479f8c6030b4e5cfc1dc8ce6d7062e7dc709a17b4b1a6` |
| 전체 ARM 로그 | `b71d1e239ef1761902989f7811cae4cced735dcbf9fefcdc467201f170407a3d` |

원본 바이너리·주소 자료·전체 로그는 ignored `evidence/wire-request-20261001/`에만
보관합니다. 도구는 전용 컨테이너에 설치했으며 추가·변경 패키지 79개와 pinned
도구체인 2,124개 파일을 목록으로 기록했습니다. 발행·최종 설치 검사와 도구
제거는 별도 릴리즈 검증 기록에서 확인합니다.

## 남은 범위

wire 연결은 관측된 요청 identity입니다. LDS producer→snapshot→응답의 자격,
실제 센서 단위·측정 시각·품질, 수신기·세션 소유권, 물리 복구와 Galaxy S25/
지도 앱 수용은 여전히 미구현 또는 미검증입니다. live `allow_assist=false`와
자격 공급부의 TODO를 유지합니다. 이 결과만으로 실제 관성항법 적용이나 v1.0
완료를 선언하지 않습니다.
