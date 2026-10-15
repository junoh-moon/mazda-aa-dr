# 순정 LDS/VBS DR 진단으로 재플래시 없이 쓸 수 있는 yaw 영점을 얻을 수 있는가 (Opus, 2026-10-15)

소유자의 2019 MX-5 ND2(Mazda Connect NA 74.00.324A)에서, 펌웨어를 재플래시하지 않고
CMU가 자기 자신에게 이미 노출하는 LDS DR 유닛 진단과 VBS 진단 선택표만으로 CAN 0x078보다
나은 yaw 영점을 얻을 수 있는지 오프라인으로 조사하였습니다. 다른 모듈의 라이브 PID를
버스로 폴링하는 경로는 범위에서 제외하였습니다. 공유 트리에서 필요한 파일만 스크래치로
복사해 사본만 읽었고(`cp -L`), 핵심 함수는 Ghidra 12.1.4로 실제 디컴파일하였습니다.
방법·근거 메모: `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/ldo.LgI0Ia/NOTES_ldo.md`.

## (1) LDS DR 진단이 노출하는 것과 그 값의 출처 (디컴파일로 확정)

FACT(인터페이스): `libjcilds-dbus.so`에 박힌 introspection XML의 `com.jci.lds.data`는
읽기 메서드 `GetPosition`(PositionAccuracy enum에 POSITION_DR=0x03, Heading, Velocity,
H/V DOP), `GetLastKnownPosition`, `GetGPSFirmwareVersion`("STA8088.XXXx"),
`GetSatelliteInfo`, `GetUbloxDiag`(u-blox 전용)와, 신호 `GPSDiagnostics`,
`OneTimeDRDiagnostics`(DRUnitVersion, AntennaStatus, GyroSelfTest, AccelSelfTest,
ResetLearning, SaveLearning), `PeriodicDRDiagnostics`(DRUnitStatus, SpeedPulse, Reverse,
DRUnitMode=GNSSonly/DRonly/mixed, GyroStatus 0..7, AccelStatus 0..8)를 가집니다.

FACT(출처, 디컴파일): 이 값들의 생성 로직은 `libjcilds-driver.so`에 있고 `svcjcilds.so`의
`LDS_DBUS_Update*DiagnosticData`는 D-Bus 전달만 합니다. 핵심 함수
`LDS_DRIVER_DRUnit_Propriatory_ValidateReading`는 **DR 유닛이 시리얼 포트(/dev/ttymxc2,
lds.xml)로 보낸 독자 NMEA 문자열을 `sscanf`로 파싱**합니다: `$PERDSYS,VERSION`→DRUnitVersion,
GPIO 두 문자('H'/'L')→상태 enum, `$PERDCRI,O`의 두 정수→SpeedPulse·Reverse(값<3→OFF, 3→ON),
`$PERDCRD,R`→DRUnitMode, `$PERDCRD,I`의 5필드 중 끝 두 정수→AccelStatus·**GyroStatus**.
파싱 결과는 뮤텍스로 보호된 구조체에 담겨 `LDS_DRIVER_DR_Periodic/OneTime_DiagnosticDataUpdate`가
콜백(opcode 4/3)으로 상위에 올립니다.

따라서 **GyroStatus와 DRUnitMode는 CAN 0x078/0x116이 아니라 GPS/DR 수신기 자체의 상태
문자열에서 나오는 정수 건강지표·모드 enum**입니다. yaw 수치도, 자이로 바이어스·영점도
아닙니다. `svcjcilds.so`와 `libjcilds-driver.so`의 의존 라이브러리·심볼 어디에도 CAN/VBS
연결이 없어 LDS 경로는 0x078을 **보지도 못합니다**. 즉 "우리가 이미 가진 0x078 영점과 같은
값"도 아니고, 이 경로에서 `dr_saved_values.bin` 같은 저장 영점을 읽지도 않습니다(그 파일은
NNG 프로세스 소유이며 LDS와 별개). 여기서 참조되는 자이로는 **차량 SAS가 아닌 수신기 내장
MEMS 센서**입니다. 이로써 선행 CAN 연구(LDS DR는 near-zero·레거시 GPS-only)를 코드 수준에서
**확인·정교화**합니다(반증 아님).

## (2) 읽기의 수동성·안전성 (파킹·시동 ON 기준)

FACT: 수동(버스 쓰기·상태변경 없음)으로 안전한 호출은 `com.jci.lds.data`의 GetPosition,
GetLastKnownPosition, GetGPSFirmwareVersion, GetSatelliteInfo, GetUbloxDiag와,
`com.jci.lds.control`의 `GetDRUnitStatus_sync`, `GetReadStatus_sync`,
`GetSelectedGPS_sync`, `GetLastKnownPosition_sync`, 그리고 `GPSDiagnostics` 신호 구독입니다.

FACT(디컴파일): **반드시 피해야 하는 상태변경** 호출 — `driver_drunitDiagnosticThread`가
`DRTestScreenRequest`의 세마포어에 깨어 `LDS_DRIVER_WriteDataToPort`로 **명령 문자열을 포트에
직접 씁니다**(자이로 셀프테스트, SIMCOLD=학습 초기화, HOT=학습 저장). 즉 OneTimeDRDiagnostics
생성 자체가 능동 쓰기입니다. 그 밖에 `ReadControl`, `DRReloadRequest`,
`SetLastKnownPosition_sync`, `SetVehicleTypeTxt`, `ResetUBlox`, `ClearBackupData`도 쓰기이므로
금지합니다. PeriodicDRDiagnostics 신호는 구독만 하면 수동이나, 테스트화면이 켜져야 방출됩니다.

## (3) VBS 진단 선택 중 자기 DR/yaw 상태 읽기가 있는가 — 없음

FACT: `libjcimod_diag.so`의 `com.jci.vbs.diag` XML 전수 — 메서드는 고장/DTC 읽기·삭제,
`Routine_Request`(동작), `Data_Request(DIDNo)`·`GenericMessage_Request`,
VehicleInfo/TAU(vehicle_type, body_style, destination, steering_wheel_location, cmu_config,
driveChart), Send_SXM_ESN, TestScreen 상태뿐입니다. yaw·gyro·DR·heading·position·wheel을
읽는 DID/선택은 **하나도 없습니다**(ALLCAPS 상수는 전부 DTC 이름). `svcjcivbs.so`는 범용
디스패처(select 루프)이고, yaw를 만지는 유일한 모듈 `libjcimod_can.so`(com.jci.vbs.mod_can)는
라이브 Yaw Rate를 **전달만** 하며 저장·보정 영점이 없습니다. 따라서 기존 VBS 선택 중 CMU 자신의
저장 yaw 영점을 돌려주는 자기-읽기는 없습니다(Fable 결과와 일치).

## (4) 판정과 다음 단계

**판정: 재플래시 없는 읽기 전용 경로로 쓸 만한 yaw 영점은 존재하지 않습니다(막힘).** LDS DR
진단은 별도 GPS/DR 수신기의 건강지표·모드와 GPS 융합 Heading/Velocity일 뿐, yaw 수치도 영점도
아니며, 소유자 차량은 LEGACY(GetSelectedGPS=2, 펌웨어 STA8088.*)라 DR 유닛의 `$PERD` 문장
자체가 나오지 않아 DRUnitStatus는 "Not connected"가 됩니다. 유일한 각도 출력인 Heading은 위성이
필요해 터널·주차장에서 무의미하므로 G1·G2·G3에 도움이 없습니다.

INFERENCE: 이 질문은 오프라인으로 이미 판가름났으므로 **정차 프로브는 불필요**합니다. 굳이
한다면 GetSelectedGPS_sync·GetDRUnitStatus_sync·GetPosition 읽기와 GPSDiagnostics 구독이
전부 수동으로 안전하나, 결과는 "LEGACY·DR 미연결·Heading은 GPS-only"를 **확인**할 뿐 없는
영점을 만들어내지 못합니다.

확정 못 함: LEGACY STA8088 유닛이 실제로 돌려주는 GyroStatus/DRUnitMode 런타임 값(프로브 없이
미관측)이나, u-blox 장착 차에서 GetUbloxDiag의 headVeh가 보정 방위를 줄 가능성 — 둘 다 이
차량의 판정은 바꾸지 못합니다.

**순위:** 폰 IMU 트랙이 이 경로를 압도합니다. 폰 IMU는 터널·주차장에서 작동하는 독립 각속도원인
반면, LDS/VBS 진단은 쓸 만한 yaw 영점을 전혀 내놓지 못합니다. 이번 조사는 제품 코드·설정·차량
상태를 바꾸지 않았고 새 차량 방문이나 주행 시험을 승인하지 않습니다.
