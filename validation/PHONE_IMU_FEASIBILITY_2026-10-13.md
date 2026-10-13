# 휴대폰 IMU → Bluetooth → CMU yaw 공급 경로의 실현 가능성 재검토 — 2026-10-13

대상: 1세대 Mazda Connect NA 74.00.324A, 소유자의 Galaxy S25(고정 거치)와 무선 AA 동글.
이 기록은 오프라인 분석 결과입니다. 차량·폰 접속, 설치, 주행은 하지 않았습니다.
제품 코드와 설정은 바꾸지 않았습니다. 폰 송신 앱, CMU 수신기와 DR 연결은 **미구현**입니다.

표기: **FACT**(펌웨어 파일·함수·문자열·설정), **DATA**(보존 기록에서 계산; 방법 명시),
**PUBLIC DOC**(URL; 이번 실행에서 다시 조회하지 않은 공개 문서는 그렇게 적었습니다),
**INFERENCE**(추론). OEM 코드·역어셈블·디컴파일 출력은 싣지 않고 함수명·주소·상수만 적습니다.

## 결론

**판정: 실현 가능하나 미검증 위험이 있습니다.** 가장 큰 미검증 위험은 다음 세 가지입니다.

1. 추가한 시리얼 서비스가 실제 BlueGo 스택에서 등록되고, 폰의 들어오는 RFCOMM 연결이
   사용자 확인 없이 승인되는지입니다(BDS 내부 승인 경로 미해석).
2. 무선 AA 세션 중 같은 폰이 CMU와 HFP를 유지하면서 추가 RFCOMM 채널을 여는 것이 AA·HFP에
   영향을 주지 않는지입니다.
3. S25 잔류 편향이 아래 기준을 만족하는지입니다.

막는 요인(blocked)은 찾지 못했습니다. 설치 방식은 기존 guard의 trial `sm.conf` 선택과
같은 방법으로 순정 `BdsConfiguration.xml`을 건드리지 않고 넣고 뺄 수 있는 근거가 있습니다
(아래 1.4). 오프라인으로 닫을 수 없는 차량 측 질문은 **하나**이며, 주차 상태 시험으로
충분합니다(아래 6).

## 1. CMU Bluetooth 스택

### 1.1 담당 프로세스

- FACT: `/jci/bds/bds -interface dbus`가 Bluetooth 데몬입니다. `sm.conf`의 `bds` 항목은
  `reset_board="yes" retry_count="30"`이며 `stage_3`·`dmserver`·`settings`에 의존합니다.
  설정의 `xlator name="bluego"`로 공급사 스택 BlueGo(`/jci/lib/libbluego.so`,
  `libjcibds-xlator-bluego.so`)를 씁니다. UART `/dev/ttymxc3`, TI1283 무선칩입니다.
- FACT: 펌웨어에 BlueZ `bluetoothd`도 있으나, DATA(2026-10-11 내보내기의 `ps_info`)에서
  실행 중인 것은 `bds`와 `jciBTHF` 등이고 `bluetoothd`는 없습니다.
- FACT: 연결 정책은 BCA(`/jci/bca/svcjcibca.so`)가 맡습니다. BCA의 D-Bus
  인터페이스 `com.jci.bca`(라이브러리에 내장된 introspection XML)에는
  `ConnectRequest(serviceId, btDeviceId)`, `QueryConnectStatus(serviceId)` →
  `(serviceId, connStatus, btDeviceId, status, terminalPath)`와 같은 내용의
  `ConnectionStatusResp` 신호가 있습니다. **`terminalPath`가 시리얼 서비스의 PTY 경로를
  소비자에게 넘기는 공식 경로**입니다.

### 1.2 시리얼 서비스 선언과 데이터 경로

- FACT(설정): `BdsConfiguration.xml`의 `serialPort` 항목은 `id`, `name`,
  `noOfInstances`, `uuidServer`, `uuidClient`, `writeDelay`를 가집니다. 순정 항목은
  ID 5 Phone Gateway, 8 Wireless iAP(6개), 9 Reflash, 10 Pandora, 11 Stitcher,
  12 AHA, 14 Android VDT입니다. ID 13·15는 비어 있습니다. 설정 주석은 RFCOMM 포트를
  다른 서비스와 공유하므로 인스턴스를 늘릴 때 주의하라고 적습니다.
- FACT(디컴파일, `libjcibds.so` `bdscfg_ReadServiceConfiguration`): 시리얼 항목은
  개수가 20 미만일 때만 추가합니다. 문자열 "Serial port count exceeds the max limit"과
  일치합니다. 순정 시리얼 서비스는 7개(인스턴스 12개)이므로 1개 추가는 한도 안입니다.
- FACT(디컴파일, `bds` `statePassThru` 0x4c464): 데이터 이벤트(0x81)를 받으면 PTY 출력
  stream에 쓰고, 그 뒤 `usleep(writeDelay×1000)`을 호출합니다. 쓰기 실패는 상태 104입니다.
  2026-10-10 Unicorn 실행 결과(상태 104, 분할 보존)와 일치합니다.
- FACT(디컴파일, `bds` `ptyReadHandler` 0x4cac4): 별도 스레드가 PTY에서 최대 1,024바이트를
  읽어 Bluetooth로 보냅니다. 따라서 **CMU→폰 방향도 같은 PTY로 쓸 수 있어** 왕복 시각
  동기화가 가능합니다.
- FACT(디컴파일, `libjcicommon_io.so`): PTY는 `COMMON_IO_Pty_New/Open/GetName`이
  `openpty` 기반 pipe 객체로 만들고, `COMMON_IO_OutputStream_Write`는 전체 길이를 다 쓸
  때까지 반복합니다. 같은 경로를 1.5의 실행으로 확인했습니다.
- FACT(디컴파일, `svcjcibca.so` `BCA_updateConnectionStatus` 0x7cb04): BDS의 연결
  상태를 서비스 표에 반영합니다. 표 크기 경계 검사가 있으며, 연결 정보의 20바이트
  terminal path를 복사합니다. 경계 밖 색인은 오류 로그만 남깁니다.
- FACT(`BCA_fillServiceInfo` 0x7822c): BCA는 시리얼 서비스 8종을 **이름**으로 찾아 내부
  종류를 매깁니다. 이름이 맞지 않는 새 시리얼 서비스는 종류가 설정되지 않습니다.
  INFERENCE: 그 경우 BCA의 `ConnectRequest`로 CMU가 먼저 연결하는 경로는 거부될 수
  있습니다. 따라서 **폰이 CMU의 서버 UUID로 먼저 연결하는 방향**을 권합니다. 이때 BCA는
  상태를 기록만 하고 장치 정보 갱신은 하지 않습니다(종류 0 분기). 해당 분기에서 연결을
  끊는 호출은 보지 못했으나 모든 상태 핸들러를 확인한 것은 아닙니다.

### 1.3 페어링과 승인

- DATA(17개 보존 내보내기의 OEM 로그 121개를 풀어 `grep`): BDS가 연결한 상대 장치는
  **하나뿐**이며(같은 OUI, 72회), BCA는 "Total Paired Devices: 1"을 반복 기록합니다.
  즉 폰은 이미 CMU와 본딩되어 있습니다. MAC 주소는 싣지 않습니다.
- FACT: 순정 BDS D-Bus 신호 목록에는 들어오는 서비스 연결을 앱에 묻는 신호가 없습니다
  (`BDS_SIGOUT_DEVICE_AUTHENTICATE`는 페어링용). `BDS_DeviceManager_Authorize`는
  상태기계 이벤트 3을 보내고, xlator는 `BLUEGO_Authorize(allow, store)`를 호출합니다.
  INFERENCE: 승인은 BDS 내부에서 결정됩니다. 본딩된 장치의 연결을 자동 허용하는지는
  **미해석**입니다(상태기계 처리 함수를 찾지 못함). 2026-10-10 생성 요청의 보안 값
  0x22의 의미도 공개 BlueGo 문서로 확인하지 못했습니다.
- FACT/DATA: BCA의 connect filter(`bca_SetConnectFilterForHfpA2dp`)는 부팅·링크 손실
  때 HFP/A2DP 장치만 허용하도록 설정되고, 로그에서는 부팅 약 34–35초 뒤
  `bca_ClearConnectFilter`로 해제됩니다. 주행 중 연결은 이 필터 밖입니다(관측 범위 안).
- PUBLIC DOC(재조회 안 함): Android `createRfcommSocketToServiceRecord(UUID)`는 SDP로
  채널을 찾는 보안 소켓입니다.
  https://developer.android.com/reference/android/bluetooth/BluetoothDevice#createRfcommSocketToServiceRecord(java.util.UUID)
  INFERENCE: 이미 본딩된 상대에는 새 페어링 UI가 필요 없을 것입니다. 차량 시험 항목입니다.

### 1.4 설치와 완전 제거

- FACT: `bds`는 `-configFilePath <경로>` 인수를 받습니다. 순정 `smtestmode.conf`와
  `smbtrftestmode.conf`가 그 인수로 다른 XML을 지정합니다. `libjcibds.so`에는
  `/tmp/BdsConfiguration.xml`("linked configuration") → `/jci/bds/BdsConfiguration.xml`
  순서의 문자열도 있습니다. 실제 열기 순서와 인수 우선순위는 `bdscfg_OpenConfiguration`을
  해석하지 않아 **미검증**입니다.
- FACT(저장소 `packaging/install.sh`): 현재 설치기는 `/jci/sm/sm.conf`·`sm_WCP.conf`를
  복사해 trial 템플릿을 만들고, `/usr/bin/autostart`를 바꿔 guard가 부팅마다 템플릿 사용
  여부를 판정하게 합니다. 백업은 `backups/<시각>`에 있고 해시로 동시 편집을 거부합니다.
  지금 템플릿 편집(`edit_service.awk`)은 지정 서비스의 `LD_PRELOAD` 한 값만 바꿉니다.
- 설계(INFERENCE): 연구용 XML을 우리 디렉터리에 두고, trial 템플릿의 `bds` 항목 `args`에만
  `-configFilePath`를 더합니다. 순정 XML과 순정 `sm.conf`는 그대로 두므로, 템플릿을
  선택하지 않거나 메뉴 4로 제거하면 다음 부팅에 순정 상태가 됩니다. 이를 위해
  `edit_service.awk`를 `args` 한 값으로 넓히는 별도 구현과 시험이 필요합니다.
- 실패와 복구:

| 실패 | 근거 | 복구 |
|---|---|---|
| 새 XML로 `bds` 기동 실패나 반복 crash | FACT `retry_count=30`, `reset_board=yes` | guard 기록(PERSISTENT_GUARD_DESIGN_2026-10-06)의 reset/boot-loop 판정이 2–3부팅 안에 순정으로 돌림; 메뉴 4 |
| `bds`는 살아 있고 HFP만 실패 | guard가 감지 못함 | collector 건강 기준 추가 필요(HFP 연결 관측 실패 시 `disable-next-start`); 미구현 |
| 수신기가 PTY를 열고 읽지 않음 | 1.5의 실행: 쓰기가 막힘 | 수신기 전용 읽기 스레드와 정지 감시; 막힌 `bds` 스레드가 HFP까지 멈추는지는 INFERENCE |
| 페어링 슬롯 | 새 페어링 불필요(본딩 1개) | 해당 없음 |

## 2. 무선 AA 동글과 공존

- DATA(OEM 로그): 동글로 AA가 시작되면 BCA가 `BCA_AAPA_START_BT_CONNECTION` →
  "start AndroidAuto pairing" → "Connecting Handsfree profile for Device 1"을 실행합니다.
  AA 중 BCA 표는 Handsfree CONNECTED이고 상태는 `bcaIdleConnectibleState`입니다. AA가
  끝나면 BCA가 음악·메시지 서비스를 요청합니다(2026-10-11 01:05:12 등 여러 번).
  **소유자의 폰은 AA 중에도 CMU 자체 Bluetooth와 HFP로 연결되어 있습니다.** 이 연결은
  BCA가 AA 신호를 받아 만드는 정상 동작입니다.
- INFERENCE: 동글은 USB AA 장치로 보이고, 폰은 동글과 별도 Bluetooth(시작 협상)와
  5 GHz Wi-Fi로 통신합니다. 동글 모델과 폰–동글 Bluetooth가 계속 유지되는지는
  **소유자에게 물어야 하는 미확인 사항**입니다. 이미 폰이 동글(BT·Wi-Fi)과 CMU(HFP)에
  동시에 연결되어 있으므로, 우리 경로는 기존 CMU ACL 위에 RFCOMM 채널 하나를 더합니다.
  새 장치 연결이 아닙니다.
- 대역폭(INFERENCE): 50 Hz × 44바이트 ≈ 2.2 kB/s(약 18 kbit/s)로 BR/EDR 용량의
  1% 미만입니다. 다만 폰 내부 BT/Wi-Fi 공존 스케줄링 영향은 측정해야 합니다.
  DATA(TRIP_BETA3_2026-10-08): 이미 동글이 약 25초 주기로 재열거되는 사례가 있었으므로,
  추가 채널 시험에서는 동글 끊김 횟수를 기존 기준선과 비교해야 합니다.

## 3. 데이터 경로 설계와 오프라인 실행

### 3.1 프레임(설계, 미구현)

리틀엔디언, 동기 바이트 2, 버전, 종류, 길이 u16, payload, CRC-16/CCITT.
IMU payload 36바이트: 순번 u32, `SensorEvent.timestamp`(elapsedRealtimeNanos) u64,
보정 gyro xyz i32 µrad/s, game rotation quaternion i16×4, 상태 u16, uncalibrated 편향
변화 카운터 u16. 전체 44바이트입니다. 시각 동기: CMU가 `t1`을 보내면 폰이 `t1, t2, t3`를
돌려주고 CMU가 `t4`를 기록합니다. 최근 창에서 RTT 하위 25% 표본으로 offset과 skew를
맞춥니다. RFCOMM은 순서·신뢰 전송이므로 재정렬 대신 순번 역행·공백을 새 세션으로 처리합니다.

### 3.2 원본 PTY 경로 실행(QEMU)

ARM 하네스(pinned GCC 4.9.1)가 **원본 `libjcicommon_io.so`의 PTY·stream 함수를 호출**합니다.
`statePassThru`처럼 각 이벤트마다 Write 후 `usleep(3 ms)`를 하고, 다른 스레드는 1,024바이트씩
읽습니다. 실행 환경은 qemu-arm 8.2.2, 펌웨어 rootfs, 원본 `libutil.so.1` preload입니다.
호스트에서 PTY slave를 raw로 읽었습니다. 펌웨어 함수 코드는 복사하지 않았습니다.

| 이벤트 수 × 크기 | 전달 | 이벤트당 시간 |
|---|---|---|
| 500 × 44 B, delay 3 | 22,000/22,000 B, SHA-256 일치 | 3.125 ms |
| 500 × 220 B, delay 3 | 110,000/110,000 B, 일치 | 3.140 ms |
| 200 × 500 B, delay 3 | 100,000/100,000 B, 일치 | 3.135 ms |
| 500 × 44 B, delay 0 | 일치 | 0.070 ms |

DATA: `writeDelay=3`일 때 상한은 약 **319 이벤트/s**(500 B MTU면 약 160 kB/s)입니다.
50 Hz를 프레임마다 보내면 그 16%, 5개씩 묶어 10 Hz로 보내면 3%입니다. 첫 실행은
`openpty` 미해결로 실패했으며 `libutil.so.1` preload 후 통과했습니다.

소비자 부재 실험: PTY slave를 닫아 두면 4,000건이 모두 막힘 없이 처리되었습니다.
slave를 연 채 읽지 않으면 8초 안에 1,958건에서 Write가 **막혔습니다**. 호스트 Linux PTY의
동작이며 대상 커널의 버퍼 크기는 다를 수 있습니다. 수신기는 반드시 항상 읽어야 합니다.

**한계:** BlueGo·무선·실제 `bds` 프로세스·BCA·D-Bus를 실행한 시험이 아닙니다.
QEMU 시간은 Cortex-A9 성능이 아닙니다. 계획했던 ARM 수신기 시제품과 폰 시뮬레이터의
지연·시각 동기 오차 측정은 **이번 실행에서 완료하지 못했습니다(미구현)**.

### 3.3 수신기 위치와 자원(INFERENCE)

FACT(저장소 `src/collector/collector.cpp`): collector는 이미
`unix:path=/tmp/dbus_service_socket`에 private D-Bus로 붙습니다. 수신기는 collector의
별도 스레드에 두는 것을 권합니다. 연결 시 BCA `ConnectionStatusResp`(serviceId 15)의
`terminalPath`를 받거나, 늦게 시작하면 `QueryConnectStatus(15)`를 한 번 호출합니다.
이 신호에 `always_broadcast=false` 표시가 있어 수동 수신이 되는지는 미검증입니다.
stock 소비자의 상태는 바꾸지 않습니다. 50 Hz × 44 B 파싱과 CRC는 초당 수만 명령 수준으로
CPU 부담이 작습니다(측정 아님). shadow.4에서 stock 프로그램 호출이 리셋 용의자였으므로
D-Bus 메서드 호출은 최소화합니다.

### 3.4 BETA 융합

yaw 입력만 폰 값으로 바꾸고 속도는 기존 바퀴 속도를 씁니다. 정직 예산의 `k`는 폰 편향
추정치로 바꿉니다. 폰 프레임이 끊기거나 동기 품질이 나쁘면 기존 0x078 경로로 돌아가고
이유를 기록합니다. 마지막 값을 반복하지 않습니다.

## 4. S25 편향 요구(모델과 수치)

모델: 상수 잔류 편향 b. 방위 오차 = b·t, 횡오차 ≈ ½·v·b·t²입니다. 백색잡음·배율·장착
오차는 제외했으므로 아래 값은 **필요조건**입니다.

| b (°/s) | 60 s / 120 s 방위 오차 | 120 s 횡오차 at 60 / 100 km/h |
|---|---|---|
| 0.17(CMU 영점 범위) | 10.2° / 20.4° | 357 m / 594 m |
| 0.1146(현재 k=0.002 rad/s) | 6.9° / 13.8° | 241 m / 400 m |
| 0.05 | 3.0° / 6.0° | 105 m / 175 m |
| 0.02 | 1.2° / 2.4° | 42 m / 70 m |
| 0.01 | 0.6° / 1.2° | 21 m / 35 m |

- 현재 정직 예산 k보다 나으려면 b < 0.115°/s, 120초에 5° 이하면 b ≤ 0.042°/s입니다.
- 120초 횡오차 50 m 이하면 60 km/h에서 b ≤ 0.024°/s, 100 km/h에서 b ≤ 0.014°/s입니다.
- PUBLIC DOC(재조회 안 함): Android CDD 7.3.4의 정지 편향 권고는 0.01 rad/s(0.57°/s,
  일반)와 0.002 rad/s(0.115°/s, 고품질)입니다. **CDD 충족만으로는 위 기준을 보장하지
  못합니다.** https://source.android.com/docs/compatibility/16/android-16-cdd

## 5. 폰 측 견고성(공개 문서, 재조회 안 함)

- Android 12 이상에서 200 Hz를 넘는 센서 주기는 `HIGH_SAMPLING_RATE_SENSORS`가 필요합니다.
  50–100 Hz면 필요 없습니다.
  https://developer.android.com/develop/sensors-and-location/sensors/sensors_overview
- 백그라운드 앱은 연속 센서 이벤트를 받지 못하므로 foreground service가 필요합니다.
  Android 14부터 서비스 종류 선언이 필요하며 `connectedDevice`가 맞습니다.
  `BLUETOOTH_CONNECT` 런타임 권한도 필요합니다.
  https://developer.android.com/develop/background-work/services/fgs/service-types ,
  https://developer.android.com/develop/connectivity/bluetooth/bt-permissions
- COMMUNITY: Samsung의 배터리 최적화가 백그라운드 서비스를 종료한다는 사례가 많으므로
  "제한 없음" 설정이 필요합니다. https://dontkillmyapp.com/samsung
- 폰을 만지거나 거치가 움직이면 상대 회전 상쇄가 깨집니다(PHONE_RELATIVE_YAW.md).
  가속도·자이로 급변이나 화면 터치 시각으로 세션을 나누고, 그 구간은 폰 yaw를 쓰지
  않습니다. Samsung의 `TYPE_GYROSCOPE` 온라인 편향 보정 조건은 공개 문서에서 찾지
  못했습니다(미확인). 정지 중 보정이 주행 중 진동·온도 변화에서 유지되는지는 측정해야 합니다.

## 6. 최소 검증 단계

오프라인(차량 불필요):
1. 원본 설정 로더로 ID 15 추가 XML 읽기(2026-10-10 실행 완료) +
   `-configFilePath`/`/tmp` 경로 우선순위 확인(QEMU, 미실행).
2. `BCA_fillServiceInfo`·`BCA_updateConnectionStatus`를 서비스 19개 구성으로 실행
   (Unicorn/QEMU, 미실행).
3. 수신기 + 폰 시뮬레이터를 3.2 하네스에 연결해 지연·손실·시각 동기 오차·막힘 감시
   측정(미실행).
4. 폰만 사용: 아래 90초 정지 시험, 이어서 송신 앱을 노트북 RFCOMM 서버(BlueZ)에 연결해
   foreground·화면 꺼짐·배터리 제한 시험.

**오프라인으로 닫을 수 없는 차량 질문 하나:** "trial 템플릿으로 ID 15를 추가한 `bds`에서,
무선 AA 세션과 HFP가 연결된 상태로 S25 앱이 서버 UUID에 RFCOMM 연결을 열 수 있고
(사용자 확인 화면 없이), 10분 동안 프레임이 도착하며 AA·HFP·동글 연결이 끊기지 않는가."
**주차 상태 시험으로 충분하며 주행은 필요 없습니다.** 순서는 USB 메뉴 1 설치 → 재부팅 →
동글로 AA → 앱 시작 → 10분 대기 → 메뉴 3 내보내기입니다. 실제 터널 성능은 그 다음
주행 기록에서 처음 판정할 수 있습니다.

## 7. 계획과 첫 소유자 행동

순서: (a) 폰 정지 시험(소유자, 차 없이) → (b) 오프라인 2·3 → (c) 송신 앱(약 3일)과
collector 수신기(약 2일), 설치 템플릿 확장과 시험(약 2일), BETA 융합·재생 시험(약 3일)
→ (d) 주차 1회 시험 → (e) 주행. 개발 기간은 대략 2주이며 추정치입니다.

**첫 소유자 행동:** `phone-imu-study` 연구 APK(SHA-256 `103ad0a4…`, 기존 비공개 빌드)를
S25에 설치합니다. 출처를 알 수 없는 앱 설치 허용이 필요합니다. 앱에서 **정지 시험**을
고르고 폰을 단단한 책상에 화면이 위로 오게 놓은 뒤 **90초 기록 시작**을 누릅니다. 끝날 때까지
만지지 않고, 완료 문구 뒤 **최근 기록 저장**으로 파일을 보냅니다. 폰을 켜고 5분 이상 둔 상태와
가능하면 차 안 온도에서 각 1회씩 하면 좋습니다. 분석기는 `stationary_60s.max_within_window_abs_yaw_deg`
(60초 창 최대 상대 yaw)와 센서 16의 `reported_bias_changes` 횟수를 보고합니다. 60초 값은
b×60에 해당합니다. **≤0.6°면 b≈0.01°/s로 유망, ≤2.5°면 120초 5° 기준 충족, ≥6.9°면 현재 k보다
나쁩니다.** 정지 결과는 주행 성능의 상한 쪽 추정이며 주행 적합 판정이 아닙니다.

## 확인하지 못한 것

- BDS 내부 승인 결정과 보안 0x22의 의미, BlueGo 실제 SDP 등록, RFCOMM 채널 여유.
- BCA 서비스 표 할당 크기(경계 검사는 확인, 용량은 미확인), 종류 0 서비스의 모든 상태 처리.
- `-configFilePath` 처리 순서, 대상 커널의 PTY 버퍼 크기, 막힌 쓰기가 HFP에 미치는 영향.
- 동글 모델과 폰–동글 Bluetooth 지속 여부(소유자 질문), S25 편향·온도 특성.
- 수신기 시제품과 시각 동기 오차 측정(이번 실행 미완료).

## 실행 기록

Ghidra 12.1.4 headless(MAXMEM 1.2–1.5 GB, 공유 잠금 사용)로 `bds`(3afd74c7…),
`svcjcibca.so`(39e238bb…), `libjcibds.so`(f57aaac2…), `libjcicommon_io.so`(fb5c32a0…),
`libjcibds-xlator-bluego.so`를 대상 함수만 디컴파일했습니다. 두 번은 실행 뒤 잠금
디렉터리가 이미 지워져 있었고, 한 번은 가용 메모리 2,051 MB(<2,200 MB)에서 시작했습니다.
이후 실행은 메모리와 jadx 부재를 함께 확인했습니다. 디컴파일 출력과 하네스는 비공개
scratch에만 있습니다. 차량 원시 로그는 로컬에서만 읽었으며 이 문서에는 집계와 함수명만
적었습니다. 제품 `make test`, ARM 제품 빌드, 전체 CMU 기동은 제품 변경이 없어 실행하지
않았습니다.
