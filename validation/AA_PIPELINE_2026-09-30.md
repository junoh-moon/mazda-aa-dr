# 원본 AA manager의 자동 LDS 요청·위치 송신 실행 — 2026-09-30

Codex가 격리 VM에서 원본 **VehicleDataManager의 자동 LDS 요청 → BLM callback →
작업 큐 → 위치 worker → RequestSendPosition → 실제 하위 AA send**를 실행했습니다.
앞선 [로컬 세션 API 시험](AA_SESSION_2026-09-30.md)의 직접 송신 호출에서
원본 관리자의 자동 위치 처리까지 범위를 확장했습니다. 폰 수용·유효 GPS·
실제 요청 identity의 제품 연결 또는 v1.0 완료를 뜻하지 않습니다.

## 실행 방법

- NA 74.00.324A 원본 rootfs·Linux 3.0.35, QEMU 7.2.22/Sabrelite를 사용했습니다.
  호스트 장치·네트워크·공유 디렉터리가 없는 guest입니다. 기존 kernel-entry
  machine ID 조정 뒤 userspace 전에 debugger를 분리했으며 userspace 추적기는
  붙이지 않았습니다. 원본 D-Bus·LDS launcher·aap_service를 실행했습니다.
- 작성한 ARM fixture는 해시와 실제 매핑·진입 바이트를 검사하고 원본 BLM
  singleton/getter로 객체를 얻습니다. 제품 adapter·ARM veneer·install_v74를
  그대로 링크하고 생산자 시작 전에 기존 OBSERVE 후크를 설치했습니다.
  전체 production preload·collector 또는 정상 SM 그래프 시험은 아닙니다.
- 원본 Dbus::Init에 원본 AAPA_object_register를 전달했습니다. 실제 HMI/service
  연결과 conn-poll 생성, 반환 100·IsInitialized를 확인했습니다. 진단 프로그램이
  원본 getter의 두 connection을 원본 JCIDBUS_dispatch로 처리했습니다.
  원본 AapProc::Init·ServiceRun 전체를 실행한 것으로 세지 않습니다.
- 원본 RaceAap::Init(true)로 표준 XML의 로컬 세션을 생성했습니다. 원본
  VehicleDataManager의 Init·StartSendVehicleData·StopSendVehicleData·UnInit은
  원본 큐에 실제 C++ std::function을 제출해 작업 스레드에서 호출했습니다.
  manager의 활성 byte·세션 포인터·객체 내부를 작성한 코드로 덮어쓰지 않았습니다.
- 관리자의 시작 API를 진단 프로그램이 명시적으로 호출했습니다. 폰이 연결되어
  정상 AA 연결 사건이 이 시작을 유발했다고 주장하지 않습니다. 위치 요청은
  시작 API가 만든 **원본 위치 스레드**가 발행했으며, fixture가 LDS 요청을
  수동으로 반복 발행하거나 LDS 응답을 바꾸지 않았습니다.

## OBSERVE 실행 결과

최초 빌드 r1은 구형 runtime의 clock_gettime에 필요한 librt 링크가 없어
실패했습니다. 뒤따른 staging/build도 바이너리 부재로 실패했으며 guest는
실행되지 않았습니다. `-lrt`를 명시한 r2는 고정 GCC 4.9.1·ARMv7 softfp로
빌드하고 순정 libstdc++·libc·librt에서 실행했습니다.

| 항목 | r2 실제 관측 |
| --- | --- |
| 원본 LDS 요청/reply | GetPosition 6건, 원본 제공자의 응답 6건. 9개 필드 모두 0 |
| 위치 후크 | 실제 LDS mode 0 여섯 건 + 명시한 합성 mode 2 한 건 |
| 기존 관측 후크 | 총 56 events: position 7, send 49 |
| native LOCATION | 합성 위치의 송신 1건 + 이후 실제 mode 0의 캐시 재송신 3건 |
| 그 외 send | type 21 일곱 건, type 3 서른다섯 건, type 7/10/13 각 한 건 |
| 실제 send 반환 | 49건 모두 0. 로컬 제출 반환이며 폰 수용은 아님 |
| 정지·종료 | 추가 task 2개 소멸, 관측 수 안정, 큐 stop=100·join=0, fixture 종료 0 |

시작 직후에는 원본 LDS의 mode 0 위치 세 개가 들어왔습니다. 원본이 생성한
type 21 위성 데이터 호출은 있었지만 type 1 LOCATION 호출은 없었습니다.
그 다음 원본 BLM callback에 **시험용** mode 2 위치 한 개를 전달했습니다.
UTC 1700000000, 위도 37.5·경도 127.25, 고도 12, 방향 45.25, 속도 36.5,
horizontal 1.25·vertical 2.5입니다. 실제 GPS 측정값이 아닙니다.

그 합성 callback도 원본 worker와 같은 큐를 거쳐 실제 MakeLocation/send를
실행했습니다. 이후 원본 LDS는 다시 mode 0과 0값을 응답했습니다. 이 세 응답의
LOCATION은 이전 캐시를 복사하고 accuracy 표시·값만 지웠습니다. **이전 좌표·
속도·방향과 hasSpeed/hasBearing=true는 남았습니다.** OBSERVE는 그 native
48바이트를 그대로 전달했습니다. 이전 정적 분석의 캐시 재송신을 실제 원본
함수 경로에서 재현한 결과이며, 제품 후크가 새로 만든 좌표가 아닙니다.

모든 위치 후크와 연결된 LOCATION/type 21 후크는 동일한 원본 큐 스레드에
있었고, position의 call_sequence·mode가 동기 send까지 일치했습니다. 별도
속도 스레드의 type 3 송신은 sequence 0이며 이 위치 scope와 연결하지 않았습니다.
이 TLS scope 검사는 LDS 비동기 요청부터 worker까지의 identity 구현을 대신하지
않습니다. 실제 제품의 request_trace 연결은 여전히 **미구현**입니다.

## 기존 SCRUB의 같은 원본 경로 회귀

r3는 같은 초기화·OBSERVE·합성 seed·자동 mode 0 처리 뒤, fixture 안에서 기존
SCRUB_STALE을 선택하고 실제 LDS 응답 세 건을 더 처리했습니다. 시작·중지와
버스 응답을 포함한 전체 실행을 새 guest에서 다시 수행했습니다. 제품 설정이나
배포 기본 모드를 변경하거나 새 보호 로직을 추가하지 않았습니다.

- 실제 GetPosition/reply 9건과 합성 seed 한 건이 position 10개로 이어졌습니다.
  버스 serial은 10/13/16/19/22/25/28/31/34이며 실제 제공자의 9개 0값 응답을
  대조했습니다. 총 관측 139개 중 send는 129개였습니다.
- LOCATION 7개 중 처음 4개는 OBSERVE로 전달했고 마지막 3개는 기존 SCRUB으로
  전달했습니다. 각 mode 0의 원본 캐시에는 이전 속도·방향과 두 유효 표시가
  남아 있었으며, outgoing에서는 hasSpeed·hasBearing과 두 값만 0이 됐습니다.
  native timestamp·좌표·고도·accuracy·padding 등 다른 모든 바이트를 대조했습니다.
- type 21은 10개, type 3은 109개, type 7/10/13은 각 하나였습니다. 모든 실제
  하위 반환은 0이었습니다. 이 빈도·개수는 VM의 이번 실행 결과이며 센서 cadence나
  실차 처리 지연의 보장이 아닙니다.
- 추가 task 두 개의 소멸, 관측 중단, 큐 stop=100·join=0, fixture 종료 0을
  다시 확인했습니다. native original/outgoing을 각각 기록하여 검사했습니다.

## 버스 응답·native 데이터·폰 수용의 구분

원본 dbus-monitor로 진단 연결 이름의 소유자를 식별했습니다. 그 연결의
GetPosition serial 10/13/16/19/22/25마다 destination·reply_serial을 대조하고,
reply sender가 실행 전 조회한 원본 LDS data 제공자의 소유자와 같은 것을
확인했습니다. 여섯 reply의 실제 타입 순서·9개 0값도 검사했습니다.
이 요청들에 앞선 GetSelectedGPS_sync와 GetDRUnitStatus_sync 조회도 있었지만,
독립 조회를 원자적인 수신기·측정 출처 자격으로 바꾸지 않았습니다.

관측한 LOCATION은 **하위 API에 전달하는 native 48바이트 구조체**입니다.
합성 seed의 UTC가 이 구조체에는 1700000000000000000ns로 들어갔고 mode 0
캐시에도 남았습니다. 이후 protocol encoder의 wire timestamp나 본문, 폰의
실제 수신·사용을 검사한 결과가 아닙니다. 원본 서비스는 시작되지 않은 AA
세션 상태에서 요청을 거절하는 로그를 남겼습니다. send=0을 수신 성공으로
승격하지 않습니다.

## 정지·정리와 남은 한계

원본 StartSendVehicleData는 위치와 속도용 detached thread를 만듭니다.
StopSendVehicleData는 활성 상태를 내리고 속도 callback을 해제하지만 thread
join을 수행하지 않습니다. fixture는 자신의 `/proc/self/task` 목록으로 시작
전 6개·시작 후 8개·정지 후 같은 6개를 확인했습니다. 추가된 두 task가 모두
사라진 뒤 버스 처리·큐 완료 표식을 거치고 1.2초 추가 관측 창에서 event 수가
변하지 않는 것도 검사했습니다. 이 창은 차량에서의 종료 지연 상한이 아닙니다.

이후 원본 manager UnInit, RaceAap UnInit, Dbus UnInit과 큐 stop/join을
수행하고 프로세스가 정상 종료했습니다. 다만 세션 파괴 중 r2에서는 원본의
server-event 대기 오류 0x108·thread cancel 오류 3, r3에서는 mutex destroy
오류 16이 다시 기록됐습니다. 원인·누수·다른 종료 경쟁까지 해결했다고 세지
않습니다. 모든 pending 객체의 정리를 관측한 시험도 아닙니다.

물리 센서·유효 LDS fix·GPS 단절 정확도·정상 AA 연결 사건·폰 수용·전체 SM
기동은 미검증입니다. 제품의 실제 요청 identity와 receiver/session 자격 연결은
아직 없습니다. 진단 코드에서도 provenance를 제공하지 않았고 ASSIST는 켜지
않았습니다. [v1.0 완료 조건](../docs/V1_READINESS_KO.md)은 남아 있습니다.

## 검사와 비공개 자료

결과 검사기는 빌드/소스/initrd/console 해시, 격리 설정, callback 순서·필드,
동기 position/send scope, native 바이트와 버스 요청/reply, task 소멸·정상
fixture 종료를 대조했습니다. r2의 1,463 assertion 평가는 polling 반복을
포함하므로 독립 시험 1,463개가 아닙니다. dispatch loop는 1,972회였습니다.
r3는 같은 방식으로 assertion 평가 3,043회·dispatch loop 2,951회였으며
추가로 SCRUB의 원본/선택 바이트 전체를 검사했습니다.

r2/r3 VM은 guest 완료 뒤 외부 runner 제한으로 각각 180.026/180.013초에 종료했습니다.
runner=124·QEMU=0·timed_out=true는 통과 기준이 아닙니다. 원본 오류도 결과에
보존했으며 통과는 위의 명시된 fixture 검사 범위에 한정합니다.

제품 소스·설치 ZIP은 변경하지 않았고 전체 make test·제품 ARM 회귀는
재실행하지 않았습니다. 이번 결과는 성공한 진단 ARM 빌드 두 개·VM 실행 두 번과
결과 검사입니다. 첫 링크 실패를 포함한 세 빌드 시도를 구분해 보존했습니다.
Codex가 직접 실행했으며 이번 fixture의 새 독립 Claude/Codex 리뷰는 없습니다.
Claude의 조사 커밋도 함께 확인했으며 `e5d87c1` 이후 추가분은 확인되지 않았습니다.

소스 기준은 `3fea3807ff2f8d6d69040b8a4e97de0bdb943f21`입니다. 원본 입력 해시는
앞선 AA 세션·BLM 큐·LDS API 기록과 같습니다. 작성한 fixture와 빌드·실행·검사
기록은 비공개 evidence에 보존합니다. OEM·전체 bus/console·disassembly는
게시하지 않습니다.

| r2 산출물 | SHA-256 |
| --- | --- |
| ARM fixture | `d396c0acaaa07db871c4cf9619fceaa0cd7c4e58c4272fa20084beaea1b9c77e` |
| initrd | `e91e907b0062ff6f1ac8f03a68a1ccc5a0ef34355b1d79be1ce33380c3575412` |
| console | `247dd4c55af531c7c41a482089b0f06ea483c7f9d329b529bab06b01c11423bb` |

| r3 산출물 | SHA-256 |
| --- | --- |
| ARM fixture | `41b81b917650c0193060da5d4373ef49e88aedc9d0bbc7638cec9ee3b867cfc2` |
| initrd | `fc53d0f728cd6157cf9472d3229effdd25b3bf6040bd618d709f05200ce8553a` |
| console | `e1f8a2673f5c0a26ba2920edf8decc53b0bcb88b719c8596f29cf81563e30549` |
