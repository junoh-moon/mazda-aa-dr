# 원본 AA 로컬 세션·송신 API 실행 — 2026-09-30

Codex가 격리 ARM VM에서 원본 `RaceAap::Init/UnInit`과
`VehicleDataManager::OrderSendVehicleData`를 실행했습니다. 실제 하위 송신
함수의 반환은 **세션 생성 전 256 → 생성 후 0 → 파괴 후 256**이었습니다.
기존 제품의 OBSERVE 후크가 세 호출의 입력과 반환을 보존했습니다.
폰 연결 없이도 0을 반환했으며, 이 값은 폰 수용을 증명하지 않습니다.

[원본 BLM 큐 시험](BLM_QUEUE_2026-09-30.md) 뒤의 별도 시험입니다.
그 시험의 send 0건과 이번의 명시적 send 호출을 구분합니다. 이번에는
LDS 요청·위치 callback에서 송신까지 이어지는 전체 활성 경로를 실행한 것이
아닙니다. Claude의 추가 커밋을 확인했으며 `e5d87c1` 이후 새 조사 커밋은
확인되지 않았습니다. 이번 실행과 결과 검사는 Codex가 직접 수행했습니다.

## 시험 방법과 실제 호출

NA 74.00.324A 원본 rootfs와 Linux 3.0.35를 QEMU 7.2.22/Sabrelite에서
실행했습니다. 호스트 장치·네트워크·공유 디렉터리를 연결하지 않았습니다.
기존 kernel-entry machine ID 조정 뒤 userspace 전에 debugger를 분리했습니다.
원본 D-Bus와 별도 `aap_service`를 실행했으며, 시작한 PID의 생존과 실행 파일을
확인했습니다. 정상 SM 의존성 그래프나 물리 USB 기동을 대체하지 않습니다.

고정 GCC 4.9.1·ARMv7 softfp로 작성한 fixture는 원본 common-util·BLM을
NOW 로드합니다. 원본 해시·load bias·함수 진입 바이트를 검사하고 기존 제품
adapter·ARM veneer·install_v74로 OBSERVE 후크를 설치했습니다. 제품 preload
전체를 실행한 시험은 아닙니다. 원본 singleton/getter로 받은 객체를 사용하며
세션 포인터·callback table·활성 플래그를 수동으로 만들거나 고치지 않았습니다.

실행 순서는 다음과 같습니다.

1. 기존 원본 큐 시험의 작성한 작업 64개와 합성 위치 callback 3개를 완료합니다.
   위치 callback은 실제 원본 worker를 거칩니다. 이 단계에는 send가 없습니다.
2. 원본 OrderSendVehicleData를 type 1·48바이트의 0값 입력으로 호출합니다.
3. 원본 RaceAap::Init(true)를 호출하여 원본 표준 XML로 로컬 세션을 생성합니다.
   원본 함수가 callback table을 구성하고 `aap_create_session`을 호출합니다.
4. 같은 원본 송신 API를 호출한 뒤 원본 UnInit으로 세션을 파괴합니다.
5. 같은 송신 API를 다시 호출하고 원본 큐를 stop·join한 뒤 정상 종료합니다.

`Init` wrapper의 true만으로 생성 성공을 판정하지 않았습니다. 정적 코드에서
그 wrapper는 특정 재시도 이후의 모든 오류를 bool 반환으로 구분하지 않습니다.
이번 생성 전후의 실제 하위 반환, 서비스 출력과 IPC 실행을 함께 검사했습니다.
UCP XML은 파일 해시만 확인했으며 실행하지 않았습니다.

## 실패한 가정과 수정 후 결과

최초 r1 fixture는 폰이 없으면 생성 후 send가 269(0x10d)를 반환할 것으로
잘못 예상했습니다. 실제 반환은 0이었고 fixture는 check 37에서 종료 1로
실패했습니다. 이때 파괴 단계는 실행하지 않았습니다. 실패 소스·바이너리·
console과 판정을 별도로 보존했습니다.

원본 interface의 해당 분기는 **로컬 서비스가 실행 중인지**를 검사합니다.
폰의 연결 상태를 검사하는 것으로 해석한 것이 오류였습니다. r2는 이 계약을
정정했으며, 성공 반환을 만들기 위해 OEM이나 제품 코드를 수정하지 않았습니다.

| r2 실행 | 원본 send 반환 | 관측·종료 |
| --- | --- | --- |
| 추적 없이 실행 | 256 / 0 / 256 | position 3·send 3, 71 checks, fixture 종료 0 |
| 원본 strace를 붙여 실행 | 256 / 0 / 256 | position 3·send 3, 71 checks, fixture 종료 0 |

각 호출마다 실제 하위 후크 event는 정확히 하나였습니다. 반환·type·길이·
original/outgoing 48바이트가 호출과 일치했습니다. 직접 송신은 위치 callback의
TLS 밖에서 호출했으므로 `ORIGINAL`, `NO_CONTEXT`, call_sequence 0입니다.
이를 실제 위치 요청에 연결된 send로 세지 않습니다.

각 실행에서 작성한 작업/완료 표식 67개가 FIFO·각 1회·같은 원본 작업
스레드에서 실행됐고 캡처 생성/파괴는 402/402였습니다. 합성 위치 세 개의
9개 필드가 보존됐습니다. 큐 stop=100·join=0을 확인했습니다. 이 계수는
작성한 캡처에 한정하며 OEM 전체 allocation/free의 검사 수치가 아닙니다.

## 송신 성공과 서비스 처리·종료의 한계

생성 후 send의 0은 로컬 요청 제출의 결과입니다. 서비스 로그에는 세션이
아직 시작되지 않았고 현재 세션 상태에서 요청을 처리할 수 없다는 오류가
기록됐습니다. 따라서 **이 시험에서 위치가 폰에 전달됐다고 주장할 수 없습니다.**
`aap_start_session`·물리 폰 연결·폰 구독과 수용은 실행하지 않았습니다.

원본 strace 4.6으로 fixture와 직접 시작한 서비스의 SysV IPC를 관측했습니다.
클라이언트의 성공한 msgsnd 3건, 같은 표시 queue ID를 사용한 서비스의 수신
완료 3건, 클라이언트의 reply 수신 완료 2건을 확인했습니다.
그러나 구형 tracer가 성공한 msgsnd의 buffer를 NULL로, 포인터를 길이로,
실제 길이를 flags로 표시하는 등 인자를 잘못 해석합니다. 이 출력으로 크기·
flags·본문 바이트 또는 요청별 본문 대응을 검증했다고 판정하지 않습니다.
서비스 stdout도 실행 중 읽어 마지막 부분이 잘렸습니다. 전체 서비스 로그나
모든 오류를 수집했다고 주장하지 않습니다.

파괴 호출은 복귀하고 fixture도 종료 0이었지만 **오류 없는 정리는 미검증**입니다.
추적 없는 실행에서는 원본 mutex destroy 오류 16, 추적 실행에서는 server-event
대기 오류 0x108과 thread cancel 오류 3이 기록됐습니다. 원인과 영향은 아직
분리하지 않았으며, tracing 때문이라고 단정하지 않습니다.

## 검증 범위와 남은 작업

검사기는 입력·initrd·console 해시, 격리 조건, 두 case의 전체 callback 순번·
위치 필드·송신 반환과 정상 fixture 종료를 대조했습니다. 원본 정리 오류도
결과에 남겼습니다. 검사기 첫 실행은 잘린 서비스 로그 뒤에 END 표식이
줄바꿈 없이 붙은 형식을 처리하지 못했습니다. 표식 경계를 수정한 뒤 전체
검사를 통과했으며 불완전한 로그라는 판정을 유지했습니다.

r1/r2 VM은 각각 150.025/180.035초 뒤 바깥 runner 제한으로 종료 124,
QEMU 종료 0·timed_out=true였습니다. runner 종료값은 통과 기준이 아닙니다.
통과 판정은 r2의 명시된 API 검사 범위에 한정합니다.

AapProc::Init·VehicleDataManager 초기화/송신 시작, 실제 LDS 요청부터 활성
위치 송신까지의 연결, 제품의 요청 identity 연결, 오류 없는 세션 정리,
qualified provenance·폰 수용은 남아 있습니다. 기존 `provenance=false`와
`allow_assist=false`를 변경하지 않았고 [v1.0 조건](../docs/V1_READINESS_KO.md)을
완료 처리하지 않습니다.

제품 소스·설치 ZIP은 변경하지 않았습니다. 전체 make test·제품 ARM 회귀는
재실행하지 않았으며, 이번 추가 검사는 두 ARM fixture 빌드·두 VM 실행과
기록 검사입니다. 이번 fixture의 새 독립 Claude/Codex 리뷰는 수행하지 않았습니다.

## 비공개 실행 자료의 식별자

소스 기준은 `02c0c9fec3b1d5284c8e3a64e9100a9d933d2676`입니다.
원본 BLM·커널·rootfs는 앞선 큐 기록과 같습니다. 추가로 검사한 원본 입력은
`aap_service` SHA-256
`a0187afab34eaf0e0fb84db6974162465004ad1227ad7866eeb15ac7672546cd`,
interface `e9eb5e0d42719c98efc5ef86a270b4b3c86467aced55d4c8158bd99e06bbd436`,
표준 XML `bc852037615dcb4e4eb24ab6f30a2e6f817fe00a22a2f767478b25f75b036812`입니다.

| 실행 | ARM fixture SHA-256 | initrd SHA-256 |
| --- | --- | --- |
| r1 실패 | `1d91afd139eee791241e98ac7f5164ef33427f046e7876e16dd048ab1a39571a` | `3e8202039c98f9512b4d97482a5e4bdc0b280607504daada2c426f2c9dea10c2` |
| r2 | `9e634a1da4977ea4af8a642b98973db32f1ec5a6b9413330eeac444e0419d1a7` | `b2c56c5be73e357605785730099e934ec8f51ddad1cfd4e92524b9c6380bc70a` |

r1 console SHA-256은
`1389d439ca23bfbfaa7f0627358320defb3c52ee47c8cdc00b60251fe584b83c`,
r2는 `441ffbd7260a98edf26bcc2439f588a0fc4d25cc3290ac3bd824e8669793c21a`입니다.
작성한 fixture·검사기와 전체 비공개 빌드/실행 기록은 evidence에 보존했습니다.
OEM 바이너리·설정 본문·디스어셈블리·전체 로그는 게시하지 않습니다.
