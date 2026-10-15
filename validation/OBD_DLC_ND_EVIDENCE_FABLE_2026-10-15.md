# ND MX-5의 OBD-II(DLC)에 HS-CAN(yaw 0x075/0x079)이 노출되는가 — 공개 증거 조사 (Fable, 2026-10-15)

목적은 `OBD_YAW_FEASIBILITY_2026-10-15`가 "벤치로만 확정"으로 남긴 한 가지, 즉 **ND1/ND2의 진단 커넥터 핀 6/14에 섀시 HS-CAN이 게이트웨이 없이 그대로 나오는지**를 공개 자료로 판정하는 것입니다. 작업 노트: `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/dlcf.ABB6tv/NOTES_dlcf.md`. 표기: FACT(공개 문서) · COMMUNITY(개인 기록) · DATA · INFERENCE. 검색 엔진 사용 제약(WebSearch 예산 소진, DDG/Yandex 캡차, Brave 429, Mojeek 403, miata.net·drive2 403, web.archive.org 불가)으로 **GitHub API·RaceChrono 포럼 JSON API·みんカラ·mx5manual.com·Racelogic 문서**를 직접 읽었고, 독일어·러시아어 포럼은 검색 경로가 모두 막혀 확인하지 못했습니다.

## (1) 결정적 증거: DLC에서 임의 ID의 섀시 CAN을 읽은 ND 사례

- **FACT(상업 문서)** Racelogic VBOX 차량 CAN 데이터베이스 "Mazda MX-5 (ND) 2015-"(2022-11-29): "On this vehicle all information is available via the OBD-II connector, this uses the standard pins of CAN High on pin 6 and CAN low on pin 14", 500 kbps. 채널: 가속·브레이크·클러치 페달, RPM, **조향각, 휠 속도 FL/FR/RL/RR**. yaw는 목록에 없습니다.
- **COMMUNITY(강함)** gitgc/mx5-miata-nd2-obd-can(2026-07, 2020·2021 US ND2 두 대): OBD2 포트에 분배기를 꽂고 OBDLink CX(원시 CAN)+MX+(OBD)로 RaceChrono에 동시 입력. CAN ID 514(0x202 차속·RPM), 120(0x078 브레이크), 134(0x086 조향), **533(0x215 휠 속도 4개, 차를 들어 바퀴를 돌려 코너 확인)** 등. "The CAN bus is also accessible through the ODB2 port."
- **COMMUNITY(강함)** timurrrr/RaceChronoDiyBleDevice README·can_db/mazda_mx5_nd.md(2021-06-28, 2019 ND RF): "main CAN bus available on pins 6 and 14 of the OBD-II port (... NC and ND generation Miatas) ... affordable OBDLink MX+". OpenSportsCar 표: ND 2016-2023 "Abundance of data available over the OBD-II port". RaceChrono 포럼 #2094(2021-07-25): "OBDLink MX+ in the 'Experimental CAN' mode ... 40 updates per second". 단 이 저자는 yaw ID를 **아직 못 찾았다**고 적었습니다.
- **COMMUNITY(강함, 일본)** DBC 저자 berumiya의 みんカラ 정비수첩(2024-12-09): innomaker USB2CAN을 **OBD2 핀 6(CAN_H)/14(CAN_L)**에 직결해 HS-CAN 500 kbps를 Listen-Only로 SavvyCAN 분석(MS-CAN은 별도 케이블). 같은 저자의 DBC가 0x079 `YawRate_Corr`·0x075 `YawRate_Raw`를 HS_ABS/HS_RCM 송신으로 정의합니다. **같은 문서 안에서 "OBD에서 0x079를 봤다"고 명시한 문장은 없습니다.**
- **COMMUNITY** mx5things.blog(2017-02-18, 댓글 2019-07-24): RPi 2채널 로거, "on OBD connector: pin 6 – HS-CAN-H, pin 14 HS-CAN-L / pin 3 MS-CAN-H and pin 11 MS-CAN-L"; MS-CAN 메시지가 HS-CAN에서도 모두 보임.
- **없음** 공개 자료 어디에도 "ND의 DLC에서 0x079(또는 yaw)를 읽었다"는 문장은 찾지 못했습니다. miata.net 스레드(t=735385, 735846, 728337, 738450, p=8845703)는 403으로 본문을 읽지 못했습니다.

## (2) ND DLC 핀배치와 게이트웨이

- **FACT(정비서)** mx5manual.com SH13220 "DLC-2 [EPS CONTROL MODULE]": 단자 **F=HS CAN Hi, E=HS CAN Lo, L=MS CAN Hi, K=MS CAN Lo**, ISO 15031-3(SAE J1962) 준수. 문자→핀 대응(F=6, E=14, L=3, K=11)은 jrx-code/car-tracker의 ISO 15031-3 대조(INFERENCE, 5개 기능이 동시에 일치)이며 Racelogic·mx5things의 6/14와 일치합니다.
- **FACT(정비서)** SM353553·SM353558: CAN 고장 진단이 **DLC-2 단자 F/E에서 직접 전압·접지 단락을 측정**하고, BCM 커넥터·C-38/39/42 등을 차례로 분리하며 같은 단자를 재측정합니다. SM353556의 HS-CAN 노드: PCM·**DSC HU/CM**·EVP·EPS(가지 A), BCM(C-38/39 경유)·TCM·앰프·FSC·AFS·CMU·SAS·클러스터. 페이지에 "gateway"라는 단어는 없습니다. → **INFERENCE(강함)**: DLC-2는 HS-CAN 선로에 전기적으로 직결된 수동 탭이며, 0x079·0x215를 내는 DSC HU/CM과 CMU가 같은 버스에 있습니다.
- **FACT/COMMUNITY(대조군)** 2024+ ND(해외 ND3)는 다릅니다. drewid74/2024-nd3-mazda-obdii·berumiya 저장소 이슈 #25(2026-06-23): "The ND3 platform change means the OBD port no longer exposes raw CAN traffic"; "All prior ND1/ND2 PID work ... was built on the old architecture". berumiya 블로그(2025-03-23): 신형은 OBD2 CAN이 BCM에 1:1로 가고 BCM이 방화벽 역할(저자 추정). 즉 **DLC 격리는 2024년식부터**이고, 2019년식 ND2는 구 구조입니다.

## (3) openpilot/comma의 접속 방식

- **FACT** opendbc `CarHarness.mazda`, "Mazda vehicles require comma power"; comma.ai/setup: 하네스는 **룸미러 뒤 LKAS 카메라 커넥터**에 끼우고 OBD-II는 comma power(전원)용. 지원 차종은 CX-5/CX-9/Mazda3/6뿐이며 **ND는 미지원**. mazda_2017.dbc(0x075/0x078/0x079/0x215 포함)는 CX-5 계열 커뮤니티 포팅(Jafar Al-Gharaibeh, 2020-2022)입니다.
- **INFERENCE** comma가 카메라 커넥터를 쓰는 이유는 LKAS 조향 메시지를 **가로채어 주입**해야 하기 때문(포트 격리 증거가 아님)입니다. 이 사실은 ND DLC 질문에 중립입니다.

## (4) 판정

| 질문 | 판정 |
|---|---|
| ND1/ND2 DLC 핀 6/14 = HS-CAN 직결 | **확립(FACT 2건 + 독립 COMMUNITY 4건, 2016–2021·US/JP)** |
| 그 HS-CAN에서 0x215/0x202/0x086 임의 ID 수신 | **확립(gitgc·Racelogic·timurrrr)** |
| 0x079(yaw) 자체를 DLC에서 읽은 보고 | **없음** — 그러나 0x079는 0x215와 **같은 송신기(DSC HU/CM)·같은 버스**이고 DLC는 필터 없는 수동 탭이므로 **INFERENCE(강함): 도달함** |
| 소유자 어댑터(ELM327급)가 500 kbps 버스에서 0x079를 누락 없이 받는가 | **미확정** — 유일하게 남은 변수 |

따라서 "DLC가 게이트웨이로 격리됐다"는 가설은 공개 증거로 **기각**됩니다. 남은 질문은 버스가 아니라 **어댑터**입니다. 가장 싼 확인은 이미 기록된 주차 10분 절차이되, 범위를 좁혀 `ATCRA 079`(수신 필터)+`ATMA`로 **0x079 한 ID만** 받아 초당 프레임 수(공칭 50 Hz)를 세면 됩니다. 필터 없이 `ATMA`만 쓰면 염가 클론은 1,000 fps 이상의 버스 부하에서 누락할 수 있어(INFERENCE) 결과가 "안 보임"으로 오판될 수 있습니다. 이 기록은 차량 방문을 요청하거나 승인하지 않습니다.

## 확정하지 못한 것

0x079가 DLC에서 실제로 관측된 공개 로그(레포의 공개 ND CSV는 cabana/SavvyCAN `time,addr,bus,data` 형식·bus 0으로 0x079를 담지만 채집 지점은 403으로 확인 불가), 독일어·러시아어·Fiat 124 포럼, 2019 한국 사양 배선이 정비서와 동일한지(정비서는 전 세계 ND 공용으로 추정), 소유자 어댑터의 실제 처리율. 어떤 질문에서도 분류기·권한으로 중단되지 않았습니다.

## 출처표

| 출처 | 검색일 | 종류·신뢰 |
|---|---|---|
| racelogic.co.uk/_downloads/vbox/Vehicles/Other/Docs/Mazda-MX-5 (ND) 2015-.pdf | 2026-10-15 | FACT, 상업 장비사, 높음 |
| mx5manual.com/page.html?docid=SH13220, SM353553, SM353556, SM353558 | 2026-10-15 | FACT, 정비서 전재, 높음 |
| github.com/gitgc/mx5-miata-nd2-obd-can (2026-07) | 2026-10-15 | COMMUNITY, 사진·영상 동반, 높음 |
| github.com/timurrrr/RaceChronoDiyBleDevice, /OpenSportsCar; racechrono.com/forum/api/discussions/2094, 2795, 1854 | 2026-10-15 | COMMUNITY, 2019 ND RF, 높음 |
| minkara.carview.co.jp/userid/615186/car/3285399/8040505/note.aspx (2024-12-09); /userid/615186/blog/48327298/ (2025-03-23); github.com/berumiya/CAN_DBC_6thGenMazda(+issue 25) | 2026-10-15 | COMMUNITY, DBC 저자, 높음 |
| github.com/drewid74/2024-nd3-mazda-obdii | 2026-10-15 | COMMUNITY, 2025 ND3, 중간 |
| mx5things.blog/2017/02/18/can-bus-sniffer/ (2017, 댓글 2019) | 2026-10-15 | COMMUNITY, 중간 |
| github.com/jrx-code/car-tracker docs/06-can-obd.md | 2026-10-15 | COMMUNITY, 계획 문서(미측정), 중간 |
| 21stcenturyfiat124spider.wordpress.com/2016/11/18/obd2/ | 2026-10-15 | COMMUNITY, 중간 |
| minkara kz1000a1 NC CAN(2023-09-26) | 2026-10-15 | COMMUNITY, NC 기준, 낮음 |
| opendbc values.py·docs_definitions.py; comma.ai/setup; github.com/commaai/openpilot/wiki/Mazda | 2026-10-15 | FACT, 높음 |
| forum.miata.net t=735385/735846/728337/738450, p=8845703 | 2026-10-15 | **403, 미열람**(검색 스니펫만) |
