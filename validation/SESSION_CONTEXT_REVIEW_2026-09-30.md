# 세션 관측 독립 검토·동시성 수정·원본 재실행 — 2026-09-30

요청 발행 당시의 세션 문맥과 실제 송신 저장소의 세션을 제품에 연결했습니다.
독립 구현을 그대로 승인하지 않고 **추가된 storage 읽기의 data race, 겹친
생성·파괴 후의 잘못된 생존 관측, 분석기의 상태 이력 모순 누락**을 재현하여
수정했습니다. 원본 API의 지연 응답에서도 이전 문맥이 보존됨을 확인했습니다.
세션 소유권·폰 연결·ASSIST 또는 v1.0 완료의 증거는 아닙니다.

## 외부 작업과 이번 실행의 구분

기준 master는 `afed20243f29606cb04af6b6459d56438cff60bb`입니다. 작업 중 SSH
fetch로 아래 독립 작업을 확인하고, 작성 소스와 필요한 회귀만 통합했습니다.

- [`56af561` 세션 제품 관측](https://github.com/junoh-moon/mazda-aa-dr/blob/56af56109f3828e56e175dfaef24881902cefcb1/validation/SESSION_PRODUCT_2026-09-30.md):
  create/destroy·상태 callback, issue/send 문맥, journal·분석기를 추가한 근거입니다.
- [`854f74d` 원본 지연 응답](https://github.com/junoh-moon/mazda-aa-dr/blob/854f74d0f469568fa29fa7e09a4c7ac4592963d4/validation/SESSION_TRANSITION_2026-09-30.md):
  원본 manager 정지가 대기 요청을 취소하지 않는 조사와 실제 DSO 요청 회귀를
  추가했습니다. 중간 merge `e901a4a`가 기존 master의 진단 강화를 반영한 것도
  확인했습니다.

외부 기록의 비공개 실행 자료는 이 workspace에 없었습니다. 해당 기록의
실행 수치를 이번 실행으로 바꾸지 않았으며 Claude CLI를 새로 실행했다고
주장하지 않습니다. 아래 원본 VM은 주에이전트가 직접 실행했습니다.
네 독립 리뷰어에게는 작성 코드·시험만 제공했고 OEM 자료는 제공하지 않았습니다.

## 재현한 결함과 수정

1. **P1 — 생성 반환 후 OEM storage 재읽기.** 원래 작성 API의 storage 쓰기를
   같은 mutex로 보호해도, wrapper의 `*storage` 읽기는 그 mutex 밖에서
   destroy와 경합했습니다. TSan이 wrapper 행의 race를 검출했고 wrapper 없는
   대조군은 통과했습니다. 그 읽기를 제거했습니다. 관측하는 것은 create의
   반환 0과 이후 관측한 destroy 경계이며, 현재 handle 유효성을 판정하지 않습니다.
2. **P2 — destroy 중 새 create의 잘못된 LIVE.** destroy 진입 때 만든 종료
   대상 목록에는 그 뒤 생성된 문맥이 없었습니다. 실제 storage가 NULL이 된
   뒤에도 lifetime 2/OBSERVED/fault 0을 재현했습니다. 양방향 재진입 및 별도
   thread에서도 검사했습니다. 기존 atomic 진입 수로 lifecycle 중첩을 확인해
   관측을 sticky fault로 남깁니다. 서로 다른 storage의 중첩도 이 관측 계약에서는
   불완전합니다. 원본 호출을 막거나 직렬화하는 새 잠금은 추가하지 않았습니다.
3. **P2 — 같은 lifetime의 모순을 분석기가 통과시킴.** event 후퇴, 같은 event의
   다른 state, known 상태의 unknown 회귀를 `local_checks_pass`로 판정했습니다.
   이제 `session_state_inconsistent` violation입니다. 정상 전진·같은 값의 다음
   event·unknown에서 첫 callback을 받는 경우는 허용합니다.

API 반환 0을 handle이나 요청 소유권으로 승격하지 않습니다. qualified bus/session
필드는 unknown, `provenance()`는 false, `allow_assist=false`를 유지합니다.
64개 문맥은 재사용하지 않으며 용량 초과·관측 실패 때도 원본 callback을 전달합니다.
상태·userdata·나머지 callback 18개·전체 payload·반환값·errno 보존을 검사했습니다.

## 네 독립 리뷰와 지속적인 회귀

- 동시성 리뷰는 native·ASan/UBSan·TSan으로 재현을 대조했습니다. 새 공개
  `output_race`와 `null_success`는 storage 읽기 복원을 검출하고, lifecycle 중첩
  처리를 지우면 같은/다른 storage·양방향 재진입 네 사례가 실패합니다.
- ARM 리뷰는 고정 GCC 4.9.1의 실제 제품 DSO와 생성 코드를 검사했습니다.
  storage 후행 load가 사라졌고 64비트 atomic 후행 DMB·8바이트 정렬·정적
  초기화가 유지됩니다. 외부 atomic helper와 세션 동적 생성자는 없습니다.
- 통합·분석기 리뷰는 요청 문맥 복사를 없애도 이전 세부 시험들이 통과하는
  공백을 찾았습니다. 실제 submit→늦은 reply→worker→새 세션 send 공개 회귀와
  실제 DSO `session_transition`을 연결했습니다. 추가 ARM runner의 trampoline
  링크 누락도 발견·수정했습니다. 분석기 회귀는 세 조건 삭제와 severity 변경을
  각각 검출했습니다.
- 마지막 독립 리뷰는 실제 DSO 세션 20개·요청 14개, 별도 분석기 입력 70개를
  실행하고 C++ 결함 8개·분석기 결함 2개를 검출했습니다. 미해결 P1/P2를
  발견하지 못했다는 범위의 결론이며 실제 차량 검증을 대신하지 않습니다.

기존 callback 검사는 포인터·마지막 sentinel만 보아 payload 중간 바이트 변조를
놓쳤습니다. 이제 callback 진입·복귀에서 전체 2,400바이트를 대조합니다.
전·후 payload 변조와 create/destroy 반환값 0 강제 네 변형이 모두 실패했습니다.

Darwin의 pthread cancellation이 C++ scope를 unwind하지 않는 차이도 별도 최소
프로그램으로 분리했습니다. macOS에서는 취소 3개만 명시적 SKIP(77)이며 나머지
17개는 통과했습니다. Makefile은 77과 Darwin을 함께 확인합니다. GNU host·ARM·
원본 Linux runtime에서는 취소를 포함한 20개 전부를 실행했습니다. 첫 동시성
리뷰의 macOS sanitizer 결과 역시 취소 3개를 제외한 범위입니다.

## 최종 빌드·전체 검사

최종 `build/arm-session-context-r3`의 제품 SHA-256:
`f298d7c3a40df505b0bc5c367fac7c0ff82b81a030d190ae98e48d67b1fc9fea`.
빌드 입력 52개와 고정 toolchain을 검증했습니다. Makefile의 Darwin 판정 변경 뒤
다시 빌드했으며 다섯 산출물 모두 실제 VM에 넣은 r2 산출물과 byte-identical입니다.

- GNU `make test`: Python **291개**(41+55+28+1+10+126+30)와 C/C++ 검사 통과,
  skip 없음. stock rootfs와 새 ARM bundle을 명시했습니다.
- 고정 ARM 전체 runner: skip 없이 통과했습니다. 실제 DSO 위치 8개·요청 14개·
  세션 20개, 초기화·cold rollback·기존 센서/MODEL 및 새 요청→세션 통합을 포함합니다.
- 원본 feature ELF `8f065b8a…`도 기존 전체 검사를 통과했지만 위 결함을
  놓쳤습니다. 그 결과와 실패 재현을 보존했고 최종 수정 ELF와 혼합하지 않습니다.
- 추가 ARM 통합의 링크 누락과 비공개 callback caller의 `-lrt` 누락으로 실패한
  명령도 보존했습니다. 누락을 고친 뒤 해당 컴파일과 최종 검사를 다시 실행했습니다.

## 직접 원본 펌웨어 VM 실행

NA 74.00.324A 원본 kernel/rootfs·LDS·AA API와 수정된 실제 preload를 격리
Sabrelite VM에서 실행했습니다. 호스트 장치·네트워크·공유 디렉터리는 없습니다.
커널 진입 시 machine ID를 조정한 뒤 debugger를 분리했으며 userspace debugger는
사용하지 않았습니다. 제품의 파일 해시·원본 진입값·cold-install 검사를 유지했습니다.

한 VM 안의 **세 별도 프로세스**를 검사했습니다.

1. 상태 callback 진단은 원본 RaceAap의 생성/파괴를 두 번 호출합니다.
   제품이 설치한 create/destroy를 그대로 전달하는 비공개 외부 진단 shim은
   저장소 인자와 API 결과를 기록하며 원본 callback table을 바꾸지 않습니다. 실제 제품의
   상태 reader에서 각 lifetime의 event 1/state 0(INVALID)을 확인했습니다.
   start 인자는 합성 304바이트 0값이며 stop은 264입니다. 원본 큐·LDS worker를
   이 프로세스에서 실행하지 않았고, 두 명시적 48바이트 송신은 폰 수용 증거가 아닙니다.
2. manager 정지 유지: 기존 manager 경로를 실행·정지한 뒤 실제 AA-util 요청
   네 개를 발행하고 client dispatch를 보류했습니다. 세션을 파괴·재생성한 뒤에도
   같은 네 method가 pending임을 확인했습니다. dispatch 재개 후 POSITION은
   도착하지만 정지된 원본 manager는 하위 send를 호출하지 않았습니다.
3. manager 재시작: 같은 준비 뒤 원본 큐에서 manager 시작을 완료한 다음
   dispatch를 재개했습니다. 네 이전 요청의 POSITION은 issue lifetime 1을
   보존했고 LOCATION은 실제 send lifetime 2로 전달됐습니다. 이후 새 요청에는
   lifetime 2가 붙었습니다. LDS 본문·OEM 객체를 덮어쓰지 않았습니다.

첫 VM의 검사 결과:

| 경우 | 실제 mode 0 요청 | 명시적 합성 seed | 지연 요청 | 해당 LOCATION send |
| --- | ---: | ---: | ---: | ---: |
| manager 정지 유지 | 14 | 1 | 4 | 0 |
| manager 재시작 | 16 | 1 | 4 | 4 |

두 manager 진단의 실제 LDS mode·UTC·좌표는 0입니다. 합성 mode 2 seed 이후의
LOCATION은 캐시 전달이며 새로운 유효 GPS 표본이 아닙니다. POSITION/SEND의
request metadata, 원본/송신 바이트 일치와 하위 반환 0을 검사했습니다.
세 프로세스 모두 journal drop·audit fault·request loss·session fault 0,
최종 request/worker 0과 durable capture acknowledgement를 확인했습니다.
원본 loader/libc/shared C++ runtime에서 실제 DSO의 작성 세션 20개도 통과했습니다.

일반 분석기는 callback 진단에 위치 요청 문맥 부재 2건으로 inconclusive(exit 2),
manager 정지 유지에 local_checks_pass(exit 0), 재시작에 세션 불일치 8건으로
inconclusive(exit 2)를 반환했습니다. 전용 검증은 이 의도적인 범위 차이를 구분합니다.
완료 표식·종료 코드·재시작 표식·이전 요청 문맥을 깨뜨린 네 입력은 각각 거부했습니다.
바깥 runner의 220초 timeout/124는 성공 기준이 아닙니다. 개별 fixture의 완료와
exit 0, 최종 로그·해시·종료 기록을 근거로 삼았습니다.

**반복 실행의 실패도 남아 있습니다.** 같은 이미지의 두 번째 VM에서는 manager
정지 유지 경우가 지연 응답 뒤의 즉시 `request/worker 0·loss 0` 검사에서
종료 1을 반환했습니다. 이 caller는 실패 시 세부 count/result를 출력하지 않아
당시의 정확한 상태를 복원할 수 없습니다. 전용 판정기는 해당 VM을 거부했으며
첫 VM의 성공으로 반복 안정성을 선언하지 않습니다. 원본 console과 실패 기록을
보존했습니다. 다음 진단에는 실패 시 health와 journal 보존, 실제 상태를 확인하는
제한된 drain 대기를 추가했습니다. 제품 코드는 바꾸지 않았으며, 이 개선 자체를
과거 실패의 원인 규명이나 제품 수정으로 세지 않습니다.

세 번째 VM은 위 진단을 추가한 별도 이미지로 실행했습니다. callback과 두 manager
경우 모두 완료·종료 0·durable acknowledgement를 확인했고, 원본 runtime의 작성
세션 20개도 통과했습니다. 정지 유지/재시작에서 실제 요청 14/16건, 합성 seed 각
1건, 이전 요청 각 4건과 해당 LOCATION send 0/4건을 다시 확인했습니다.
추가 health 기록에서는 두 경우 모두 첫 확인부터 request/worker/loss 0,
session fault 0으로 **drain 재시도 0회**였습니다. 일반 분석기의 판정은 첫 VM과
같았고 전용 판정기의 결함 입력 네 개도 거부했습니다. 따라서 추가 진단에서
실패가 재현되지 않았을 뿐, 제한된 대기가 과거 실패를 고쳤다는 증거는 없습니다.

## 보존 자료와 남은 범위

비공개 `evidence/session-context-20260930/`에 소스 snapshot·빌드 기록·실패/통과
로그·네 리뷰와 변형 검사·원본 VM console·전용 판정기를 보존합니다. OEM 원본,
전체 console, 개인 공유 링크와 장치 자료는 git에 추가하지 않습니다. 이번 작업은
기존 컨테이너와 고정 toolchain을 사용했으며 새 패키지를 설치하지 않았습니다.

| 자료 | SHA-256 |
| --- | --- |
| 실제 VM initrd | `9b1da03ab9b0636fc8f18ecc1e6a3cc01357dfcd7391bcd0ecf1c5f2f13ea897` |
| 원본 manager caller | `2372f9024647b923d6f07c2a36b696b229fe9fba049e6d26e8a0c8caab2a3366` |
| 제품 callback caller | `67d125edb919581a522fd52148d96c527e8b947f6bc147651cdcc7ff77ec9f7a` |
| 첫 VM console | `07ee1219d363af3745b80b4d9b03547d540dc585c0d74f1a0cb90aae19b89c2e` |
| 실패한 반복 VM console | `c2478c12388a6576e43055018ec2f937665edcc0f2735c0e24b8618522ff0b5e` |
| 진단 추가 initrd | `c4f180ef732522d3f721065617a19940686775cfff7dc50a13d7ce794812b0ac` |
| 진단 추가 manager caller | `ec4f23679a1517f23eff9682792a0c7834ffe5cc243c2b379bbfc343fb2f030f` |
| 세 번째 VM console | `a3e7cfd40e65cd8390c91b92b64a9451807e1f358331957654ffc669e21221b3` |

**미완료:** 요청의 실제 session/receiver/provider 자격, 세션 전환을 가로지르는
센서·계산 결과의 ASSIST 연결, 물리 입력 단위·시간·품질, 폰/앱 수용, 정상 전체
SM 기동·물리 복구입니다. 기존 OEM SDK의 mutex/semaphore/transport 오류를
해결했다고 주장하지 않습니다. 실차를 사용하거나 방문을 요청하지 않았습니다.
반복 VM의 정리 검사 실패도 정확한 원인이 미분리입니다.
공개 설치 ZIP은 기존 v0.3.1-shadow.1이며 이 소스 변경을 v1.0으로 게시하지 않습니다.
