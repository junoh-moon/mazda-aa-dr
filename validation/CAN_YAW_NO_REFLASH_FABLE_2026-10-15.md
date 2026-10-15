# 재플래시 없이 CAN yaw를 개선할 수 있는가 — 오프라인 타당성 (Fable, 2026-10-15)

소유자의 2019 MX-5 ND2(Mazda Connect NA 74.00.324A)를 대상으로, 펌웨어를
재플래시하지 않고 외부 OBD/CAN 장치나 폰 없이 **CMU가 이미 가진 경로만으로** 더
안정적인 yaw(계단식 영점이 작은 0x075 계열 또는 보정된 진단 yaw)를 얻을 수 있는지
오프라인으로 검토하였습니다. 기존 기록을 토대로 삼고 결론을 반복하지 않습니다. 방법
메모: `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/cnf.NPg0Bw/NOTES_cnf.md`. 이번에
Ghidra/QEMU는 돌리지 않았고(기존 기록의 M16C 재구성·ARM 실행을 재사용), ARM 클라이언트
라이브러리 심볼·문자열 1건만 새로 확인하였습니다. 펌웨어 바이너리·덤프·오프셋은 싣지
않습니다.

## (1) 주행 중 UDS/진단 폴링으로 YAW_RATE_C/R 읽기 — 막힘

- FACT(신규): CMU의 진단 클라이언트 `libjcivbsdiagclient.so`의 export는
  고장/DTC 읽기·삭제, `Routine_Request`, `GenericMessage_Request`,
  `Data_Request/Response`, 차량정보(VehicleInfo/TAU), TestScreen 상태 등
  **IPC 스텁**뿐입니다. yaw·dsc·sas·did·pid·gyro·module·uds 문자열은 **한 건도
  없습니다**. DSC의 PID/DID 선택 상수를 담고 있지 않습니다.
- FACT(기록): 서버 측 `VBS_DIAG_Data_Request_svc`는 SPI 0x203으로 **한 바이트
  선택 번호**를 보내고 VIP는 **고정 선택지**의 상태·식별 데이터를 돌려줍니다. 임의
  CAN ID/바이트 범위를 읽는 명령이 아닙니다(ACQUISITION, VIP_0X079).
- INFERENCE: 이 인터페이스는 CMU/텔레매틱스의 **고장진단·루틴·차량정보** 창구이지,
  DSC에 ReadDataByIdentifier를 걸어 YAW_RATE_C/R를 주기적으로 읽는 라이브 PID
  리더가 아닙니다. 따라서 DSC가 속도에서 진단에 응답하는지(세션/속도 게이트)는 **무의미
  (경로 자체가 없음)**합니다.
- 확정 못 함: 서버 측 선택표 전수, `GenericMessage_Request`/`Routine_Request`가
  DSC로 UDS 읽기를 중계하도록 강제될 여지(PID/DID 근거 없음; `svcjcivbs.so`
  디컴파일 필요).

## (2) 재플래시 없이 설정만으로 VIP 전달 변경 — 반증/막힘

- FACT(기록, 2개 모델 교차확인): `Vip_Config.xml`/`OPT_CONFIG_*`는 하트비트·LIN·
  전원 감시 파라미터이며 **CAN 메시지의 보관 길이를 바꾸거나 메시지를 추가하거나
  원시 CAN을 중계하지 못합니다**(VIP_0X079).
- FACT(기록): 전달 필드 선택 마스크(예 SPI 0x119의 0x7e)는 **VIP 펌웨어 코드의 송신
  분기**에서 정해지며 `MODCAN_GetDataOnePIDMask`가 이를 소비합니다. 마스크 비트를
  바꾸면 뒤 필드의 해석 위치도 밀립니다(MOVING_AND_STEERING).
- 한계: 이번 세션에서 `Vip_Config.xml` 재열람은 분류기에 의해 **거부**되어, 역할은
  위 기록에 의존하였습니다.
- 판정: 0x079 바이트 3–5나 조향각을 전달 마스크에 추가하려면 VIP **펌웨어 변경**이
  필요합니다. 설정으로는 불가합니다.

## (3) VIM 시험 API가 물리 버스를 건드릴 수 있는가 — 확정: 불가(루프백)

- FACT(기록): 시험 요청 해석기→송신 helper→`VIMC_SendMessage` serializer를 원본
  ARM으로 8사례 실행한 결과, 작성 ID 0x079/0x116/0x203은 **VIM IPC envelope→SPI
  header**로 보존되고, 채널 2·미연결·닫힘·길이 불일치는 **송신 없이 오류**였습니다.
  이 ID들은 CAN ID가 아니라 **SPI 메시지 ID**입니다(PHONE_IMU, ACQUISITION).
- FACT(기록): VIP(M16C)에는 **물리 CAN 프레임을 송신하거나 임의 CAN ID를 되읽는
  SPI 명령이 없고**, 리눅스 VBS에는 원시 CAN 통과/청취 전용 모드가 없습니다
  (VIP_0X079의 수신·디스패치 재구성).
- 확정 결론: VIM 시험 API는 **SPI 전송계로 들어가는 루프백/시험 envelope**이며, 차량
  버스에 프레임을 내보내거나 임의 CAN을 되읽는 도달 가능한 모드는 없습니다. 잔여:
  vim_app/VIP의 모든 비공개 opcode를 전수하지는 못했으나, 재구성된 VIP 디스패치에
  원시-CAN 송신 핸들러가 없습니다.

## (4) 전달되지만 미해독된 필드 — 더 안정적인 yaw는 없음

현재 CMU가 이미 받는 VIP→CMU 내부 메시지를 전부 재점검한 결과(VIP_CAN_MAP,
VIM_CHANNEL_CAPTURE):

- 0x116: +1..+3 yaw 합계·개수(**사용 중**), +4/+9 Qf(=0x078의 **종가속도·브레이크용**),
  +5..+6 종가속도, +7..+8 브레이크 압력. **yaw 전용 Qf·품질·영점 비트는 없음**(0x078
  yaw 자체에 Qf 없음, 공개 KCD와 일치).
- 0x169: +1 Qf(0x079용), +2..+3 **횡가속도**(구심가속도 거동, yaw 아님).
- 0x15B: 차속·rpm·상태. 0x119: 조향 PID 0xd904(마스크 0x7e로 **제외**). 0x118 후진,
  0x100 휠, 0x11C 속도 플래그.
- 결론: 전달 스트림에 **0x078보다 안정적인 yaw나 보정 완료/영점 필드는 없습니다.**
  실제로 영점이 ~27–30배 안정적인 0x075와 0x079 바이트 3–5는 VIP가 **수신·보관조차
  하지 않습니다**(VIP_0X079).

## (5) 그 밖의 CMU↔CAN 접점과 LDS DR 진단

- FACT(기록): 리눅스로 가는 **제2 CAN 채널은 없음**(VIM 채널은 SPI). 런타임에 더 많은
  CAN을 노출하는 공장/서비스 모드는 확인되지 않음.
- FACT(기록): LDS D-Bus에 `OneTimeDRDiagnostics`(resetLearning/saveLearning/
  gyroSelfTest, boolean 신호 필드), `PeriodicDRDiagnostics`(gyroStatus/dRUnitMode),
  `com.jci.lds.control`의 GetDRUnitStatus_sync/DRTestScreenRequest/DRReloadRequest
  가 **존재**합니다. 이는 **DR 수신기 자체의 융합 상태**를 뜻합니다.
- FACT(기록): 소유자 차량은 **LEGACY(2)**, isUblox=0(891건 일치, VEHICLE_AUDIT).
  따라서 리눅스까지 들어오는 u-blox HNR-INS 보정 각속도(0.001°/s, zAngRateValid)
  경로는 **이 차량에는 해당하지 않습니다.**
- INFERENCE: LDS DR 진단은 재플래시 없이 D-Bus로 **읽을 수 있으나**, CAN 0x078 영점이
  아니라 DR 수신기의 gyro/보정 상태를 보고하며 CAN yaw로의 대응이 문서화되지 않았습니다.
- 확정 못 함: LEGACY DR 하드웨어의 정체와, 그 D-Bus 진단이 수치화된 보정 각속도/저장
  영점을 노출하는지 — **정차 1회 읽기 실험**이 필요합니다.

## (6) 경로별 판정·순위와 가장 싼 다음 단계

가치 기준: 영점 불확실도(≈0.17°/s)를 없애는 안정 yaw는 터널 횡오차 ~100 m에 해당.

| 경로 | 가치 | 노력/위험 | 판정 |
|---|---|---|---|
| 0x075 / 0x079 b3–5 확보 | 높음 | VIP 재플래시(벽돌 위험) | **막힘** |
| DSC YAW_RATE_C/R 라이브 UDS | 높음 | 경로 없음·안전ECU 게이트 | **막힘** |
| VIM 시험 API로 버스 접촉 | — | — | **막힘(루프백)** |
| 설정으로 전달 마스크 확장 | 높음 | — | **막힘(코드)** |
| LDS DR 진단 읽기(LEGACY) | 불확실 | 낮음(D-Bus 읽기) | **정차 1회 실험으로 판가름** |
| 폰 IMU / SW로 0x078 처리 | 중 | 별도 트랙 | 기존 진행 |

재플래시 없이 **실행 가능한 유일한 신규 접점은 LDS DR 진단 읽기**이나 가치가
불확실합니다. **가장 싼 다음 단계(오프라인 우선)**: `svcjcivbs.so`의
`VBS_DIAG_Data_Request_svc` 선택표와 `GenericMessage_Request`/`Routine_Request`를
표적 디컴파일하여, 기존 진단 선택 중 DSC의 라이브 PID/DID를 중계하는 것이 하나라도
있는지 **차량 없이 확정**합니다(Ghidra lock·메모리 규칙 준수). 오프라인으로 판가름나지
않을 때에 한해, **정차·시동 상태의 읽기 전용 1회 프로브**(LDS PeriodicDRDiagnostics/
OneTimeDRDiagnostics와 몇몇 VBS_DIAG 선택을 읽어 DR 유닛·VIP가 실제로 무엇을 돌려주는지
기록)만 제안합니다. 주행·폰·재플래시는 불필요합니다.
