# 블루투스 OBD-II 어댑터로 안정된 yaw를 읽을 수 있는가 — 오프라인 타당성 (2026-10-15)

대상은 소유자의 2019 MX-5 ND2(Mazda Connect NA 74.00.324A)입니다. 목표는 **리플래시 없이, 차를 열지 않고**, 표준 진단 커넥터(OBD-II DLC)에서 차량 CAN을 읽어 CMU의 VIP 0x078(영점 변화폭 3.343 카운트, 터널 횡오차 약 100 m)보다 영점이 안정된 yaw를 얻고, 이를 beta.16이 만든 CMU 블루투스 채널로 넣는 길이 실행 가능한지입니다. 기존 기록(`VIP_0X079_GHIDRA_2026-10-12`, `YAW_DECODE_2026-10-10/`, `ND_CAN_LOG_2026-10-03`, `VIP_CAN_MAP_2026-10-03`, `CAN_YAW_NO_REFLASH_2026-10-15`, nd-public-can-dbc 메모) 위에 쌓았습니다. 작업 노트는 `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/obd.IXh7pX/NOTES_obd.md`입니다. 표기: FACT(공개문서/펌웨어) · DATA(계산) · COMMUNITY · INFERENCE.

## (1) ND OBD-II 핀배치와 게이트웨이

- **FACT** OBD-II(J1962) 핀 6=CAN-H, 핀 14=CAN-L이고 ISO 15765-4는 250k 또는 500k이며 500k가 가장 흔합니다. 다만 "최신 차 다수는 게이트웨이가 내부 CAN 접근을 막고 OBD2 통신만 허용"합니다. (csselectronics.com/pages/obd2-explained-simple-intro, 2026-10-15, 신뢰 높음)
- **FACT** ND1/ND2는 **HS-CAN 500 kbps, MS-CAN 125 kbps**이며 yaw·가속도(0x075/0x079, HS_RCM/HS_ABS), 휠 속도 0x215, 차속 0x202, 조향 0x082가 모두 **HS-CAN**에 있습니다. (github.com/berumiya/CAN_DBC_6thGenMazda README·DBC, CC-BY 4.0, 2026-10-15, 신뢰 중간 — 개인 역설계)
- **DATA(저장소)** 공개 ND 원시 CAN 로그(miata.net 게시)에 0x078/0x079/0x215/0x202/0x075가 50~100 Hz로 들어 있고, 같은 공개 폴더에 RaceCapture GPS+IMU 로그가 있었습니다(`ND_CAN_LOG_2026-10-03`). 즉 취미 로거가 ND의 HS-CAN 전체를 실제로 캡처합니다.
- **판정(INFERENCE, 강함):** ND(2016–2021, BCMM 세대, 보안 게이트웨이 도입 이전)는 **DLC 핀 6/14에 HS-CAN을 그대로 노출**할 가능성이 높습니다. 다만 위 공개 로그가 DLC에서 받은 것인지 다른 지점의 탭인지는 확정하지 못했습니다(포럼·RaceChrono 페이지가 403/404로 직접 인용 불가). **이 한 가지는 소유자의 10분 벤치 시험으로만 확정됩니다(아래 (5)).**

## (2) 어댑터 능력

- **FACT** 염가 ELM327은 `AT MA`(Monitor All)로 송수신 주소와 무관하게 버스의 모든 프레임을 표시하며, 이때 **칩은 침묵하고 CAN 모듈이 ACK를 보내지 않습니다(완전 수동 청취)**. `AT CRA`/`CF`/`CM`로 임의 CAN ID 수신 필터를 겁니다. (elmelectronics.com ELM327 데이터시트, 2026-10-15, 신뢰 높음)
- **FACT** STN 계열(OBDLink SX/MX+)은 ELM327 AT를 모두 지원하고, 추가로 `STM`(원시 CAN 모니터, 현재 필터로), `STMA`(전체), `STFPA`/`STFAP` 통과 필터를 가지며 **ISO 11898 원시 CAN**과 Ford MS-CAN을 지원하고 UART 최대 10 Mbps·대용량 버퍼·PowerSave(절전/깨움) 트리거가 있습니다. (OBDLink FRPM, scantool.net, 2026-10-15, 신뢰 높음)
- **INFERENCE** 따라서 원시 0x079 스니핑은 **원리상 ELM327로도 가능하나**, 500 kbps·고부하에서 저속 UART/BT SPP와 작은 버퍼 탓에 **염가 클론은 프레임을 다량 누락**하기 쉽습니다. STN(OBDLink)이 신뢰성·필터·절전에서 명확히 우수합니다. 소유자 어댑터 모델이 미제공이라 그 기기의 가부는 **벤치 시험으로만** 판정됩니다.

## (3) 데이터 경로

- **옵션 A(가능):** OBD 어댑터 →(BT SPP)→ 폰 앱 →(BT SPP, beta.16 채널)→ CMU. beta.16은 순정 `BdsConfiguration.xml`에 연구용 serialPort(ID 15, 128비트 UUID)를 trial `sm.conf`의 `-configFilePath`로만 더해 선언하고, CMU가 연결마다 PTY를 열어 별도 수신기가 32바이트/50 Hz(1.6 kB/s)로 읽습니다(`PHONE_IMU_FEASIBILITY_FABLE`). 폰 앱이 0x079를 yaw로 변환해 같은 프레임 형식으로 중계하면 **수신기·프로토콜을 그대로 재사용**합니다. 폰 자이로보다 나은가: **예 — 거치 각도·틸트 오차가 없는 실제 차량 yaw이고 영점이 더 안정**(0x079 역산 영점폭 0.189 카운트 대 0x078 3.343, 0x075는 0x078 대비 약 27배 안정, `YAW_DECODE_2026-10-10`)합니다. **단** 폰 IMU와 **같은 블루투스 전송**을 쓰고 OBD→폰 홉이 하나 더 붙어 지연은 폰 IMU 이상입니다.
- **옵션 B(불가):** 어댑터가 CMU와 직접 블루투스로 대화 — OBD 어댑터와 CMU serialPort는 **둘 다 SPP 서버**이고 CMU에는 OBD/ELM 클라이언트가 없으며 beta.16에서 CMU는 서버·폰이 클라이언트입니다. 서버끼리는 접속하지 못하므로 **중간 장치(폰 또는 맞춤 ESP32-CAN)가 필수**입니다. (참고: ESP32+CAN을 DLC에 꽂아 CMU의 beta.16 SPP 클라이언트로 직접 붙이면 폰·홉을 없앨 수 있으나 이는 소유자 기존 어댑터가 아닌 신규 하드웨어입니다.)

## (4) 상시 연결 위험·비용

- **배터리:** ELM327 클론은 절전이 부실해 며칠 주차 시 방전 사례가 흔합니다(COMMUNITY, 통설). OBDLink는 PowerSave 트리거로 자동 수면합니다(FRPM, FACT). 권고: 염가 클론은 주차 시 분리.
- **간섭:** `AT MA`/STM 모니터는 ACK를 보내지 않는 수동 청취라 버스·소유자 사용을 방해하지 않습니다(FACT). 다만 어댑터 오설정 시 ACK/웨이크 주입 위험은 있습니다(INFERENCE).
- **법·보증:** 소유자 자기 차량의 표준 진단 포트 읽기로, 보안 통제를 뚫지 않습니다(정보 제공).
- **신뢰성:** 가짜 "v1.5" 클론과 프레임 누락 보고가 많습니다(COMMUNITY).

## (5) 판정·순위·가장 싼 다음 걸음

| 경로 | 판정 |
|---|---|
| OBD yaw (옵션 A) | **DLC에 HS-CAN 노출 + 어댑터 원시 CAN 가능이면 실행 가능**. 신호 품질은 폰 IMU 이상(실 차량 yaw·안정 영점). 전송·AA 공존 위험은 폰 IMU와 동일, 복잡도는 더 큼. |
| 폰 IMU (beta.16) | 이미 구현·추가 하드웨어·DLC 의존 없음. S25 자이로 편향 미검증. |
| 현행 유지(소프트웨어 0x078) | 기준선. |

순위: 벤치 시험이 (a) 0x079가 DLC에 뜨고 (b) 어댑터가 원시 CAN을 받음을 보이면 **OBD yaw가 물리 yaw 소스로 가장 우수**(폰 IMU와 대등하거나 약간 앞섬)하되, 두 홉·폰 앱·수신기 구현이 필요합니다. 그렇지 못하면 폰 IMU가 다음, 안 되면 현행 유지입니다.

**가장 싼 다음 걸음(소유자, 주차·약 10분):** 자기 BT OBD 어댑터를 DLC에 꽂고 폰과 페어링한 뒤, **RaceChrono(무료)**에서 그 어댑터를 "CAN-Bus/OBD-II 장치"로 추가하고 **CAN 모니터**로 들어오는 CAN ID 목록을 봅니다(타이핑 불필요). RaceChrono가 안 되면 ELM 터미널 앱으로 `AT MA`를, OBDLink 앱이면 `STM`을 보냅니다. **확인할 것:** 0x075·0x078·0x079·0x215·0x202가 뜨는지와 대략 속도. 결과 해석: 다섯 ID가 50~100 Hz로 뜨면 → HS-CAN이 DLC에 있고 어댑터가 스니핑 가능 → OBD yaw 추진. 표준 PID 응답만 있고 자발 브로드캐스트가 없으면 → 게이트웨이 차단이거나 어댑터가 PID 전용(클론) → 원시 CAN 가능 어댑터(OBDLink MX+/SX 또는 ESP32) 필요. 접속은 되나 모니터가 빈값이면 → 어댑터가 PID 전용. **보고 숫자는 "보인 ID 목록과 각 대략 Hz" 하나면 됩니다.**

## 확정하지 못한 것
ND의 DLC가 HS-CAN을 직접 노출하는지(강한 정황, 벤치로만 확정), 소유자 어댑터 모델의 원시 CAN 가부, 0x079 하위 2비트 해독식이 제품 입력 계약으로 확정되는지, 실제 터널 GPS 상실 중 0x079 영점 거동(공개 자료는 타 차량). 이 기록은 설계 변경·live ASSIST 활성화·차량 방문을 승인하지 않으며, 저는 어느 질문에서도 분류기·권한으로 중단되지 않았습니다.
