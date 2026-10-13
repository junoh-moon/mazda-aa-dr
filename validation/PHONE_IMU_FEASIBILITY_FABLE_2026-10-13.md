# 휴대폰 IMU(Bluetooth) yaw 공급 경로의 타당성 재검토 — 독립 2차 실행(Fable), 2026-10-13

대상은 1세대 Mazda Connect **NA 74.00.324A**이며, 추출된 순정 루트 트리와 공개 문서, 저장소의 기존 기록만 사용했습니다. **차량·폰·동글·DHU는 사용하지 않았고 저장소 코드와 차량 설정도 바꾸지 않았습니다.** 본 기록은 차량 방문, 주행, 발행, 자격 ASSIST 활성화를 승인하지 않습니다. 독립 규칙에 따라 다른 실행의 기록(`PHONE_IMU_FEASIBILITY_2026-10-13.md`)과 그 scratch는 읽지 않았습니다. 작업 노트와 원자료 경로는 `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/pff.ypKHux/NOTES_pff.md`입니다(디컴파일 출력이 들어 있으므로 저장소로 옮기지 않습니다).

표기: **FACT** = 펌웨어 파일·함수·문자열·설정 또는 공개 문서(URL), **DATA** = 이 기록에서 계산·실행한 값(방법 명시), **COMMUNITY** = 포럼·언론·제조사 안내 수준, **INFERENCE** = 추론. 기존 기록에서 가져온 사실은 출처 기록을 밝힙니다.

## 0. 판정 요약

**실행 가능하나 미검증 위험 X가 있는 경로**로 판정합니다. 펌웨어 쪽에서 차단 요인은 찾지 못했습니다.

- CMU 쪽: 순정 `bds`는 시작 인수 `-configFilePath <경로>`를 지원하고, 그 파일을 `/tmp/BdsConfiguration.xml`로 복사한 뒤 그 복사본을 읽습니다(디컴파일 + 문자열 + 원본 바이너리를 qemu-arm으로 실행해 3중 확인). 따라서 **순정 `/jci/bds/BdsConfiguration.xml`을 건드리지 않고**, 가드가 매 부팅 만드는 trial `sm.conf` 복사본의 `bds` 인수에만 옵션을 더해 연구용 serialPort(ID 15, 128비트 UUID)를 선언할 수 있습니다. 설정 항목 한계는 serialPort 20개(현재 7개)이며, 수신 PTY 경로는 순정 BCA가 D-Bus 신호 `ConnectionStatusResp`의 `terminalPath`로 내보냅니다.
- 데이터 경로: 32바이트 프레임, 50 Hz(1.6 kB/s)로 충분하며, PTY 경로의 처리 지연은 1 ms 미만(BDS식 분할 쓰기 시 약 6.5 ms), 수신기 CPU 사용은 무시할 수준입니다(DATA, 무선 구간 제외).
- 미검증 위험 X: (1) **S25 자이로의 잔류 편향**(0.03 °/s 이하여야 장터널 출구 점프를 네이버 R1 규칙 50 m 아래로 내릴 수 있고, 0.17 °/s를 넘으면 CMU 영점보다 나쁩니다), (2) **무선 AA 동글과 CMU 자체 Bluetooth SPP 링크의 동시 사용**(차량에서만 확인 가능), (3) S25가 CMU Bluetooth와 **페어링돼 있는지**(소유자에게 물어야 함).
- 첫 소유자 행동은 차 없이 되는 **폰 단독 90초 정지 시험**이며, 보고할 숫자는 분석기의 `max_within_window_abs_yaw_deg` 하나입니다(판정 기준 §7).

## 1. 질문 (1) — CMU Bluetooth 스택, 서비스 선언, 지속성, 설치·제거

### 1.1 어떤 스택인가

- **FACT(설정)** `/jci/sm/sm.conf` 326행: `bds`는 `type="process"`, `path="/jci/bds/bds"`, `args="-interface dbus"`, `reset_board="yes"`, `retry_count="30"`, `ready_flag="/tmp/bds_ready_flag"`이며 `stage_3`, `dmserver`, `settings`에 의존합니다. WCP 구성(`sm_WCP.conf` 327행)은 `-interface dbus -wcp`입니다. 13개 이상의 순정 서비스(jciBTHF, jciBTMUSIC, jciBCA, jcibteca, jciMSGS, blm_ira_* 등)가 `bds`에 의존합니다.
- **FACT(파일·문자열)** `/jci/lib/libbluego.so`(2.2 MB)에 BlueGo 코어(`bgspp.c`, `bgdb.c`, `bgmgr.c`)와 Blue SDK 스택(`rfcomm.c`, `sdpserv.c`, `hci.c`) 소스 경로 문자열이 있고, `/jci/bds/BdsConfiguration.xml`은 `transport serPort="/dev/ttymxc3"`(UART, 3 Mbaud), `btRadio radioType="TI1283"`, `xlator name="bluego"`를 지정합니다. 즉 RFCOMM·SDP·HCI는 **커널 BlueZ가 아니라 사용자 공간 BlueGo/Blue SDK가 UART로 직접 처리**합니다.
- **FACT** BlueZ 흔적(`/usr/sbin/bluetoothd`, `libbluetooth.so.3.11.5`, `lib/modules/3.0.35/.../hci_uart.ko`, `bluetooth.ko`, `rfcomm/`)은 있으나 `sm.conf`·`autostart`·`etc/init.d`·`stage_bt.sh`(빈 스크립트) 어디에서도 기동하지 않고, `97-bluetooth.rules`는 커널 bluetooth 서브시스템 장치가 나타날 때만 `bluetoothd --udev`를 실행합니다. `usr/share/ti/bluetooth/bt-readme.txt`는 보드 개발용 메모입니다. **INFERENCE**: BlueGo가 `/dev/ttymxc3`을 점유하므로 커널 HCI 장치는 생기지 않고 BlueZ 경로는 사용되지 않습니다. 따라서 `rfcomm`/`sdptool` 같은 BlueZ 도구로 서비스를 추가하는 길은 없습니다.
- **FACT** 로그: `bds`의 `LOG_Write`는 `/dev/log`(syslog 소켓)로 나갑니다(qemu-arm 실행에서 호스트 저널에 `BDS, <pid>, ..., bds_manager.c, BDS_Manager_Start 1665, Using address: ...` 형식으로 기록됨). 차량에서는 CMU의 syslog 수신자가 받습니다.

### 1.2 serialPort 서비스 선언과 PTY

- **FACT(설정)** `BdsConfiguration.xml` `<serviceConfiguration>`에 serialPort 7개: id 5 Phone Gateway, 8 Wireless iAP(인스턴스 6), 9 JCI Reflash Server, 10 Pandora, 11 Stitcher, 12 AHA Radio, 14 Android VDT. 각 항목은 `uuidServer`/`uuidClient`(하이픈 없는 32자리), `noOfInstances`, `writeDelay="3"`을 갖고 `securityLevel` 속성은 없습니다. 주석에 "serialPort만 다중 인스턴스 지원, 인스턴스는 RFCOMM 포트를 소비하므로 1~3개 권장"이 있습니다.
- **FACT(디컴파일, `libjcibds.so` `bdscfg_ReadServiceConfiguration`)** serialPort 항목 수 전역 카운터가 `0x14`(20) 미만일 때만 `BDS_SerialPortConfiguration_Add`를 호출하고, 넘으면 "Serial port count exceeds the max limit"를 기록합니다. **DATA(행동)** 원본 `bds`를 qemu-arm으로 실행해 21개 항목 설정을 주면 저널에 그 Warn이 나타나고, 8개(순정 7 + 연구용 1)에서는 나타나지 않습니다. 따라서 **한 개 추가는 한계 안**입니다. `BDS_SerialPortConfiguration_Add`는 UUID 필드를 길이 바이트(0x10)와 16바이트로 보관하며 길이가 0 또는 16이 아니면 assert합니다(기존 기록 `BDS_SDP.md`의 바이트 순서 관찰과 일치).
- **FACT(디컴파일, `bds` 시리얼 서비스)** 서비스 생성(`serviceCreateHandler`)에서 128비트 UUID의 ServiceClassIDList, L2CAP(0x0100)+RFCOMM(0x0003) ProtocolDescriptorList로 SDP 레코드와 클라이언트 검색 패턴을 만들고 `COMMON_IO_Pty_New`로 PTY 객체를 준비합니다. 연결(`serviceConnectHandler`)에서 `COMMON_IO_Pty_Open`(실패 시 `system()`으로 보조 명령 후 재시도)을 하고, 입력·출력 stream을 얻고 읽기 스레드를 시작하며, **PTY 이름을 서비스 객체에 20바이트까지 복사해 기록**합니다. `COMMON_IO_Pty_*`는 `/jci/lib/libjcicommon_io.so`의 pipe 추상화 위에 있고 그 라이브러리는 `openpty`를 가져옵니다(`libutil.so.1`). 전달 상태(`statePassThru`)는 수신 이벤트 0x81마다 PTY 출력 stream에 쓰고 `usleep(writeDelay × 1000)`(= 3 ms)을 하며, 이벤트 −2에서 PTY를 닫고 읽기 스레드를 취소합니다. 반대 방향(`ptyReadHandler`)은 PTY에서 0x400바이트 단위로 읽어 서비스 관리자 명령 0x80으로 보냅니다. **결론: 연결마다 새 PTY가 열리므로 수신기는 연결 신호마다 PTY 경로를 다시 받아야 합니다.**
- **FACT(디컴파일 + 공개 문서)** PTY 경로의 소비자 쪽 전달: `/jci/bca/svcjcibca.so`의 `BCA_SendSignal_ConnectionStatusResp`가 "pty terminal path is %s"를 기록하고 21바이트 배열을 신호에 넣습니다. 공개 D-Bus 문서([silverchris com.jci.bca](https://github.com/silverchris/mazda-cmu-documentation/blob/master/dbus/com.jci.bca.xml.md))의 `ConnectionStatusResp(u serviceId, u connStatus, u btDeviceId, u status, (ay) terminalPath)`, `QueryConnectStatus(u serviceId) → i(iiii(ay))`, `GetServiceId(u serviceType)`과 일치합니다. 순정 Stitcher 서비스(`svcjciblm_ira_stitcher.so`)는 "SPP already connected: Using pseudo terminal <%s>"로 같은 경로를 씁니다. 원시 신호 `com.jci.bds SignalConnected_cb(u type, (ay) bds_msg_1_t)`도 있으나(`libjcibdsdbusservice.so` introspection), 시리얼 연결 이벤트의 PTY 이름 오프셋은 이번에 확정하지 못했습니다(`sendConnectedEvent`가 두 블록을 복사하는 것까지 확인).
- **FACT(디컴파일, `BCA_fillServiceInfo`)** BCA는 시작 시 BDS 서비스 표 **전체**를 열거해 자기 표에 넣고, serialPort 중 이름이 알려진 8종(Phone Gateway, Wireless iAP, Pandora, Stitcher, AHA, serial service 1/2, Reflash)에만 분류 코드를 매깁니다. 알려지지 않은 serialPort 항목도 슬롯·ID·이름은 등록됩니다(분류 코드는 비어 있음). **INFERENCE**: ID 15에 대해서도 `ConnectionStatusResp`가 나올 가능성이 높지만 신호 발생 경로까지는 추적하지 않았으므로, 수신기는 신호 구독과 `QueryConnectStatus(15)` 폴링을 모두 구현해야 합니다.
- **FACT(설정)** `/jci/settings/configurations/blm_bca-system.xml`에는 serialService1UUID(`DAE3E193…`, BDS 설정에 없음)와 serialService2UUID(`DC022184…` = Android VDT, id 14)가 있고 의존 서비스 ID 표에서 "serial service 2"는 1(Handsfree)에 의존합니다. 즉 **기존 serialPort를 재사용하면 순정 소비자(IRA/BCA)와 충돌하거나 HFP 의존이 붙으므로 새 ID 15를 쓰는 설계가 맞습니다.**

### 1.3 설정 파일 경로 — 세 경로로 확인한 핵심 사실

- **FACT(문자열)** `bds`와 `libjcibds.so` 모두 `/tmp/BdsConfiguration.xml`과 `/jci/bds/BdsConfiguration.xml` 문자열을 갖고, `bds`에는 옵션 문자열 `-configFilePath`, `-radioOnly`, `-wcp`, "You need to specify a File Path for configuration"이 있습니다.
- **FACT(디컴파일)** `libjcibds.so` `bdscfg_OpenConfiguration`: 26바이트 경로(`/tmp/BdsConfiguration.xml`)를 먼저 `fopen`하고 실패하면 30바이트 경로(`/jci/bds/BdsConfiguration.xml`)로 넘어가며 둘 다 실패하면 assert입니다. `bds` `BDS_Manager_Start`: `cp -vf <원본> /tmp/BdsConfiguration.xml` 문자열을 조립해 `system()`으로 실행합니다. `<원본>`은 `main`이 `-configFilePath` 다음 인수를 저장해 둔 필드(최대 0xF8바이트)가 있으면 그것, 없으면 RfTest/ServiceLess/기본 `/jci/bds/BdsConfiguration.xml`입니다. 그 뒤 `BDS_Configuration_Load`가 위 로더를 호출하고 `bdscfg_CmdLineOverride`를 적용합니다.
- **DATA(행동, qemu-arm 8.2.2)** 원본 `bds`를 `-interface dbus -configFilePath <우리 XML>`로 실행(`-strace`, 기록용 `cp` 대체 스크립트): `execve("/bin/sh", sh -c "cp -vf <우리 XML> /tmp/BdsConfiguration.xml")` → `open("/tmp/BdsConfiguration.xml")` 2회 → 설정에서 DID 정보(productId 5701) 로그 → `/dev/ttymxc3` 열기 실패 → "Failed to start BDS with status: Failed (104)" 종료. 옵션 없이 실행하면 `cp -vf /jci/bds/BdsConfiguration.xml /tmp/...`를 조립하고(호스트에 원본 경로가 없어 복사 실패) `/tmp` 열기 실패 후 `/jci/bds/...`를 엽니다. **잘못된 XML**(닫는 태그 손상)을 주면 "Null pointer - can't close xml info" 뒤 **SIGSEGV**로 죽습니다.
- **결론**: (a) `/tmp/BdsConfiguration.xml`을 미리 두는 방식은 `cp -vf`에 덮어써지므로 쓸 수 없습니다. (b) `-configFilePath`가 지원되는 정식 경로이며, 우리 설치기가 매 부팅 `/tmp/mx5dr-trial-??????/sm.conf`로 생성하는 trial 복사본(`packaging/common.sh` `trial_to`, `edit_service.awk`)의 `bds` 인수에만 `-configFilePath /data_persist/mx5-aa-dr/bt/bdsconfiguration.xml`(공백 없는 소문자 경로)을 더하면, **순정 `sm.conf`와 `/jci/bds`는 그대로**이고 가드가 trial을 고르지 않은 부팅은 순정 인수로 `bds`가 뜹니다. (c) 우리 XML은 순정 XML(SHA-256 `33a2450c…`)에 serialPort 한 줄만 더한 파일이어야 하고, 설치 시 순정 파일 해시를 대조해야 하며, 릴리스 검사에 **이 기록과 같은 qemu-arm 파싱 실행**(Warn/SIGSEGV 없음, 저널의 DID 로그 확인)을 넣어야 합니다. 설정 손상은 `bds` 크래시 → SM `reset_board` 경로이기 때문입니다(§1.5).

### 1.4 설치·제거·지속성

- **FACT(저장소)** `packaging/install.sh`는 `/jci/sm`, `/usr/bin`을 읽기·쓰기로 재마운트하고, `sm.conf`·`sm_WCP.conf` 원본은 보존한 채 `/data_persist/mx5-aa-dr/guard/normal.trial`·`wcp.trial`을 만들며, `/usr/bin/autostart`에 `MX5DR ONE-BOOT` 블록(가드 `select` → `$SMCFG_*`를 trial 경로로 교체, collector 시작, 90 s 후 `confirm`)을 넣습니다. 소유권 토큰은 `/data_persist/mx5-aa-dr/libmx5dr.so` 등 LD_PRELOAD 값이며 `edit_service.awk`는 지정 서비스의 LD_PRELOAD 값만 바꾸는 폐쇄형 편집기입니다. `uninstall.sh`(메뉴 4)는 블록·토큰·arm/persist를 제거하고, `purge.sh`(메뉴 6)는 새 부팅 뒤 패키지 디렉터리를 지웁니다. 메뉴 1 설치, 2 상태 보고, 3 수집 종료·내보내기, 4 제거, 5 재부팅, 6 완전 삭제(`packaging/trial` 524~531행).
- **설계(INFERENCE)**: Bluetooth 선언을 같은 틀로 넣습니다. 새 폐쇄형 awk 편집기가 trial 복사본의 `<service name="bds" ...>` 한 줄에서만 `args="-interface dbus"`(WCP는 `-interface dbus -wcp`)에 `-configFilePath …`를 덧붙이고 다른 어떤 속성도 바꾸지 않습니다. XML 파일은 패키지 디렉터리 아래에 두므로 메뉴 4(trial 재생성 중단) 뒤 첫 재부팅부터 효력이 없고 메뉴 6이 파일을 지웁니다. 순정 파일은 한 바이트도 바뀌지 않으므로 "완전 복원"은 재부팅 자체가 보장합니다. 영구 저장은 `/data_persist`뿐이며 `/tmp`는 부팅마다 비워집니다(`bds` 자신도 그렇게 가정하고 매번 복사합니다).
- **FACT(디컴파일)** 페어링 DB는 BlueGo `bgdb.c`(`BGDB_RecordCreate/Delete/RecordsFlush`, "Another linkkey is already store in the bgdb.devicelist")이며 장치 수 한계 상수는 이번에 찾지 못했습니다(**미확정**). BT 주소는 `/sys/fsl_otp/HW_OCOTP_MAC0` → `/config-mfg/bt_addr.dat` → `/data/bt_addr_secondary.dat` 순으로 읽습니다(저널 로그).

### 1.5 페어링 요건과 실패 모드

- **FACT(공개 문서)** Android 클라이언트가 `createRfcommSocketToServiceRecord(UUID)`로 연결하면 시스템이 SDP로 UUID의 채널을 찾고, **두 장치가 페어링돼 있지 않으면 프레임워크가 페어링 요청 대화상자를 자동으로 띄우며 사용자가 거부하면 연결이 실패**합니다([connect-bluetooth-devices](https://developer.android.com/develop/connectivity/bluetooth/connect-bluetooth-devices)). 이미 페어링된 장치와의 통신에는 `BLUETOOTH_CONNECT` 런타임 권한만 필요하고 `BLUETOOTH_SCAN`·위치 권한은 불필요합니다([bt-permissions](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions)).
- **FACT(펌웨어)** `BdsConfiguration.xml`은 `enableSsp="True"`, `ioCap="DISPLAY_YES_NO"`이고, `bds`의 `BDS_DeviceManager_SetIncomingPair`는 장치 관리자 플래그를 세우며 "Allow incoming Pairing set to %s"를 기록합니다(초기화 전이면 상태 0x78 반환). 기존 기록 `PHONE_IMU_AND_TRANSPORT.md`는 시리얼 생성 요청의 보안값 0x22를 관찰했습니다. **INFERENCE**: 0x22는 Blue SDK 보안 수준 인코딩에서 수신·발신 인증 요구에 해당할 가능성이 높고(공개 URL로 확인하지 못함), SSP 숫자 비교(DISPLAY_YES_NO)는 CMU 화면의 확인이 필요합니다. 따라서 **폰은 CMU와 한 번은 본딩돼 있어야 하고, 본딩이 없으면 주차 중 한 번 CMU 화면에서 페어링을 수락하는 단계가 필요**합니다. 본딩 뒤에는 폰 앱의 연결에 CMU 조작이 필요 없습니다(공개 문서상 자동). 수신 페어링 허용이 CMU의 "기기 추가" 상태에서만 켜지는지는 미확정입니다.
- 실패 모드와 USB 메뉴 복구(FACT는 sm.conf 속성, 영향은 INFERENCE):
  1. **설정 파싱 실패로 bds 크래시** → SM이 `retry_count=30`만큼 재시작 후 `reset_board="yes"`로 보드 재설정 가능성. 완화: 위 1.3(c)의 오프라인 검증 + 기존 persistent 가드의 미확인 부팅 카운터(90 s 확인 전 재부팅이 반복되면 3회 내 순정으로 복귀, `install.sh`·`trial` GO/NO-GO의 `unconfirmed … of 3`). 사용자 복구: 메뉴 4(제거) 또는 재부팅 후 메뉴 2로 `stock this boot` 확인.
  2. **SDP/RFCOMM 자원**: 128비트 레코드 1개, RFCOMM 서버 채널 1개 추가(규격상 서버 채널 30개; 현재 serial 12인스턴스 + HFP/PBAP/MAP/OPP 등). 설정 한계 20항목은 확인됨. 채널 고갈 시 `serviceCreateHandler`의 BlueGo 생성 실패는 로그로만 남고 다른 서비스는 영향 없음(**INFERENCE**).
  3. **페어링 슬롯**: 새 본딩이 오래된 장치를 밀어낼 수 있음(한계값 미확정). 주차 시험 전에 CMU 기기 목록을 메모하도록 안내.
  4. **스택 크래시**: `bds`가 죽으면 HFP/A2DP/PBAP 전부와 `bds` 의존 서비스가 재시작되므로 이 경로의 가장 큰 사용자 체감 위험입니다. 우리 코드는 `bds` 프로세스에 들어가지 않고(PTY 슬레이브와 D-Bus만 사용) 설정 한 줄만 더하므로, 크래시 위험은 설정 파싱과 BlueGo의 추가 서비스 처리뿐입니다. 전자는 오프라인에서, 후자는 주차 시험 1회에서 확인합니다.
  5. **부팅 루프**: 가드 fallback이 trial 자체를 끄므로 `bds` 인수도 순정으로 돌아갑니다. 설정 파일은 `/data_persist`에만 있어 순정 부팅에 영향이 없습니다.

## 2. 질문 (2) — 무선 AA 동글과의 공존

- **FACT(펌웨어, `/jci/aapa/blmjciaapa.so` 문자열·D-Bus introspection)** CMU의 AA 서비스는 AA 프로토콜의 Bluetooth 채널을 구현합니다: `RequestBTPairing: MACAddr=%s, PairingMethod=%d`, `SetVehicleBtMacAddress`, `BTReadyToPair(alreadyPaired)`, `BTPairingResult`, `StartBtConnection`, `NotifyBTConnectionComplete`, 그리고 "Switch the connect mode to MTP: BT Pairing failed."가 있습니다. 즉 **유선 AA에서는 폰이 CMU 자체 Bluetooth와 페어링되는 절차가 AA 세션의 일부**이고, 실패하면 MTP 모드로 바뀝니다(문자열 기반 FACT, 조건은 INFERENCE).
- **COMMUNITY/제조사** 동글(AAWireless, Motorola MA1 등)은 폰과 Bluetooth로 먼저 연결해 Wi-Fi(5 GHz) 자격을 넘기고 이후 Wi-Fi로 AA를 중계하며, 헤드유닛에는 USB AA 액세서리로 보입니다([9to5google 2020-12](https://9to5google.com/2020/12/14/aawireless-android-auto-dongle-hands-on/), [xda 2022-01 MA1](https://www.xda-developers.com/motorola-ma1/)). AAWireless 제품 안내는 "Android Auto also keeps using Bluetooth, for example to make calls"라고만 적습니다([aawireless.io product-info](https://www.aawireless.io/en/support/topics-faq-aawireless/product-info)). 공식 AA 안내는 무선 연결의 첫 단계로 **폰과 차의 Bluetooth 페어링**을 요구합니다([support.google.com/androidauto/answer/6348029](https://support.google.com/androidauto/answer/6348029)).
- **알 수 없는 사실(소유자에게 물어야 함, 가정 금지)**: (a) 동글 모델·펌웨어; (b) CMU 설정의 Bluetooth 기기 목록에 **S25가 있는지, 동글이 있는지, 둘 다인지**; (c) AA 중 전화가 CMU 핸즈프리(차량 마이크/스피커)로 되는지; (d) S25의 Bluetooth 목록에 CMU 이름(`/jci/bds/bt_name.dat`의 이름)이 페어링돼 있는지. 동글이 자기 BT 주소를 AA 채널에 넣어 CMU와 자신이 본딩하는 방식이면 S25는 CMU와 본딩이 없고(→ 한 번 페어링 필요), 폰 주소를 그대로 넘기는 방식이면 이미 본딩돼 있습니다(**INFERENCE**, 모델별 상이).
- 동시 연결의 정상성: Android는 여러 클래식 Bluetooth 장치와 동시에 ACL을 유지할 수 있고(워치+차량 등 일반적 사용) RFCOMM 소켓은 앱 단위로 열리므로, **S25가 동글과 CMU에 동시에 연결되는 것 자체는 지원 범위**입니다(**INFERENCE**, 수치 한계 문서 없음). 위험은 (1) S25가 CMU와 본딩되면 CMU가 HFP/A2DP 자동 재연결을 시도해 동글 쪽 HFP와 경쟁할 수 있음(CMU `BdsConfiguration` `autoTransferEnabled="true"`), (2) 폰의 Bluetooth/Wi-Fi 공존(2.4 GHz와 5 GHz이면 간섭 작음; 동글이 5 GHz 요구), (3) CMU `SniffEnable="true"`로 유휴 링크가 sniff로 들어가 지연이 커질 수 있음(50 Hz 연속 트래픽이면 활성 유지, **INFERENCE**). 이 셋은 **주차 상태의 차량 시험 1회**로만 확인됩니다.

## 3. 질문 (3) — 데이터 경로 설계

- **프레임(32바이트, LE)**: `'M','X'`, ver u8, flags u8, seq u32, t_phone_ns u64(`SensorEvent.timestamp` 기준 = `elapsedRealtimeNanos`), ω_z_body f32(중력축 기준 각속도, rad/s; `TYPE_GYROSCOPE` 값과 `TYPE_GRAVITY` 단위벡터의 내적), |ω| f32, bias_z f32(`TYPE_GYROSCOPE_UNCALIBRATED`의 편향 필드), tilt×100 u16, CRC16-CCITT u16. flags 0x80 = CMU→폰 ping, 0x81 = 응답(수신 시각 포함). 50 Hz = 1.6 kB/s(12.8 kbit/s). 100 Hz도 3.2 kB/s에 불과합니다. 세션 시작 프레임(장치 모델, 센서 이름, 앱 버전)과 상태 프레임(FGS 상태, 거치 이동 플래그, 배터리)을 1 Hz로 더합니다.
- **시각 정렬**: 수신기는 CMU `CLOCK_MONOTONIC`으로 수신 시각을 찍고, 5 s마다 ping을 보내 폰이 수신·송신 시각을 넣어 돌려주면 `offset = t_phone − (t_tx+t_rx)/2`의 중앙값(최근 10개)을 씁니다. 적분의 dt는 **폰 시각 차**로만 계산하고 offset은 휠 창(10 Hz)과의 정렬에만 쓰므로 수십 ms 오차는 1° 미만입니다(20 °/s 회전에서 50 ms = 1.0°, §5). 손실·역전: seq 간격 > 1이면 손실로 기록하고 손실 구간은 선형 보간 없이 **직전 각속도 유지 최대 200 ms**, 그 이상이면 폰 입력 무효 → 기존 CMU yaw와 예산으로 복귀(사유 기록).
- **수신기 위치**: D-Bus는 이미 collector가 사용하므로(`src/collector/collector.cpp`, `unix:path=/tmp/dbus_service_socket`) collector가 `com.jci.bca` `ConnectionStatusResp`(serviceId 15)와 `QueryConnectStatus(15)` 폴링으로 `terminalPath`를 얻어 `/tmp/mx5dr-phone-pty` 같은 파일에 적고, jciVBS 안의 preload worker(DR가 도는 곳)가 그 경로의 PTY 슬레이브를 열어 읽습니다(또는 collector가 읽고 UNIX 데이터그램으로 전달). **순정 Bluetooth 소비자(BCA/IRA)는 건드리지 않고** 슬레이브만 엽니다. PTY는 연결마다 바뀌므로 `connStatus`가 끊김이면 닫고 다시 기다립니다.
- **DATA(벤치, PTY 구간만)** 폰·BDS 대역을 호스트 Python이 PTY 마스터로 대신하고(한 번에 한 프레임 쓰기 + 3 ms 대기, 또는 1/16/15바이트 분할 쓰기 + 각 3 ms), 수신기(C, x86 네이티브 및 핀 고정 ARM 툴체인 빌드를 qemu-arm으로)가 슬레이브를 읽었습니다. 50 Hz × 60 s: 3000/3000 프레임, 손실 0, CRC 오류 0, 적분 yaw 343.66°(기대 0.1 rad/s × 60 s = 343.77°), 지연 평균 0.25 ms(단일 쓰기) / 6.5 ms(분할 쓰기, 3 ms 간격 2회 포함), 단방향 ping 0.12 ms; qemu-arm 수신기도 동일 계수, 에뮬레이션 CPU 0.33 s user + 0.24 s sys/60 s, RSS 12 MB. 100 Hz × 20 s도 손실 0. **무선 RFCOMM·BlueGo·`bds`는 포함되지 않았으므로 실제 Bluetooth 지연은 차량 시험에서 ping으로 재야 합니다.** 결과 JSON: scratch `rx/bench_*.json`.
- **CMU 자원(INFERENCE)**: 50 Hz 32바이트 파싱은 i.MX6에서도 1% 미만이며, 메모리는 수 KB 링 버퍼입니다. jciVBS 안에서 블로킹 read는 금지하고 `select` 기반 worker tick에 합칩니다.
- **BETA 융합**: `mx5_dr_interval`(`src/core/dr_core.h`)의 `yaw_rad_s`를 폰 값으로 채우고 `raw_yaw`는 기록만 합니다. **영점 처리가 CMU와 근본적으로 다릅니다.** CMU 0x078 영점은 관측 불가능한 정수 계단(`YAW_ZERO_STEP_TRIGGER_2026-10-12.md`)이라 마지막 정차 평균이 최악의 예측자였지만, 폰 편향은 연속량이고 Android가 정지 시 온라인 보정하므로 **네 바퀴 0인 정차(≥ 5 s)에서 폰 각속도 평균을 잔류 편향으로 학습해 다음 주행에 빼는 것이 타당**합니다(검증은 다음 beta 로그로). 정직 예산은 `k = 0.002 rad/s` 대신 `b_eff = max(b_정지시험, |b_정차학습|, b_floor)`를 쓰며 `b_floor`는 0.03 °/s(5.2e-4 rad/s)로 제안합니다(아래 모델). 폰 입력과 CMU yaw가 창 10 s에서 1 °/s 이상 어긋나고 폰 tilt/|ω|에 거치 교란이 보이면 폰 입력을 무효화하고 현재 예산으로 돌아갑니다(현행보다 나빠지지 않음).

## 4. 질문 (4) — 폰 쪽 견고성

- **FACT(공개 문서)** Android 12+는 `registerListener` 표본율을 200 Hz로 제한하고 그 이상에서만 `HIGH_SAMPLING_RATE_SENSORS`가 필요하므로 50~100 Hz에는 권한이 필요 없습니다. Android 9+에서 백그라운드 앱은 연속 보고 센서 이벤트를 받지 못하므로 **포그라운드 서비스**가 필요합니다([sensors_overview](https://developer.android.com/develop/sensors-and-location/sensors/sensors_overview)). Android 14+는 FGS 종류를 요구하며 외부 Bluetooth 장치 연결에는 `connectedDevice`(`FOREGROUND_SERVICE_CONNECTED_DEVICE` + `BLUETOOTH_CONNECT` 런타임 권한)가 맞습니다([fgs/service-types](https://developer.android.com/develop/background-work/services/fgs/service-types)). **INFERENCE**: 센서 수신 자체는 FGS 종류에 무관하므로 `health` 종류는 쓰지 않습니다.
- **FACT(Samsung 문서)** 약 3일 미사용 앱은 "sleeping"이 되어 Job·Alarm·**Foreground-service가 제한**되고, 16일이면 deep sleeping, 예외는 "Never sleeping apps" 목록(딥링크 API 제공)이며 One UI 6.0부터 Android 14 대상 앱의 FGS는 새 FGS 정책을 따르면 보장됩니다([developer.samsung.com/mobile/app-management](https://developer.samsung.com/mobile/app-management.html)). 따라서 앱은 target 34+, `connectedDevice` FGS, 그리고 설치 시 소유자가 Never sleeping apps에 추가하는 절차를 둡니다. 화면 꺼짐·주머니 상태는 FGS로 해결되며 `WakeLock`은 Samsung이 FGS에서도 제한한 전례가 있어(COMMUNITY, dontkillmyapp) 의존하지 않습니다.
- **FACT(AOSP)** `TYPE_GYROSCOPE`는 온도·배율·**온라인 편향 보정**이 적용되고 정지 시 0에 가까워야 하며, `TYPE_GYROSCOPE_UNCALIBRATED`의 편향 필드는 추정이 바뀔 때 점프하고 그 외에는 안정해야 하고(`uncalibrated = calibrated + bias`), `GAME_ROTATION_VECTOR`의 기준 방위는 자이로 Z 드리프트만큼 흐를 수 있으며 정확도 추정을 주지 않습니다([sensor-types](https://source.android.com/docs/core/interaction/sensors/sensor-types)). Samsung 단말의 보정 알고리즘·발동 조건은 공개 문서가 없습니다(**미확정**). CDD의 자이로 수치(일반 0.01 rad/s 강력 권고, 고품질 0.002 rad/s 권고)는 기존 기록 `PHONE_IMU_AND_TRANSPORT.md`의 인용을 따릅니다(이번에 페이지 분량 때문에 재인출 실패). S25의 자이로 부품도 공개 분해 기사에서 확정하지 못했습니다.
- **거치 이동·터치**: 중력축 기준 각속도는 요 방향 거치 각도에 무관하고(`PHONE_RELATIVE_YAW.md`의 기하), 기울기 변화는 tilt 필드로 보이므로 (a) tilt가 1 s 안에 3° 이상 바뀌거나 |ω|가 차량이 낼 수 없는 값(> 60 °/s)이면 "거치 교란"으로 2 s 무효 + 새 앵커까지 보수 예산, (b) 교란 뒤 tilt가 이전과 2° 이상 다르면 거치가 바뀐 것으로 보고 다음 GPS 앵커까지 폰 입력 무효. 고정 거치는 소유자가 확인했으나 수치 기준은 이번 제안이며 실측으로 조정합니다.

## 5. S25 편향 요구치 — 모델과 숫자(DATA)

모델: 상수 잔류 편향 b로 heading 오차 θ(t)=b·t, 속도 v 직진 T초 → 횡오차 c = v·b·T²/2(소각 근사). 입력은 `EXIT_JUMP_BLEND_2026-10-13.md`의 사건 표와 `YAW_ZERO_STEP_TRIGGER_2026-10-12.md`의 계단 상수(1.5275카운트/단, 26.5카운트 ≈ 1 °/s). 계산 스크립트는 scratch `bias_model.py`.

- CMU 영점: 1단 0.0576 °/s(1.0e-3 rad/s), 3단 범위 0.173 °/s, 현행 정직 예산 k = 0.002 rad/s = 0.115 °/s.
- 실제 두 장터널의 기록 heading 오차에서 역산한 등가 편향: 758.8(111 s, 8.1 m/s) 0.199 °/s(횡오차 기준 0.124), 908.2(60 s, 12.5 m/s) 0.156(0.132) — CMU 계단 모델과 같은 자릿수입니다.

| 편향 b [°/s] | 758.8 (111 s, 8.1 m/s) | 908.2 (60 s, 12.5 m/s) | 일반 120 s, 15 m/s |
|---|---|---|---|
| 0.173 (CMU 3단) | 150 m (19.2°) | 68 m (10.4°) | 326 m (20.8°) |
| 0.115 (k) | 100 m (12.7°) | 45 m (6.9°) | 216 m (13.8°) |
| 0.058 (CMU 1단) | 50 m (6.4°) | 23 m (3.5°) | 109 m (6.9°) |
| 0.030 | 26 m (3.3°) | 12 m (1.8°) | 57 m (3.6°) |
| 0.010 | 9 m (1.1°) | 4 m (0.6°) | 19 m (1.2°) |

- 네이버 R1(≥ 50 m 점프) 아래로 들어가는 한계 편향: 758.8 → 0.058 °/s, 908.2 → 0.126 °/s, 일반 120 s/15 m/s → **0.027 °/s**.
- 그 밖의 항: 배율 1 %는 90° 회전당 0.9°(누적 아님), 시각 50 ms 오차는 20 °/s 회전에서 1.0°, 거치 기울기 기하 상한은 pitch ±10°/roll ±5°에서 1.97°, ±20°/±10°에서 7.98°(`PHONE_RELATIVE_YAW.md`).
- **결론**: S25 잔류 편향이 **0.03 °/s(5.2e-4 rad/s) 이하**면 120 s 터널에서도 R1을 피하고 heading 오차 4° 이내, **0.058 °/s 이하**면 CMU 1단보다 좋고, **0.17 °/s를 넘으면 CMU 최악보다 나쁘므로 이 경로는 포기**합니다. 60 s 정지 시험의 상대 yaw로 환산하면 각각 1.8°, 3.5°, 10.4°입니다. 단 정지 시험은 Android의 온라인 보정이 켜진 조건의 상한이며, 주행 중(보정이 정지 조건을 못 만나는 동안)의 온도 드리프트는 별도로 다음 beta 로그의 정차 간 편향 변화로 측정해야 합니다.

## 6. 질문 (5) — 최소 검증 단계

오프라인(차 없음):

1. **설정 파싱 검증(완료, 이 기록)**: 원본 `bds` + 우리 XML을 qemu-arm으로 실행해 Warn/SIGSEGV 없음과 DID 로그 확인. 릴리스 검사에 넣습니다.
2. **수신기 벤치(완료, 이 기록)**: PTY 경로 50/100 Hz 무손실, 지연 ≤ 7 ms. 다음 단계에서 D-Bus `ConnectionStatusResp`/`QueryConnectStatus` 처리, 재연결, 손실·역전·세션 경계를 host 검사와 exact-ARM 검사로 추가합니다. `bds`는 라디오 없이 베이스밴드 구성에서 종료하므로 QEMU CMU 이미지에서도 SDP 등록·RFCOMM 연결은 재현할 수 없습니다(**오프라인 한계**).
3. **설치기 편집기**: trial `sm.conf`의 `bds` 인수 편집기와 그 음성 검사(다른 서비스·속성 불변, WCP 변형, 중복 적용 거부), 메뉴 4/6 제거 검사(fixture).
4. **폰 앱**: 기존 `tools/phone-imu-study`에 FGS(`connectedDevice`), `BLUETOOTH_CONNECT`, UUID `a9519c23-f030-4ee8-821f-701493890550`(연구용, 제품 예약 아님) RFCOMM 클라이언트, 프레임·ping을 더합니다. 벤치: BlueZ가 있는 PC를 같은 UUID의 SPP 서버로 두고 30분 화면 꺼짐 상태에서 손실·RTT·FGS 생존을 측정(소유자 PC 또는 개발 PC에 Bluetooth 어댑터 필요; 없으면 생략하고 차량 주차 시험으로 대체).
5. **재생**: 기존 비공개 주행 기록에 "참 yaw(GPS 진행 방위) + 측정 편향 b"의 합성 폰 입력을 넣어 BETA 재생으로 출구 점프 감소를 정량화합니다(위 모델의 독립 확인).

차량(주차, 주행 없음, 평소 USB 절차, 1회):

- 메뉴 1 설치 → 5 재부팅 → 2 상태(새로 추가되는 행: `bt_service=registered|absent`, `phone_spp=connected|none`, `phone_frames`, `phone_ping_ms`) → 폰 앱 시작 → AA를 동글로 평소처럼 연결(시동 ON, 정차) → 다시 USB로 바꿔 메뉴 2·3. 필요하면 CMU 화면의 페어링 수락 1회.
- **차량에서만 답할 수 있는 단일 질문**: "동글로 무선 AA가 진행되는 동안 S25가 CMU 자체 Bluetooth의 새 UUID로 RFCOMM 연결을 유지하고(필요 시 1회 페어링 뒤), AA 세션과 CMU 핸즈프리가 그대로 동작하는가." 주차 상태로 충분하며 주행은 필요 없습니다. 터널 효과는 그 뒤 정상 beta 주행 로그에서 봅니다.

## 7. 판정, 순서, 공수, 첫 소유자 행동

- **판정: 실행 가능(미검증 위험 X = S25 잔류 편향, 동글 공존, 페어링 상태).** 펌웨어 차단 요인 없음. 대안 전달부는 없습니다: AA 프로토콜은 폰→HU 센서 채널이 없고(`AA_PIPELINE_2026-09-30.md`의 방향), USB는 동글이 점유하며, CMU에는 폰과의 Wi-Fi 경로가 없습니다. 기존 serialPort 재사용은 순정 소비자·HFP 의존 때문에 배제합니다.
- 순서: ① 소유자 90초 정지 시험(아래) → 0.17 °/s 초과면 중단. ② 소유자 질문 4개(§2) 답변. ③ 폰 앱 + CMU 수신기 + 설치기 편집기 + 검사(약 6~8 작업일). ④ 재생 정량화(1~2일). ⑤ 주차 시험 1회(소유자 30분). ⑥ 정차 학습 편향·예산 교체를 BETA 다음 빌드에 기록 전용으로 넣고 다음 주행 로그로 확인. 총 약 2주의 에이전트 작업과 소유자 시간 1시간 내외입니다.
- **첫 소유자 행동(차 불필요)**: `tools/phone-imu-study/build.sh <android-sdk>`로 만든 `build/phone-imu-study.apk`(또는 기존 빌드 SHA-256 `103ad0a4…`)를 S25에 설치하고, 앱에서 **정지 시험** 선택 → 폰을 책상 위에 놓고 손대지 않은 채 **90초 기록 시작** → 완료 후 **최근 기록 저장** → `python3 tools/phone-imu-study/analyze.py <파일>`. 보고할 숫자는 `stationary_60s.max_within_window_abs_yaw_deg`(와 `issues`가 비어 있는지, `reported_bias_changes`의 횟수). 판정: **1.8° 이하 양호(0.03 °/s)**, 3.5° 이하 CMU 1단보다 우수, 10.4° 초과면 이 경로 중단. 가능하면 차 안 거치대에서(시동 OFF, 정차) 한 번 더 같은 시험을 하면 거치 진동 조건이 추가됩니다.

## 8. 확정하지 못한 것

- `com.jci.bds SignalConnected_cb` 원시 payload의 시리얼 PTY 이름 오프셋(BCA 신호로 대체 가능).
- BCA가 분류되지 않은 serialPort ID에도 `ConnectionStatusResp`를 내는지(슬롯은 만들어짐).
- Blue SDK 보안값 0x22의 정확한 의미, BlueGo 페어링 DB의 장치 수 한계, TI1283 무선 구간의 실제 지연·sniff 영향.
- 소유자 동글 모델, S25–CMU 본딩 여부, AA 중 통화 경로.
- S25 자이로 부품·잔류 편향·주행 중 온도 드리프트(정지 시험과 beta 로그로).
- 기존 기록의 "security 0x22"와 "독립 서비스 등록·연결 정책"은 여전히 차량에서만 확인됩니다.

## 부록 — 방법과 입력

- Ghidra 12.1.4 headless, `-noanalysis`가 아닌 분석 포함 import(바이너리가 작아 1.5 GB 힙으로 충분), Java GhidraScript로 이름·호출자·주소 범위·문자열 xref의 함수만 디컴파일, 4회 실행 모두 `ghidra.lock` 획득/해제, 가용 메모리 2.2 GB 이상 확인, jadx 미실행 확인, `nice -n 19`. 디컴파일 텍스트는 scratch에만 있습니다.
- 분석 입력 SHA-256(앞 16자리): `jci/bds/bds` 3afd74c7385023ad, `libjcibds.so` f57aaac2b9933eee, `libjcibds-interface-common.so` d4c8de0962470674, `libjcicommon_io.so` fb5c32a0057864e0, `libjcibdsdbusservice.so` 71b8956f7b61269a, `svcjcibca.so` 39e238bb578617e5, `svcjciblm_ira_stitcher.so` 74343f782c8d8b31, `BdsConfiguration.xml` 33a2450c5c932f2a, `blm_bca-system.xml` 842c6bf90af4005d, `sm.conf` fa3484d7c644393a.
- qemu-arm 8.2.2(static) `-L <펌웨어 트리> -E LD_LIBRARY_PATH=/jci/lib:/usr/lib:/lib -strace`, 25 s 타임아웃, `cp` 대체 스크립트는 argv를 기록하고 대상이 이미 있으면 복사하지 않으며 실행 직후 `/tmp/BdsConfiguration.xml`을 삭제했습니다(동시 분석자 보호). 작성 설정 SHA-256: 8항목 dcf553d298d91fb1, debug 1dd92308d96a3931, 21항목 8bd3e33792c0e4b4, 손상 6588d3daf9045569. 실행 중 `bds`가 호스트 `/tmp/inbox`, `/tmp/audio` 디렉터리를 만들었고 호스트 저널에 로그가 남았습니다(OEM 코드 아님).
- 벤치: `rx/receiver.c`(662be06244104daf), `rx/phone_sim.py`(70e0b77bfa6b40b7), ARM 빌드는 핀 고정 m3-toolchain gcc 4.9.1 `-std=gnu99 -lrt`.
- 제품 코드 변경이 없으므로 `make test`·exact-ARM·QEMU 제품 기동은 이번 기록에서 실행하지 않았습니다. 기존 검증 수치는 수정하지 않았습니다.
