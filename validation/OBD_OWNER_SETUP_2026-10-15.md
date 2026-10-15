# 소유자 OBD 환경 실태 (2026-10-15)

소유자가 제공한 `mx5-obd-dashboard-collection-20261015.zip`(비공개 폴더 private-data/obd/)에서 확인한 사실입니다. 소유자는 이미 ND2에서 Car Scanner로 커스텀 PID를 능숙하게 다루고 있었습니다(공식 지원 범위를 넘는 작업을 직접 해 온 이력).

## 어댑터 (FACT, ELM327 Identifier 로그)
- JFIND JF327, ELM327 v1.5 선언(중국산 복제칩). 2026-09-06 집에서 어댑터만 스캔한 것이며 차에 꽂은 것이 아님.
- 원시 CAN 관련 명령이 응답함: `ATMA`(monitor all), `ATCRA`(수신 ID 필터), `ATCF`/`ATCM`(ID 필터/마스크), `ATCS`(CAN 상태), `ATSP6`/`ATCAF0`. 프로토콜 현재 ISO 15765-4 CAN 11bit/500k.
- 상위(v2.x) 명령(`ATAMC`, `ATAMT`, `ATCSM`, `ATC0/C1`)은 미지원 → 복제칩 한계. 500k 고부하에서 프레임 누락 가능성은 남음(미확인).

## 지금까지 소유자가 한 방식 (FACT, Car Scanner 스크린샷·custom PID 노트)
- 표준 UDS 진단 요청 방식의 custom PID를 씀: 타이어 공기압/온도(TPMS, 스크린샷에서 32.3/29.7/29.9 psi 등 실동 확인), 연료%/45L 환산, 광대역/지령 혼합비(016D 등), 페달 7E0/22032B, 브레이크 760/222B0D.
- 즉 "요청→응답"(UDS) 방식이며, **흘러가는 원시 CAN 브로드캐스트(0x079 등)를 수동 모니터한 적은 자료에 없음.**

## 우리 목표와의 관계
- 우리가 원하는 안정 yaw(0x079/0x075)는 UDS PID가 아니라 **HS-CAN 브로드캐스트**라서, custom PID(요청/응답)가 아니라 **원시 CAN 모니터**(ATMA 또는 RaceChrono CAN)로 읽어야 함.
- ND2 DLC가 원시 HS-CAN을 게이트웨이 없이 노출함은 공개 증거로 확립됨(validation/OBD_DLC_ND_EVIDENCE_2026-10-15.md: gitgc/mx5-miata-nd2-obd-can이 OBD 포트에서 0x215/0x202/0x086/0x078 등 임의 ID 원시 CAN 수신). 0x079 직접 관측 보고만 아직 없음(같은 ABS 버스라 존재는 강한 추정).
- 소유자 어댑터가 원시 CAN 명령을 지원하므로, 원리적으로 이 환경에서 0x079 모니터가 가능. 복제칩의 500k 프레임 누락 여부만 실측 변수.

## 가장 싼 다음 단계 (소유자, 주차 10분, 읽기 전용)
RaceChrono(무료)의 CAN 모니터 또는 Car Scanner/ELM 터미널에서 `ATZ`→`ATE0`→`ATSP6`→`ATCAF0`→`ATMA`. 쏟아지는 줄의 맨 앞 ID에 075/078/079/215/202가 뜨는지와 대략 Hz만 보고. 뜨면 OBD 경로 확정(폰 자이로보다 우위), 누락이 심하면 OBDLink CX(STN칩, CAN 모드)로 교체가 정답. 차량 무영향·읽기 전용.
