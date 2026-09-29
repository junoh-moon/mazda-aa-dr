# 위치 작업 객체 ABI와 요청 출처 — 2026-09-30

원본 위치 작업 객체의 생성·파괴 ABI를 격리 ARM VM에서 실행했습니다.
이 결과는 실제 요청의 provider/receiver/session 연결을 구현하거나 검증한
것이 아닙니다. live `provenance()`와 qualified runtime 연결은 여전히
미구현이며 `allow_assist=false`입니다.

## 원본 객체의 실제 실행

대상은 NA 74.00.324A의 BLM SHA-256
`10e7235bfce075b44c1a8ffc99bbc9b63af85d9262df868ca1874ec36d8d3b71`입니다.
고정 GCC 4.9.1의 Linux 도구체인 2,124개 blob을 다시 검증한 뒤 ARM softfp
fixture를 빌드했습니다. 런타임 파일 해시·load bias·entry·vtable을 대조하고
원본 함수를 호출했습니다. 절대 주소를 고정하거나 OEM 함수를 대체하지 않았습니다.

| 원본 경계 | 실행한 ABI |
| --- | --- |
| WLdsGetPosition 생성자 | this와 mode/UTC/위도/경도/고도/heading/속도/수평/수직의 아홉 scalar 입력. position 구조체 포인터가 아님 |
| IsAsync | const this 입력, false 반환, 객체 바이트 보존 |
| D1 정상 소멸자 | this 입력, 객체 경계·필드 보존과 파생 vptr이 아닌 nonzero 값으로 전환 확인 |
| D0 deleting 소멸자 | 원본 operator new로 할당한 객체에만 호출, 정상 소멸과 해제 뒤 fixture는 해당 바이트를 읽지 않음 |

동일한 8바이트 정렬·88바이트 storage에 서로 다른 세 payload를 번갈아 넣어
33회 생성/IsAsync/D1을 실행했습니다. 아홉 필드와 양옆 16바이트 canary, padding,
vptr, `[this+0x50] == this+8`을 확인했습니다. 생성자는 padding을 초기화하지
않으므로 그 공간을 요청 metadata로 사용해서는 안 됩니다. canary 검사는 이
쓰기 경계의 확인이며 모든 out-of-bounds read가 없다는 증명은 아닙니다.
D1 뒤 vptr은 nonzero이고 기존 파생 vptr과 다름을 검사했으며, 정확한 base
vtable 주소와의 equality를 동적으로 검사한 것으로 확대하지 않습니다.

별도로 원본 libstdc++의 실제 operator new 소유와 해시를 확인해 32회
생성/D0를 실행했습니다. 이후 할당에서 같은 주소 재사용을 31회 관찰했습니다.
각 순환의 필드·경계 검사 합계는 1,391개이며 fixture 프로세스가 0으로
종료됐습니다. 이 수는 서로 다른 1,391종 시나리오를 뜻하지 않습니다.

### 실패를 보존한 의존성 재현

첫 r1은 BLM의 NOW 로딩에서 `COMMON_UTIL_List_Delete`를 찾지 못해
종료 2였습니다. 생성자나 소멸자는 실행되지 않았습니다.

실제 제공자는 원본 `libjcicommon_util.so`입니다. 원본 launcher의 직접
DT_NEEDED와 이전 실제 AA loader 기록에서 BLM보다 먼저 초기화됨을 확인한
뒤, r2에서 해당 원본 DSO를 해시 검사하고 NOW|GLOBAL로 먼저 로드했습니다.
BLM NOW 로드와 실제 initializer 1회가 이어졌습니다. symbol stub이나
LAZY 우회는 사용하지 않았습니다. 빠진 provider와 순서를 재현한 것으로,
launcher 전체 namespace가 동일하다고 주장하지 않습니다.

common-util SHA-256은
`c44304ad0e493eb172e0e298ba5ce6d35ee5d021563dae97af5877852dc98e58`,
원본 libstdc++는
`e83967f730b5b212921ebd4a49314ee418d38bc58ad6400c0503b4f4a767214a`입니다.
fixture의 직접 의존성은 libc/libdl이며 별도 C++ runtime으로 원본 allocator를
대체하지 않았습니다.

### 실행 식별자와 한계

QEMU 7.2.22/Sabrelite, 원본 Linux 3.0.35, 기존 kernel-entry r1 조정,
`nohlt enable_wait_mode=off`를 사용했습니다. NIC·호스트 장치·공유 디렉터리는
없습니다. 진단 PID1은 product preload, SM·VBS·AA ServiceInit,
PostWorker·doWork·vehicle-data 송신·폰/session API를 호출하지 않았습니다.
원본 DSO 초기화와 프로세스 종료는 실제로 수행됐습니다.

| r2 비공개 산출물 | SHA-256 |
| --- | --- |
| authored fixture | `5ec5bea308c377ab8e1aa4ed2105c9a31dcd71ab4480edc435a4e0555ae7cd82` |
| 진단 init | `b00ec365658d21ae3ba7cef85f12fd314cabb82de6212943c614318d61ac8662` |
| initrd | `cdd4a0e2a830750ddb0dabb686a98b01ec3274a41d6629c2e90ae759914cab41` |
| console | `c151807ea58dfe24660407b3046fd4c3fb0112e654c6350bc75c169d8c187dd1` |

fixture 완료와 VM 종료를 구별했습니다. VM은 120.022초 제한에 도달했고
timed_out=true, QEMU exit=0이었습니다. PASS는 fixture의 전체 검사와 종료 0을
근거로 합니다. 소스·명령·r1/r2·컴파일 및 이미지 출처를 비공개 보존했고,
상위 작업자도 identity chain과 판정을 다시 계산해 일치를 확인했습니다.

## 실제 요청 연결의 남은 구현

별도 정적 감사는 정확한 원본 일곱 파일, 함수 경계 28개, import slot 8개와
직접 ARM 호출 연결 8개 등을 대조했습니다. 다음은 정적 분석의 해석이며
위 객체 실행만으로 큐·요청 lifetime을 확인한 것으로 확대하지 않습니다.

- 원본 LDS 응답은 callback에서 위치 worker로 복사된 뒤 큐를 통과합니다.
  현재 위치/send hook의 TLS만으로 그 이전 요청까지 연결할 수 없습니다.
  PostWorker의 C++ shared_ptr 값 전달은 간접 주소 ABI이며 Worker 포인터
  하나로 대신 호출해서는 안 됩니다.
- 실제 sender와 오류는 JCIDBUS reply callback에서 복사할 수 있는 후보입니다.
  원본 util은 이를 평탄화하므로 현재 위치 필드만으로 복원할 수 없습니다.
  reply serial getter의 backing message가 해당 경로에서 존재하는지도 아직
  실행 검증하지 않았습니다.
- AA util의 selected GPS 값은 전역 cache입니다. 이것이나 collector의 최신
  owner/receiver poll을 과거의 특정 위치 응답에 붙여 자격을 증명할 수 없습니다.
  아홉 위치 반환값에도 receiver 세대 필드가 없습니다.
- AA status callback의 현재 userdata는 고유 session ID가 아닙니다.
  실제 create/destroy/status 전이와 요청 발행 시점의 관계를 보존해야 하며,
  재접속·같은 주소 재사용·뒤늦은 reply를 구분하는 구현과 검사가 필요합니다.

다음 구현 단위는 실제 method/reply와 큐에 들어간 객체의 identity를 보존하는
관측 연결입니다. 원본 객체의 padding을 쓰거나 좌표·시각 값이 같다는 이유로
요청을 연결하지 않습니다. 동시 요청·취소·오류·destruction·주소 재사용과
관측 저장 공간 부족까지 원본 전달을 유지하며 검사해야 합니다.

이 연결과 실제 receiver association, 검증된 센서 시간·품질·보정,
qualified 계산 결과의 runtime publication은 아직 완료되지 않았습니다.
상수 MODEL context나 성공한 API poll을 ASSIST 자격으로 승격하지 않습니다.
제품 코드 변경 없이 확인한 이번 객체 ABI를 그 미구현 부분의 완료로 세지
않으며 [v1.0 완료 조건](../docs/V1_READINESS_KO.md)을 계속 따릅니다.
