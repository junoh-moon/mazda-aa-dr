# 리플래시 없이 더 안정된 CAN yaw를 얻을 수 있는가 — 오프라인 타당성 (2026-10-15)

대상은 소유자의 2019 MX-5 ND2, Mazda Connect NA 74.00.324A입니다. 질문은 CMU가 **어떤 펌웨어도 다시 플래시하지 않고, 외부 OBD/CAN 장치나 폰 없이**, 이미 가진 경로만으로 0x078보다 영점이 안정된 yaw(0x075·0x079 계열 또는 보정된 진단 yaw)를 확보할 수 있는가입니다. 기존 기록(`VIP_0X079_GHIDRA_2026-10-12.md`, `YAW_DECODE_2026-10-10/`, `reports/CMU yaw 영점 보정 신호.md`, `VIP_CAN_MAP_2026-10-03.md`)의 결론을 반복하지 않고 그 위에 새 증거를 쌓았습니다. 작업 메모는 스크래치의 `cno.*/NOTES_cno.md`에 있습니다(휘발성, 저장소에 넣지 않음). 펌웨어 바이너리는 읽기 전용 원본을 스크래치로 복사해 분석했고, 코드·주소·바이트 덤프는 싣지 않습니다.

## (1) DSC 진단 폴링(YAW_RATE_C/R)

**차단(blocked).** 진단 모듈 `libjcimod_diag.so`의 D-Bus 인터페이스 `com.jci.vbs.diag`를 전수 확인했습니다(FIRMWARE FACT). `Data_Request`의 인수는 **DIDNo 한 바이트**뿐이고, 응답은 CMU가 보유한 고정 DID 집합(VehicleInfo·TAUVehicleInfo: 사양·목적지·바디스타일·조향 위치 등 구성 데이터)입니다. 대상 모듈을 지정하는 주소 인수가 없습니다. `GGDSData_Request`·`GGDSRoutine_Request`는 **CMU로 들어오는** 신호이고 CMU가 `GGDS_*_Response`로 답합니다. 즉 CMU는 진단 **서버(피시험 ECU)**이며, DSC/SAS에 ReadDataByIdentifier를 **보내** YAW_RATE_C/R을 받는 클라이언트 경로가 인터페이스에 존재하지 않습니다. 앞 기록이 본 SPI 0x203 `VBS_DIAG_Data_Request_svc`의 "1바이트 선택"은 바로 이 CMU 자체 DID 선택자이지, 임의 모듈·임의 DID 요청이 아닙니다. 따라서 주행 중이든 정차 중이든 이 경로로 DSC yaw를 폴링할 방법은 없습니다. (DSC가 고속에서 진단을 거부하는지 여부는 경로 자체가 없어 무의미합니다.)

## (2) 리플래시 없는 VIP 설정 변경

**차단.** 영속 VIP 설정의 실제 내용을 확인했습니다(FIRMWARE FACT: `jci/vcm/Vip_Config.xml`, `Vip_ALOGMON_Config.xml`, 소비자 `vcm_autoconf`). 명령 어휘는 `OPT_CONFIG_HBMON`(하트비트), `OPT_CONFIG_LINHW`/`LIN_1`/`LIN_2`(LIN), `OPT_CONFIG_PWRMON`(전원), `OPT_CONFIG_ADCONV`(버튼 타입)뿐이고, `reflash-path`·`configure-path`는 비어 있으며 `autosensor allow="no"`입니다. **어느 CAN ID를 받을지, 어느 바이트를 SPI로 전달할지, 보관 길이를 얼마로 할지 바꾸는 설정 항목이 없습니다.** 수신 수락표·전달 마스크·보관 길이는 모두 VIP 플래시의 상수표(앞 기록의 해시/핸들표, 최소 DLC표, 보관 길이표)이고, 0x240→0x119 조향을 가리는 선택 마스크 0x7e도 VIP 송신 분기가 프레임에 넣는 값입니다. 설정으로 0x079 바이트 3–5나 0x075를 전달 대상에 추가할 수 없으며, 바꾸려면 VIP 리플래시가 필요합니다(소유자 제외).

## (3) VIM 시험/진단 API가 물리 CAN에 닿는가

**정리됨: 루프백은 아니나 yaw 획득 경로는 아님.** 클라이언트 API `VIMC_SendMessage`를 디컴파일했습니다(FIRMWARE FACT, 서술만). 이 함수는 `{헤더-ID, 길이, payload}`를 모아 `IPCAPI_SendServerCommandAsync`(opcode 1)로 **vim_app 서버에 넘기는 얇은 IPC 직렬화기**일 뿐, 내부에 루프백 분기도 CAN 로직도 채널 판정도 없습니다. 실제 SPI 마스터는 `vim_app`(`vimapp_SendMessage`→`IUC_SPI_PopTxMessage`→`/dev/spidev1.0` 쓰기)입니다. 순정의 모든 송신 제어 메시지(`VBS_BUS_CAN_EPAS_Control`, `DSCControl_Status`, `YawRate_BrakeControl`, Regen/SCBS/TSR/BSM/TPMS/RCM 등)가 `VBS_BUS_CAN_SendHandler`→`VIMC_SendMessage`로 **같은 경로**를 씁니다. 그러므로 이 경로는 "밀봉된 시험용 루프백"이 아니라 **실제 전송 경로**이고, 시험 플러그인도 같은 직렬화기를 재사용합니다. 그러나 (a) 송신은 VIP가 인식하는 **고정 송신 어휘**의 메시지 ID만, VIP가 정한 CAN ID·배치로 나가고, (b) 어떤 송신도 CAN 수신 필터를 추가하거나 임의 CAN ID를 되읽게 하지 못하며, (c) DSC에 물리 UDS 읽기를 보내는 ID도 없습니다. 결론적으로 VIP까지 "닿기는" 하지만 **새 yaw를 읽는 수단은 없습니다.** VIP 측 SPI 명령→송신 테이블 전체(M16C)는 이번에 재구성하지 않았으나, CAN 수신/전달이 플래시 상수라는 앞 결론과 (b)로 인해 판정은 바뀌지 않습니다.

## (4) 이미 전달되는 미해독 필드 재검토

**차단: 제2 yaw 없음.** 전달 필드를 신뢰도와 함께 재확인했습니다(FIRMWARE FACT + 앞 기록 종합). 0x116 = 0x078의 yaw 합계+개수(현재 소비) 및 +4..9의 종가속도·브레이크 압력·Qf(높음); 0x169 = 0x079 앞부분 횡가속도(구심가속도로 거동, 높음); 0x15B = 0x202 차속·회전수, 값 변화 시에만(높음); 0x119 = 내부 조향 PID 0xd904이나 `MODCAN_GetDataOnePIDMask`가 마스크 0x7e로 **제외**(높음), 나머지는 0x240 토크 계열; 0x118 = 0x09F 후진(중간). `libjcimod_can.so`의 `gs_yawRateTS`는 **송신** `YawRate_BrakeControl`에 딸린 시각으로 보이며 입력 yaw가 아닙니다(INFERENCE). 0x078보다 안정된 yaw, 별도 yaw 품질/유효 비트, DSC 보정 상태는 전달 집합에 없습니다.

## (5) 그 밖의 CMU→CAN 접점과 LDS DR 진단

**부분 가능하나 yaw 가치 거의 없음.** 제2 CAN 수신 채널·런타임 리스닝 모드는 없습니다(앞 결론 유지). LDS DR 진단은 `libjcilds-driver.so`의 `driver_drunitDiagnosticThread`가 DR 수신기의 **독점 NMEA 문장**에서 `antennaStatus·speedPulse·reverse·drUnitMode·accelStatus·gyroStatus·version`을 뽑아 `com.jci.lds.data`(PeriodicDRDiagnostics)로 올립니다(FIRMWARE FACT). CMU는 이를 **리플래시 없이 런타임에 이미 읽습니다.** 그러나 이것들은 자가진단 **상태·모드 코드**일 뿐, 보정된 yaw 값이나 저장된 영점 숫자가 아닙니다. `resetLearning`/`saveLearning`/`gyroSelfTest`도 독점 NMEA의 명령·불리언 플래그이지 읽어낼 오프셋이 아닙니다. 레거시 NMEA 파서(`libjcilds-nmea.so`)는 `$GPRMC/$GPVTG/$GPGGA/$GPGSA` 등 표준 문장만 해독하므로, 방위는 GPS 유래이고 터널에서 무효화됩니다(융합 DR 방위 문장 없음). 소유자 차량은 LEGACY(2)로 확인됐으므로 u-blox HNR 경로도 해당 없습니다.

## (6) 판정·순위·다음 한 걸음

| 경로 | 판정 |
|---|---|
| (1) DSC UDS 진단 폴링 | **차단**: 클라이언트 경로 부재(CMU는 진단 서버). |
| (2) VIP 설정 확장 | **차단**: 설정 어휘에 CAN 전달 항목 없음, 전달은 플래시 상수(리플래시 필요). |
| (3) VIM 시험 API | **차단(yaw)**: 실전송 경로이나 수신·되읽기 불가, yaw 읽는 ID 없음. |
| (4) 미해독 전달 필드 | **차단**: 전달 집합에 제2 yaw 없음. |
| (5) LDS DR 진단 읽기 | **차량 변경 없이 가능**하나 상태·모드만, yaw·영점 값 없음 → 가치 ≈ 0. |

안정된 영점 yaw는 터널 횡오차 약 100 m 가치가 있으나, **리플래시나 외부 OBD 없이 더 나은 물리 yaw에 닿는 경로는 전무**하며(두 수단 모두 소유자 제외), 유일하게 실행 가능한 신규 접점(LDS DR 진단 읽기)은 yaw 가치가 사실상 없습니다. 이는 병렬 분석(Fable)과 **일치**합니다. 저는 어느 질문에서도 분류기·권한에 의해 중단되지 않았습니다.

**가장 싼 다음 걸음:** 새 yaw를 위한 주차 시험은 권하지 않습니다(오프라인에서 모든 경로가 차단으로 결정됨). 판정을 바꾸지 않는 유일한 미해결 항목은 VIP 측 SPI 명령→송신 테이블 전체 모습뿐이며, 필요하면 **M16C 디스어셈블(오프라인)**로만 확인하면 됩니다. 실질 권고는 현행대로 소프트웨어로 0x078을 다루는 BETA와 폰 IMU 채널(beta.16)을 계속하는 것입니다.

**확정하지 못한 것:** VIP SPI→송신 테이블 전체(판정 불변), 숨은 진단 선택자의 보편적 부재(보편 부정은 증명 불가, 단 인터페이스 형상이 서버 전용을 강하게 시사), `gyroStatus`·`drUnitMode` enum의 의미, 고속 진단 응답 여부(경로 없어 무의미). 이 기록은 설계 변경·live ASSIST 활성화·차량 방문을 승인하지 않습니다.
