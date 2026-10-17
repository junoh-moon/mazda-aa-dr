# BCA `QueryConnectStatus` 정적 분석: 수집기가 PTY를 받지 못한 원인 (2026-10-17)

대상: 1세대 Mazda Connect NA 74.00.324A 순정 트리 사본(읽기 전용)에서 작업 폴더로 복사한 바이너리만 썼습니다. 이 기록에는 OEM 바이트와 디스어셈블리를 싣지 않았습니다. 함수 이름과 주소만 말로 적습니다. 공유 라이브러리 주소는 Ghidra 기본 적재 주소(0x10000)를 기준으로 하며, 기존 기록(`BCA_updateConnectionStatus` 0x7cb04)과 같은 기준입니다. 실행 파일 `bds`는 링크 주소 그대로입니다.

분석 입력 SHA-256(앞 16자리): `svcjcibca.so` 39e238bb578617e5, `libjcidbus.so` b44b2f462c093767, `libjcibca_client.so` e055363b4f390c38, `libjcibdsdbusservice.so` 71b8956f7b61269a, `libjcibds-interface-dbus.so` 3236aed7089ef921, `bds` 3afd74c7385023ad. 도구는 Ghidra 12.1.4 headless(디컴파일), m3 툴체인 objdump, qemu-arm 8.2.2(사용자 모드)입니다.

## 요약 (결론)

**FACT(펌웨어) + DATA(오프라인 재현): 수집기가 엉뚱한 버스에 묻고 있었습니다.** BCA는 `com.jci.bca`라는 이름을 **서비스 버스와 HMI 버스 양쪽**에서 소유하지만, `QueryConnectStatus`를 가진 객체 `/com/jci/bca`는 **HMI 버스 연결에만** 등록하고, `ConnectionStatusResp` 신호도 **HMI 버스로만** 보냅니다. 수집기는 서비스 버스(`/tmp/dbus_service_socket`)에 연결하므로, 호출은 객체가 없는 BCA의 서비스 버스 연결로 배달되고 libdbus가 즉시 `org.freedesktop.DBus.Error.UnknownMethod`로 답합니다. 신호는 서비스 버스에 아예 나오지 않습니다. 순정 시리얼 소비자 `svcjcivdtcon.so`는 HMI 버스 연결 하나만 씁니다. 분류 코드가 없는 서비스 15 때문에 생긴 오류도, 350 ms 타임아웃도 아닙니다.

## 1. `QueryConnectStatus` 처리기 (질문 1)

- FACT(디컴파일): D-Bus 쪽 진입점은 생성된 래퍼 `method_void__iuint32_obca_connectionStatus_t`(0x680f8)입니다. 인수가 정확히 1개이고 형이 uint32일 때만 서비스 함수 `BCA_QueryConnectStatus_svc`(0x6e57c)를 호출하고, 그 자리에서 결과를 `(iiii(ay))` 구조체로 묶어 성공 코드 0을 돌려줍니다. 서비스 함수의 반환값은 보지 않습니다. 인수 개수나 형이 다르면 1을 돌려줍니다.
- FACT: `BCA_QueryConnectStatus_svc`는 동기 함수입니다. 결과 버퍼를 할당하고, serviceId가 0이면 로그만 남기고 0으로 채운 구조체를 돌려줍니다. 0이 아니면 `BCA_getServiceConnectStatus`(0x83344)로 서비스 표를 읽어 serviceId, connStatus, btDeviceId, status와 21바이트 terminalPath(배열 길이는 문자열 길이+1)를 채웁니다. 대기, 타이머, 지연 응답, 오류 응답 경로는 없습니다.
- FACT: `BCA_getServiceConnectStatus`는 `BCA_getServiceIndexFromServiceId`로 표 색인을 찾고, 표 안이면 해당 행의 40바이트(serviceId, 연결 상태, 장치 ID, status, 20바이트 경로)를 복사합니다. **연결 상태가 3이면 3, 그 밖의 모든 값은 6(DISCONNECTED)으로 바꿉니다.** 표에서 못 찾으면 복사하지 않으므로 serviceId=0, connStatus=6, 빈 경로로 답합니다. 분류 코드(행의 +0x8c)는 보지 않습니다.
- FACT(`BCA_fillServiceInfo` 0x7822c): BDS 설정 표를 열거하면서 serialPort(0x4000)는 이름으로 찾은 8종에만 분류 코드를 매기지만, 모든 항목의 서비스 ID(+0x3c), 인스턴스 정보(+0x90, +0x94), 이름은 등록합니다. 따라서 서비스 15에도 행이 생깁니다. INFERENCE(높음): BCA는 `bds`에 의존해 늦게 뜨고(`sm.conf`의 `jciBCA` 의존성), `libjcibds.so`의 설정 열기는 `/tmp/BdsConfiguration.xml`을 먼저 읽으므로(기존 기록), BCA는 우리 설정의 15번을 봅니다.
- **답:** 분류 없는 서비스 15에 대해서도 **즉시, 정상 응답**합니다. 나중에 응답하지 않으며 오류로 거절하지 않고, 내부 타임아웃도 없습니다. `async="client"` 주석은 클라이언트 스텁을 비동기로 만든다는 뜻일 뿐입니다(4절).
- 이 메서드에서 생길 수 있는 D-Bus 오류 이름(FACT, `libjcidbus.so`와 순정 libdbus 1.7.2):
  - `org.freedesktop.DBus.Error.UnknownMethod`, 메시지 `Method "QueryConnectStatus" with signature "u" on interface "com.jci.bca" doesn't exist`. 받는 연결에 그 경로의 객체가 없을 때(서비스 버스, 이번 경우), 또는 HMI 버스에서 인수 형이 틀려 래퍼가 1을 돌려준 때입니다(`jcidbus_verify_match_and_prepare_reply`가 상태 6으로 바꾸고, `JCIDBUS_obj_path_msg_function`이 처리하지 않음으로 넘기면 libdbus 기본 처리가 이 오류를 보냅니다).
  - `org.freedesktop.DBus.Error.Failed`, 메시지 `Error: unpredictable error` 또는 `Error: method not found`. `JCIDBUS_reply_send_error_msg`는 이름을 항상 Failed로 고정합니다. 이 래퍼에서는 나오지 않는 경로입니다.
  - 버스 데몬이 만드는 `ServiceUnknown`/`NameHasNoOwner`(BCA가 없음), 클라이언트 쪽 `NoReply`(시간 초과).

## 2. `ConnectionStatusResp`가 나가는 조건 (질문 2)

- FACT: `BCA_SendSignal_ConnectionStatusResp`(0x7064c)는 생성 스텁 `BCA_ConnectionStatusResp`(0x68fb4)에 **전역 `hmiDbusConnPtr`**를 넘깁니다. 목적지 인수는 0이라 HMI 버스 전체로 방송됩니다. 서비스 버스로 보내는 경로는 없습니다.
- FACT: BDS 연결 신호는 서비스 버스의 BCA 연결로 들어와 `bcaBDSSignalHandler`(0x73acc)의 형식 0x82 분기에서 처리됩니다. 장치가 CarPlay 장치이거나 HFP 서비스이면서 다른 장치이면 BCA가 `BCA_SendDisconnectToBds`로 끊을 수 있습니다. 그 밖에는 `BCA_isConnectedServiceAcceptable`(0x415e8)이 받아들일지 정합니다. 받아들이면 WCP 상태기계에 메시지 8로 넘기고, `bca_WcpConnectionTopState_EventHandler`(0x58008)는 이를 **서비스 ID와 상관없이** 주 상태기계 이벤트 0x30(BDS 서비스 연결)으로 다시 보냅니다. INFERENCE(높음): 거절 분기들은 특정 서비스 슬롯(거절 표시된 서비스, AA/iAP 슬롯, HFP)과만 비교하므로, Android 폰의 서비스 15는 받아들여지고 BCA가 끊지 않습니다. 폰이 15초 동안 연결을 유지한 관측과도 맞습니다.
- FACT: 이벤트 0x30을 받은 상태 처리기는 `BCA_getEmptyServiceInstanceIndexFromServiceId`로 행을 찾고 `BCA_updateConnectionStatus`(0x7cb04)를 부릅니다. 이 함수는 status가 100이면 행의 연결 상태를 3으로 바꾸고, 장치 ID와 20바이트 PTY 이름을 복사합니다. 실패 status이면 6으로 둡니다. 분류 코드가 없으면 장치 정보 갱신만 건너뜁니다(종류 0). 신호 송신은 상태마다 다릅니다.
  - `bca_IdleConnectibleState_EventHandler`(0x2c238): 항상 `BCA_sendConnectionStatusResp`를 부릅니다(연결되었으면 3, 아니면 7=FAILED).
  - `bca_NormalAutoConnectState_EventHandler`(0x3a3e0): status가 100일 때만 부릅니다.
  - `bca_NotAllowedState_EventHandler`(0x2438c): 표만 갱신하고 신호는 보내지 않습니다.
  - 분류 코드를 검사해 신호를 거르는 곳은 없습니다. `BCA_sendConnectionStatusResp`(0x75f08)는 단일 인스턴스 서비스이면 `BCA_sendSingleConnectionStatusResp`로 그 행의 값을 그대로 보냅니다.
- **답:** BCA 자신의 ConnectRequest 뒤에만 나가는 신호가 아닙니다. 들어오는 연결(BDS 연결 이벤트)에도 서비스 ID와 무관하게 나갑니다. 다만 BCA 상태에 따라 생략될 수 있고(NotAllowed), **어느 경우든 HMI 버스로만** 나갑니다. 그래서 서비스 버스의 수집기는 신호를 하나도 받지 못했습니다(관측과 일치).

## 3. terminalPath의 출처와 BCA 없이 PTY를 얻는 길 (질문 3)

- FACT: `bds` 시리얼 서비스의 `serviceConnectHandler`(0x4c7dc)가 `COMMON_IO_Pty_Open`으로 PTY를 열고, `COMMON_IO_Pty_GetName`의 이름을 서비스 객체(+0xe0)에 20바이트까지 복사합니다. `libjcicommon_io.so`의 `common_io_pipeOpenHandler`는 `openpty`(raw 모드 선택 가능)를 부르고, 객체 플래그가 0이면 **slave fd를 곧바로 닫아** master만 남깁니다. 이름은 `openpty`가 돌려준 slave 경로입니다.
- FACT: `bds`의 D-Bus 인터페이스(`libjcibds-interface-dbus.so` `BDS_Interface_Open`)는 **서비스 버스**(버스 형식 0)에 연결합니다. `BDS_DbusInterface_Event` 형식 0x82가 `BDS_SignalConnected_cb`를 목적지 없이(방송) 보냅니다. 신호는 `com.jci.bds` 인터페이스의 `SignalConnected_cb(u type, (ay) data)`이고, `data`는 연결 이벤트 C 구조체 원본입니다(앞 8바이트를 형식 0x82와 길이로 덮어씀). BCA 소비 코드의 오프셋(FACT, `bcaBDSSignalHandler`와 `BCA_updateConnectionStatus`): +0x20 장치 ID, +0x24 서비스 ID, +0x28 서비스 형식, +0x2c status(100=성공, 0x68=실패), **+0x30 PTY 이름 20바이트**. BCA는 최대 0x9c8바이트를 복사합니다. INFERENCE: `bds` 쪽 구조체 조립 함수는 이번에 다시 추적하지 않았으므로, 오프셋은 소비자 쪽 판독입니다.
- 대안 평가:
  - (a) **HMI 버스의 BCA**(권장): 순정 소비자와 같은 경로이며 3·4절과 오프라인 재현으로 확인했습니다.
  - (b) **서비스 버스의 `com.jci.bds` `SignalConnected_cb`/`SignalDisconnected_cb` 직접 구독**: BCA 없이 가능하고, 수집기가 이미 쓰는 버스입니다. 단점은 원시 C 구조체 오프셋에 의존한다는 것과 방송 신호라 놓치면 질의로 되찾을 방법이 확인되지 않았다는 것입니다(`serviceStateConnected`의 명령 5가 +0xe0부터 정보를 복사하는 질의 경로로 보이나 D-Bus로 노출되는지는 추적하지 않았습니다).
  - (c) `/proc/<bds pid>/fd`: slave fd는 기본으로 닫히므로 `/dev/ptmx` master만 보입니다. 오래된 커널의 fdinfo에는 tty 색인이 없을 것입니다(INFERENCE, 커널 버전 미확인). 어느 서비스의 PTY인지도 알 수 없어 쓸 수 없습니다.
  - (d) 다른 순정 서비스: 확인한 소비자(`svcjcivdtcon.so`)는 모두 HMI 버스의 BCA를 거칩니다.

## 4. 순정 클라이언트의 호출 방식 (질문 4)

- FACT: `libjcibca_client.so`의 `BCA_QueryConnectStatus`(0x18a28)는 목적지 `com.jci.bca`, 경로 `/com/jci/bca`, 인터페이스 `com.jci.bca`로 uint32 하나를 보내고, `JCIDBUS_method_send_async_with_notify`에 **타임아웃 -1**(libdbus 기본값, 보통 25초)을 줘서 비동기로 기다립니다. `BCA_GetConnectStatus`도 같습니다.
- FACT: `svcjcivdtcon.so`의 `connectHmiDbus`(0x49e34)는 `JCIDBUS_conn_connect(..., 1, 0)`, 즉 **HMI 버스**에 연결해 그 연결 하나로 BCA를 호출하고 신호를 받습니다. `processConnectionInfo`(0x69228)는 연결 상태가 3일 때만 terminalPath를 받아 씁니다(기존 판독과 일치).
- FACT(`libjcidbus.so`): `JCIDBUS_conn_connect`의 세 번째 인수는 0=`JCI_SERVICE_BUS`, 1=`JCI_HMI_BUS` 환경변수 주소입니다(`conn_by_env_name`). 이름은 `acquire_name`이 `DBUS_NAME_FLAG_DO_NOT_QUEUE`로 요청하며, 1차 소유자가 아니면 연결이 실패합니다. BCA는 `BCA_ServiceDbusInit`(0x6b1a0, 버스 0)과 `BCA_HmiDbusInit`(0x6aeac, 버스 1) 둘 다에서 `com.jci.bca`를 요청하고, 둘 다 성공해야 `ServiceInit`이 계속됩니다. 따라서 서비스 버스의 `GetNameOwner(com.jci.bca)`는 성공하며 수집기가 이를 정상으로 오인할 수 있습니다. 객체 등록 `BCA_object_register`(0x683c8)의 호출자는 `BCA_HmiDbusInit` 하나뿐입니다. HMI 버스 주소는 순정 `hmi.conf`의 `unix:path=/tmp/dbus_hmi_socket`이며 정책은 모든 사용자 허용, 익명 허용입니다.
- **350 ms가 짧은가:** 이번 실패와는 무관합니다. 처리기가 동기라서 정상 경로의 지연은 BCA D-Bus 폴 스레드의 반응 시간뿐입니다. 오프라인 qemu 재현에서는 52 ms였습니다(DATA). 차량에서 BCA 폴 스레드가 얼마나 막히는지는 모르므로(미확인), 순정처럼 비동기로 기다리거나 상한을 1 s 정도로 늘리는 것은 선택 사항입니다.

## 5. 설명의 순위 (질문 5)

1. **잘못된 버스(확정에 가까움).** 근거: 위 FACT들(객체는 HMI 연결에만 있고, 신호도 HMI로만 나가며, 이름은 양쪽에서 소유, 순정 소비자는 HMI만 사용). 그리고 DATA: 순정 `libjcidbus.so`와 순정 libdbus 1.7.2로 BCA의 두 연결을 그대로 만들고(`JCIDBUS_conn_create/connect`, 버스 0과 1), HMI 연결에만 **순정 `BCA_object_register`**(svcjcibca.so 안의 함수를 그대로 호출)로 객체를 등록한 뒤 qemu-arm으로 실행했습니다. 호스트 dbus-daemon 두 개를 띄워 시험했고 결과는 다음과 같습니다.
   - 서비스 버스: `GetNameOwner`는 성공. `QueryConnectStatus(u 15)`는 37 ms 만에 `UnknownMethod ... doesn't exist`.
   - HMI 버스: 52 ms 만에 `(iiii(ay))` 응답(서비스 표가 초기화되지 않은 상태라 `0,6,0,0,""`).
   - 순정 `BCA_ConnectionStatusResp`로 서비스 15, 상태 3, `/dev/pts/7`을 보내면 HMI 버스에서만 `u,u,u,u,(ay 21바이트)` 신호가 보이고 서비스 버스에서는 0건.
   - 수집기의 `why=error`(dbus 오류가 설정됨)와 신호 0건, 5초마다 같은 결과라는 관측 모두와 맞습니다.
2. **NoReply(350 ms 시간 초과): 기각.** 처리기는 동기이고, 서비스 버스에서는 libdbus가 즉시 UnknownMethod로 답합니다. BCA는 두 연결을 모두 폴 대상에 넣습니다(`BCA_PollDbusConnInit`). 남는 가능성은 BCA 폴 스레드가 통째로 멈춘 경우뿐이고, 이번 관측을 설명할 필요가 없습니다.
3. **분류 없는 서비스에 대한 오류: 기각.** 처리기는 serviceId 때문에 오류를 내지 않습니다. 표에 없으면 serviceId=0, 상태 6으로 정상 응답합니다.
4. 호출 형식 문제: 기각. 서명 `u`와 반환 `(iiii(ay))`가 맞습니다.

**한 줄 기록으로 판정하는 법:** `pimu_link ev=poll_error` 행에 `dbus_error.name`(그리고 메시지 앞 80자)을 넣으면 됩니다.
- `org.freedesktop.DBus.Error.UnknownMethod` + `signature "u" ... doesn't exist`: 잘못된 버스(1번)로 확정.
- `NoReply`: 시간 초과(2번).
- `ServiceUnknown`/`NameHasNoOwner`: 이 버스에 BCA가 없음.
- `Failed`: BCA/JCIDBUS 내부 오류.
- `InvalidArgs`/`UnknownMethod`이면서 `signature`가 `u`가 아님: 호출 형식 문제.

참고(기록 공백, FACT, `phone_imu_rx.h`): 응답이 성공해도 serviceId가 15가 아니면 `handle_status`가 행을 남기기 전에 `sig_other_`만 올리고 돌아갑니다. 실패 행은 1·3회째만 남습니다. 따라서 "그 뒤 행이 없음"만으로 모든 호출이 실패했다고 단정할 수는 없습니다. 이번에는 서비스 버스라 모두 실패했을 것입니다(위 재현). HMI 버스로 바꾸면 BCA 표에 15가 없을 때 serviceId=0 응답이 조용히 버려질 수 있으므로, 함께 기록해야 합니다.

## 6. 권장 수정과 오프라인 검증 (질문 6)

### 최소 수정(수집기, `src/collector/phone_imu_rx.h`·`collector.cpp`)
1. BCA 관련 D-Bus(이름 소유자 조회, `ConnectionStatusResp` 매치와 `NameOwnerChanged` 매치, `QueryConnectStatus`)를 **HMI 버스 `unix:path=/tmp/dbus_hmi_socket`의 별도 private 연결**로 옮깁니다(`RxOptions`에 `bca_bus_address`를 더하고 기본값을 HMI로, 시험용 `--bca-bus-address` 인수 추가). bds 감시, 위치 서비스 등 나머지는 서비스 버스 그대로 둡니다. 순정 소비자 `svcjcivdtcon.so`와 같은 경로이며, 순정 상태를 바꾸지 않는 읽기 전용 질의입니다.
2. `poll_error` 행에 오류 이름과 메시지 앞부분(길이 상한, 토큰 버킷 유지)을 넣습니다.
3. 성공 응답의 serviceId가 15가 아닐 때(특히 0, 즉 BCA 표에 15가 없음)도 첫 1회는 `pimu_link ev=poll why=other svc=<n> cs=<n>` 행을 남깁니다. PTY는 열지 않습니다(다른 서비스의 PTY를 열지 않는 규칙 유지).
4. 연결 상태 해석은 그대로 둡니다. 다만 질의 응답은 3 또는 6만 나온다는 점(FACT: `BCA_getServiceConnectStatus`)을 주석에 적습니다. CONNECTING(2)은 신호에서만 올 수 있습니다.
5. 타임아웃은 350 ms를 유지해도 됩니다(재현 52 ms). 수집기 주 루프를 막지 않으려면 1 s 이하로 유지합니다. 순정처럼 `dbus_connection_send_with_reply` + pending call로 바꾸는 것은 선택 사항입니다.
6. 선택(기록 전용 진단): 서비스 버스에서 `com.jci.bds` `SignalConnected_cb`/`SignalDisconnected_cb`를 구독해 서비스 15 항목의 길이, 서비스 ID, status, 경로 문자열만 기록합니다. PTY 열기는 BCA 경로만 씁니다. BCA가 NotAllowed 상태라 신호를 생략하는 경우의 증거가 되고, 2차 경로(b)의 오프셋을 차량에서 검증할 수 있습니다.

### 오프라인에서 검증할 것
- 기존 `tests/collector/fake_bca.c` 시험은 같은 버스 주소 하나에 가짜 BCA를 두므로 이 결함을 잡지 못합니다. 시험을 두 버스(서비스, HMI)로 나누고, 서비스 버스에는 **이름만 소유하고 객체가 없는** 가짜(현재 BCA의 서비스 연결과 같은 모양)를 두어 수집기가 HMI 쪽에서만 PTY를 얻는지 확인해야 합니다.
- **실제 순정 바이너리로 가능한 범위(DATA, 이번에 실행):** qemu-arm 사용자 모드, 순정 루트를 `-L`로 지정, `LD_LIBRARY_PATH=/jci/lib:/usr/lib:/lib`. 순정 `libjcidbus.so`의 연결과 이름 획득, 순정 libdbus 1.7.2의 미처리 메서드 응답, `svcjcibca.so`의 `BCA_object_register`와 실제 래퍼·`BCA_QueryConnectStatus_svc`·`BCA_ConnectionStatusResp`까지 실행됩니다(`dlopen` RTLD_LAZY 후 내부 심볼 주소 계산). 작업 폴더의 `qtest/srv.c`가 그 하네스입니다. 이를 ARM 시험으로 옮겨, 실제 ARM collector 빌드가 HMI 버스에서 응답을 해석하고 신호(서비스 15, 상태 3, `/dev/pts/N`)로 PTY를 여는지 검사할 수 있습니다.
- **어려운 범위:** `svcjcibca.so`의 `ServiceInit` 전체(SM 서비스 호스트 API, 설정 레지스트리, 장치 관리자 `BCA_DmOpen`, BDS 설정 읽기, 상태기계)를 qemu에서 돌리는 일은 의존성이 많아 이번에 시도하지 않았습니다. 서비스 표를 채운 상태(행 15, 상태 3, 경로)의 응답을 실제 코드로 보려면 표 초기화(`BCA_setConfiguration` → `BCA_fillServiceInfo`, `libjcibds.so`의 설정 읽기)만 따로 부르는 하네스가 다음 단계입니다. 미검증으로 남깁니다.

## 남은 불확실성
- 차량의 BCA가 어떤 상태에서 서비스 15 연결 이벤트를 받는지(NotAllowed이면 신호 없음, 질의는 정상)는 모릅니다. 폴링을 유지해야 하는 이유입니다.
- 오류 이름은 차량에서 기록되지 않았습니다. 1번 설명은 펌웨어 판독과 순정 라이브러리 오프라인 재현에 근거하며, 차량 확인은 다음 기록(오류 이름 행)으로 합니다.
- BDS 신호 구조체 오프셋은 BCA 소비자 쪽 판독입니다.
- HMI 소켓 권한: 순정 `hmi.conf`는 익명과 모든 사용자를 허용하지만 실제 소켓 파일 모드는 차량에서 확인하지 않았습니다(서비스 소켓과 같은 데몬 기본값일 것으로 추정).
- QEMU/합성 시험의 성공은 차량 검증이 아닙니다. 이 기록은 차량이나 폰을 쓰지 않았습니다.
