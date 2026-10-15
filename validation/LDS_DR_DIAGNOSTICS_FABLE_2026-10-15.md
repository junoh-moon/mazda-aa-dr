# 순정 LDS/VBS DR 진단으로 재플래시 없이 yaw 영점을 읽을 수 있는가 — 오프라인 디컴파일 (Fable, 2026-10-15)

대상: 소유자의 2019 MX-5 ND2, Mazda Connect NA 74.00.324A. 방법: 펌웨어 파일을 스크래치로 복사한 뒤 Ghidra 12.1.4 headless(`-noanalysis` 임포트 + Java 스크립트로 지정 함수만 디컴파일, ARM, 기준 0x10000) 3회 실행(락·메모리·부하 규칙 준수). svcjcilds.so, libjcilds-dbus/driver/steng/util.so, libjcimod_diag.so, svcjcitestdiag.so의 핵심 함수 80여 개를 디컴파일했고 실패한 함수는 없습니다(libjcilds-dbus.so 안의 `UsbMgrEventDRUnitHandler_cb`는 import stub이라 본체는 svcjcilds.so에서 읽음). 메모: `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/ldf.IUzAmf/NOTES_ldf.md`. OEM 코드·바이트는 싣지 않습니다.

## 결론 먼저: 막힘(읽을 값이 없음)

**FACT:** LDS의 "DR 유닛"은 CAN 0x078과 무관한 **별도의 USB 외장 GNSS+DR 수신기(Furuno eRideOPUS 7)** 이며, 소유자 차량에는 장착되어 있지 않습니다(LEGACY 선택, 891건 일치). 설령 장착돼 있어도 D-Bus 진단은 **상태 코드만** 내보내고 각속도·영점·방위 수치는 없습니다. 따라서 "0x078과 같은 영점"조차 아니라 **아무 yaw 정보도 없습니다.** 이전 CAN 연구의 "LEGACY는 GPS 전용, DR 방위 문장 없음"은 코드 수준에서 **확인**됩니다.

## (1) LDS DR 진단의 정체와 값의 출처

- FACT(svcjcilds.so `UsbMgrEventDRUnitHandler_cb`, `USBMGR_USBDevGetDevList_cb`, `lds_wait_DR_event`): USBMGR 장치 이벤트 2/4가 오면 `lds_DrUnitStatus=1`, `lds_gpsAntennaType=2`(DR-Unit), 장치 노드를 포트로 복사하고 `/data_persist/lds_AntennaType.txt`에 'D'를 씁니다. 장치가 없으면 재시도 후 "Selected GPS Device is LegacyGPS", 타입 1, 'L'입니다.
- FACT(libjcilds-driver.so `LDS_DRIVER_DRUnit_Propriatory_ValidateReading`): 파서 서식은 `$PERDSYS,VERSION,OPUS7_SFLASH_MP_64P,…`, `$PERDSYS,GPIO,%c%c`, `$PERDCRI,O`(4·5필드 → SpeedPulse, Reverse==3), `$PERDCRD,R`(→ DRUnitMode), `$PERDCRD,I ,%d,%f,%f,%f,%d`(4필드 → AccelStatus, 8필드 → GyroStatus). **세 개의 실수 필드는 지역변수로 읽은 뒤 버립니다.** DRUnitStatus는 타입 2이면 GNRMC 수신 여부(0/1), 아니면 2(Not connected)입니다.
- FACT: OneTime의 `GyroSelfTest/AccelSelfTest/ResetLearning/SaveLearning` 네 필드는 **어떤 코드도 쓰지 않습니다.** 인터페이스 문서가 언급하는 `$PERDSYS,DRSELFTEST`, `$PERDAPI,STOP,DRPARK`, `START,SIMCOLD`, `START,HOT` 문자열은 LDS 바이너리 전부(ASCII·UTF-16)에 없습니다. 초기화 함수가 0(false)으로 두는 값이 그대로 나갑니다. CMU가 DR 유닛에 쓰는 문장은 `$PERDCFG,NMEAOUT,GGA/GLL`(시작 시)과 `$PERDSYS,VERSION*2C`, `$PERDSYS,GPIO*67`(`driver_drunitDiagnosticThread`)뿐입니다. 테스트 화면 73(`TESTDIAG_createDrDataResponse`)도 버전·상태·안테나·속도펄스·후진·모드·Gyro/Accel status만 표시합니다.
- FACT: LDS 코드는 VBS/CAN/0x116을 전혀 참조하지 않습니다. `dr_saved_values.bin`은 LDS가 아니라 **NNG**의 파일(`jci_cmu_gps_and_sensor.ini` `[dr.distance_gyro] dr_conf="dr_saved_values.bin"`, `use_saved_state=1`)이며, NNG가 0x116(=0x078)에서 정차 학습한 영점·배율입니다. 우리 정차 학습과 같은 입력·같은 방법이라 새 정보가 없고, SD 내비가 없으면 생성되지도 않습니다(경로는 바이너리 문자열에서 찾지 못함).
- FACT(libjcilds-dbus.so `LDS_DBUS_UpdateCurrentPosition`, util `getPositionAACallback`): LEGACY 방위는 NMEA RMC 코스뿐이며 유효 전환 히스테리시스는 7/9 km/h(DR·u-blox는 2/4), 후진등 파일 `/tmp/RvrseLmpReq`로 180° 반전합니다. 자이로는 없습니다.

## (2) 읽기 전용/상태 변경 구분

- 읽기 전용(차량 상태 무변경): `GetSelectedGPS_sync`(전역 반환), `GetDRUnitStatus_sync`(Connected=(타입==2), Status=lds_DrUnitStatus, result 0), `GetReadStatus_sync`, `GetPosition`, `GetLastKnownPosition`, `GetSatelliteInfo`, `GetUbloxDiag`(구조체 복사; LEGACY에선 의미 없음), OneTime/Periodic 신호 구독(LEGACY에선 신호 자체가 오지 않음: `LDS_DRIVER_Open`은 타입 2일 때만 `LDS_DRIVER_DR_DIAG_Init`을 부르므로 `isDRDriverStarted=0`, `LDS_DBUS_SendPeriodicDiagnosticData`는 "request pending"만 기록). 미확인: `GetGPSFirmwareVersion`이 포트 쓰기를 유발하는지.
- 피해야 할 호출: `DRTestScreenRequest(0)`(플래그 설정·1초 타이머 시작, DR 드라이버가 떠 있으면 GPS 시리얼 포트에 `$PERDSYS` 2문장 기록), `DRReloadRequest`(`/tmp/DR_ResetInterface.sh` 실행 경로), `ReadControl(READ_STOP)`(LDS 정지 → AA 위치 소실), `SetLastKnownPosition_sync`(저장 위치 덮어씀), `ResetUBlox/ClearBackupData/SetVehicleTypeTxt`(u-blox GPIO 리셋·초기화). `ResetLearning/SaveLearning`은 메서드가 아니라 신호 필드이며 구현이 없어 호출 자체가 불가합니다.

## (3) VBS 진단 선택(libjcimod_diag.so)

- FACT: `Data_Request(DIDNo 1바이트)` → VIP SPI 0x203, 응답 0x235; `Routine_Request` → 0x204, 응답 0x23e; `GenericMessage_Request` enum 0..4 → 0x1d5에 PID 0x19e/0x1ba/0x1b5/0x199/0x1b8(의미 미확정). 모두 **VIP로 가는 요청**입니다. `svcjcivbs.so`에는 진단 선택표가 없습니다(모듈 로더).
- FACT(CMU 자체 읽기): VIP→CMU 0x206/0x205(GGDS 데이터/루틴 요청) 신호를 `svcjcitestdiag.so`가 받아 답합니다. 자체 읽기 ID는 4 TFT SW 버전, 5 터치 SW 버전, 6 GPS SW 버전(STA8088 버전 문자열), 0xf CMU SW 버전, 0x14 CMU 일련번호, 0x19 DTC, 0x1a 공장초기화 파일 생성+재부팅, 0xd TAU 차량정보(VIP)이고, 0..3·7..0xc·0xe·0x10..0x12·0x16·0x17은 VIP로 되묻습니다. **LDS DR·자이로·yaw·0x078 상태를 읽는 선택은 없습니다.** VIP 측 0x203 DID 표 전수는 M16C 작업이라 하지 않았습니다(기존 기록: 고정된 상태·식별 선택).

## (4) 판정과 순위

- **막힘.** 재플래시 없는 읽기 전용 경로로 얻을 수 있는 yaw 영점은 없습니다. 0x078 유래 값도 아니고, 별도 학습값도 아니며, 하드웨어 자체가 이 차량에 없습니다. **정차 프로브는 불필요**합니다: `GetSelectedGPS_sync=2`, `GetDRUnitStatus_sync=false`는 이미 차량 기록에 있고, 코드가 LEGACY에서 신호를 내지 않음을 확정했습니다.
- 순위: 폰 IMU 트랙(별도 센서, 진행 중) > 0x078 소프트웨어 처리(정차 학습·불확실도) ≫ LDS DR 진단(0). 이 기록은 제품·설정·차량을 바꾸지 않았고 차량 방문을 요청하지 않습니다.
