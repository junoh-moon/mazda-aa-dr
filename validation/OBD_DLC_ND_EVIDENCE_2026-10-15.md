# ND MX-5 OBD-II 포트(DLC)가 yaw를 품은 HS-CAN을 노출하는가 — 공개·커뮤니티 증거 연구 (2026-10-15)

유일한 미해결 질문은 "ND의 섀시 CAN ID(yaw 0x075/0x079, 휠 0x215, 차속 0x202, 조향 0x086)가 게이트웨이 뒤가 아니라 **OBD-II DLC 핀에 그대로 오는가**"입니다. `OBD_YAW_FEASIBILITY_2026-10-15`, `ND_CAN_LOG_2026-10-03`, `VIP_0X079_GHIDRA_2026-10-12`, nd-public-can-dbc 메모 위에 쌓았습니다. 작업 노트: `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/dlco.aEIe0P/NOTES_dlco.md`. WebSearch 예산이 세션 공유로 소진(200/200)되어 WebFetch와 `gh`만 사용했습니다.

## 결정적 증거 (1): ND2 소유자가 DLC에서 원시 CAN을 임의 ID로 읽었다

- **FACT** `gitgc/mx5-miata-nd2-obd-can`(MIT, 생성 2026-07-09, 갱신 2026-07-14, 2020 US ND2 소프트톱 2.0·2021 US ND2 RF 2.0에서 시험)은 **"CAN 버스도 OBD2 포트로 접근된다"**고 명시하고, **OBDLink CX를 CAN 모드로** OBD 분배기를 통해 **DLC에 꽂아** 원시 CAN을 임의 ID로 읽습니다. 공개한 RaceChrono 설정에 든 CAN 채널: 533(=0x215) 네 휠 속도 `(raw−10000)/360 m/s`, 514(=0x202) 차속 `raw/360 m/s`·RPM `raw/4`, 134(=0x086) 조향 `(16000−raw)*0.1°`, 120(=**0x078**) 브레이크, 80(=0x050) 클러치, 357(=0x165) 기어. (github.com/gitgc/mx5-miata-nd2-obd-can README·`RaceChrono/vehicleProfile.json`, `gh` 조회 2026-10-15, 신뢰 중상 — 차종·하드웨어·휠 순서 물리검증 명시)
- **교차검증(DATA)** 이 식들은 우리가 펌웨어·공개 로그에서 독립적으로 얻은 디코드와 **그대로 일치**합니다(0x215 `(raw−10000)/360`, 0x202 `raw/360`, 조향 좌−/우+). 즉 신뢰 가능한 1차 자료입니다.
- **중요 공백:** gitgc는 **yaw 채널을 싣지 않았습니다**(0x079/0x075 없음). 따라서 "DLC에서 **yaw를 직접** 봤다"는 공개 보고는 아직 못 찾았습니다. 다만 **0x078 자체가 DLC에서 원시로 읽혔고**, 0x079는 같은 HS_ABS 모듈의 **바로 옆 ID**로 같은 버스를 흐릅니다.

## (2) DLC 핀배치·게이트웨이

- **FACT** J1962 핀 6=CAN-H, 14=CAN-L, 흔히 500k. "**다수 최신차는 게이트웨이가 내부 CAN 접근을 막고 OBD2 통신만 허용**"합니다(일반 주의). (csselectronics.com/pages/obd2-explained-simple-intro, 2026-10-15, 신뢰 높음)
- **INFERENCE(강함)** 그러나 ND2에서는 (1)의 섀시 브로드캐스트 프레임(0x215·0x202·0x086·0x078·0x050·0x165)이 **자발적으로 DLC에 도달**하므로, ND2 DLC는 **게이트웨이로 격리되어 있지 않습니다**. ND(2016–2021, BCMM 세대, 보안 게이트웨이 도입 이전)와 일치합니다.
- **FACT(보강)** 개인 역설계 DBC는 ND1/ND2 HS-CAN 500k·MS-CAN 125k를 전부 다루나 캡처 지점은 적지 않았고 SavvyCAN을 권합니다. (github.com/berumiya/CAN_DBC_6thGenMazda README, 2026-10-15, 신뢰 중간)

## (3) comma.ai/openpilot은 ND 증거가 못 된다

- **FACT** openpilot/opendbc 지원 목록에 **ND MX-5는 없습니다**(마즈다는 3·6·CX-5·CX-9뿐). `mazda_2017.dbc`는 533·514·130/134 등 ID를 ND와 공유하나, ND 전용 물리연결 증거는 없습니다. (github.com/commaai/opendbc docs/CARS.md, 2026-10-15, 신뢰 높음)
- comma 기기는 차종별 하니스로 붙으며(기기 포트명 "OBD-C") ND 미지원이라, DLC 질문에 기여하지 못합니다. 따라서 이 질문은 (1)의 직접 증거로 **대체**됩니다.

## (4) 판정과 가장 싼 확인

- **판정: 공개 증거상 ND(ND2) OBD 어댑터가 DLC에서 원시 HS-CAN을 읽을 수 있음이 "확립"됩니다**(높은 신뢰). OBDLink CX(CAN 모드)로 재현됐고, 0x078은 DLC에서 직접 관측됐습니다. **0x079/0x075(yaw)는** 같은 버스·인접 ID라 사실상 존재가 확실하나, **DLC에서 직접 관측한 공개 보고는 아직 없습니다**(남은 유일한 불확실).
- **그래서 소유자 확인은 여전히 결정적이지만 범위가 좁아졌습니다.** 확인할 것은 두 가지뿐입니다: ① 0x079(와 0x075)가 **실제로 뜨는가**, ② 소유자의 **JF327 클론이 500k 고부하에서 프레임을 놓치지 않는가**. 나머지(HS-CAN의 DLC 노출·게이트웨이 부재)는 이미 공개 증거로 해결됐습니다.
- **가장 싼 다음 걸음(주차·약 10분, 기존 계획과 동일):** JF327을 DLC에 꽂고 RaceChrono "CAN 모니터"(또는 `AT MA`)로 **0x079·0x075·0x078·0x215·0x202가 뜨는 ID 목록과 대략 Hz**만 봅니다. 다섯 ID가 50~100 Hz로 뜨면 OBD yaw 추진 확정. 0x079가 뜨지 않거나 클론이 프레임을 다량 누락하면 **OBDLink CX(CAN 모드, (1)에서 ND2 DLC로 입증)**로 교체하면 됩니다.

## 확정하지 못한 것
ND에서 **0x079 자체를 DLC에서 직접 관측한** 공개 보고(0x078은 확인, yaw는 인접 추정), 소유자 2019 ND2의 송신 여부(시험 차종은 2020/2021 ND2), JF327 클론의 500k 무손실 수신. 포럼(miata.net 403, racechrono 404), reddit(차단), web.archive.org(불가)은 직접 인용하지 못했습니다. 이 기록은 설계 변경·차량 방문·live ASSIST 활성화를 승인하지 않으며, 어느 질문에서도 분류기·권한으로 중단되지 않았습니다.

## 출처 표

| 출처 | URL | 조회일 | 신뢰도 |
|---|---|---|---|
| gitgc ND2 OBD/CAN 문서(결정적) | github.com/gitgc/mx5-miata-nd2-obd-can | 2026-10-15 | 중상(차종·HW·물리검증 명시, 우리 디코드와 일치) |
| comma opendbc 지원표 | github.com/commaai/opendbc (docs/CARS.md, mazda_2017.dbc) | 2026-10-15 | 높음 |
| berumiya ND DBC | github.com/berumiya/CAN_DBC_6thGenMazda | 2026-10-15 | 중간(개인 역설계) |
| OBD2/게이트웨이 일반 | csselectronics.com/pages/obd2-explained-simple-intro | 2026-10-15 | 높음 |
