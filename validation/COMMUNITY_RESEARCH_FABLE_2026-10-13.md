# 해외 커뮤니티·공개 자료 조사: "우리와 같은 문제를 겪은 사람이 있고, 답이 있는가" (FABLE, 2026-10-13)

범위: 공개 웹(WebSearch 47회, 영·독·일·러·한·중·불), 공개 GitHub(`gh api`로 파일만 읽음), NHTSA 공개 정비회보 PDF. 로그인·가입·게시·다운로드(실행 파일) 없음. 403으로 막힌 페이지는 검색 요약만 썼고 표에 표시했습니다. 질의 기록과 받은 파일은 `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/crf.izMFRT/NOTES_crf.md`(같은 폴더 `gh/`, `*.txt`)에 있습니다. 표기: PUBLIC DOC / COMMUNITY / INFERENCE / FIRMWARE FACT(기존 기록 인용). 어떤 포럼 주장도 74.00.324A·이 차에 대한 증명으로 취급하지 않았습니다.

## 1. 결론 먼저

1. **Mazda 1세대 + Android Auto(이하 AA)에서 터널·지하주차장 DR 문제를 다룬 글은 어느 언어에서도 찾지 못했습니다.** 마쯔다 포럼의 GPS 글은 안테나 불량·잘못된 위치·펌웨어 갱신 이야기뿐입니다. 네이버 지도를 해외에서 다룬 글도 없습니다.
2. 가장 가까운 선례는 **아우디 MIB2 High의 "AA 내비가 진행 방향을 거꾸로 표시" TPI(2018)**입니다. 원인은 헤드유닛이 GPS 안테나의 진행 방향을 쓰지 않은 것이고, **Google이 AA 쪽에 우회책을 넣었다**고 적혀 있습니다. G3(최초 연결 방위)와 직접 맞닿습니다.
3. 마쯔다 2세대(2024-25 MX-5 포함) 정비회보는 순정 DR 입력이 **차속·GNSS·조향각**이고, **ACC OFF 지점에서 500 m 이상 떨어진 곳에서 ACC ON**이면 위치를 불신한다고 명시합니다. 우리 G3 설계(시동 OFF 때 방위 저장 + 출발 이동 검사)의 OEM 선례입니다.
4. CAN: 공개 DBC 세 가지가 **0x078의 내용에서 서로 다릅니다**(silverchris: 12비트 Yaw, berumiya: 브레이크 압력+종가속도만, opendbc: 13비트 가속도 X/Y). 우리 펌웨어 사실(VIP가 0x078 필드로 0x0116 자이로 메시지를 만듦)을 뒤집는 것은 없지만, 한 번 대조할 가치가 있습니다. yaw 영점 계단에 대한 공개 논의는 없습니다.
5. 폰 쪽 해법(2GIS 센서 내비, HERE SDK VDR, Yandex "По шагам")은 모두 **폰 IMU 또는 수동 모드**이며, 외부 센서로 헤드유닛/앱에 더 나은 yaw 영점이나 방위를 공급하는 공개 제품·프로젝트는 **없습니다**.

## 2. 출처표 (조회일 2026-10-13)

| # | 출처 | 종류/신뢰도 | 게시일 | 말하는 것(FACT) | 닿는 문제 |
|---|---|---|---|---|---|
| S1 | bolidenforum.de TPI 2050566/1-91 "Android Auto navigation shows backwards direction of travel" (직접 조회 403, 검색 요약) | OEM TPI의 포럼 전재 / 중 | TPI 2018-02-27 | 2017 A3/A4/A5/Q5 MIB2 High(7UH): AA 내비(Google Maps, Waze)에서 화살표가 반대, 계속 "Rerouting". 원인 "MMI not utilizing the direction of travel from the GPS antenna". Google이 AA 소프트웨어에 우회책을 넣었고, MMI는 CW22/17부터 수정판 | G3, AA LOCATION |
| S2 | NHTSA MC-11022726-0001 (Mazda 정비회보, PDF 텍스트 추출) | PUBLIC DOC(OEM) / 상 | 2025-08 | 2019-26 Mazda3, CX-30, MX-30, **2024-25 MX-5** 내비 장착차: "dead reckoning … does not function properly for more than 30 seconds (e.g., due to an abnormality in the vehicle speed signal, GNSS … or steering angle signal)"; 발생 조건에 "Driving immediately after parking in an enclosed area", 긴 터널, "ACC is turned ON at a location more than … 500 meters away from the location where the ACC was last turned OFF"; 복구는 직선 도로 주행 | G2, G3, 순정 DR |
| S3 | NHTSA MC-10143980-9999 = Mazda Service Alert SA-034/18 "Mazda Connect FAQ" | PUBLIC DOC(OEM, 1세대) / 상 | 2018-07-16 | Q9: 시동 때 GPS 품질이 나쁘면 "may not be able to recognize your current location until it acquires better/more GPS signals. This is a normal MAZDA CONNECT operation."; 위치 화면은 GPS 없으면 "known position" 표시 | G3 |
| S4 | NHTSA MC-10134704-9999 = Bulletin 09-007/18 "Navigation system shows wrong location" | PUBLIC DOC(OEM, 1세대) / 상 | 2018-03-22 | 원인은 GPS 수신 불량; 안테나 피더 점검 후 개선 CMU로 교체 | (무관: 하드웨어) |
| S5 | owners-manual.mazda.com MZD Connect v1 "Navigation" | PUBLIC DOC(OEM) / 상 | — | SD 없으면 나침반 표시; "may not indicate the correct bearing when the vehicle is stopped or travelling at a slow speed" | G3(순정도 정지 방위 없음) |
| S6 | bbs.kakaku.com K0000962484 SortID=21703700 (CX-8 2017, 1세대 CMU) | COMMUNITY(JP) / 하 | 2018-03~12 | 자립측위(빨간 마크) 때 100-200 m 이탈, 터널·빌딩가에서 심함; 펌웨어 갱신 후 "이탈이 사라졌다"는 보고 | 순정 DR |
| S7 | trafficnews.jp/post/622672 (야마테 터널) | 미디어 해설 / 중 | 2026-01-12 | 스마트폰 내비는 휠 속도가 없어 정지하거나 "폭주"; 차량 내비는 휠 회전을 쓴다; ETC2.0 ITS 스팟 보정 | 터널 연속성(일반론) |
| S8 | Google 특허 US9554241B2 | PUBLIC DOC(특허, 제품 동작 아님) / 중 | 출원 2015-09-04 | 헤드유닛→폰 위치 공유, 헤드유닛이 GPS 없을 때 DR, 위치에 타임스탬프·정확도 포함 | AA LOCATION |
| S9 | 특허 US9366540B2 (AT&T→Hyundai/Kia 양도) | 특허 / 중 | 출원 2014-10-23 | 차가 휠 회전·조향각·가속도를 폰에 보내고 폰이 DR, GPS로 센서 편향 보정 | (4) 선례 |
| S10 | 9to5google.com/2023/03/13/android-auto-gps | 미디어 / 중 | 2023-03-13 | AA+Google Maps 위치 오류; AA 앱 위치 권한을 끄면 "theoretically" 헤드유닛 안테나 사용을 막는다(추정) | AA LOCATION |
| S11 | waze.uservoice.com 33990550 "Android Auto – Use Car Antenna" | COMMUNITY / 중 | 2018-04-18~2024-08, 314표 | Waze는 AA에서 폰 GPS만 쓰고 Google Maps는 차 안테나를 씀; 한 사용자: 폰 위치 끄니 차 안테나로 완벽 추적되다 30분 뒤 권한 문제로 끊김; 공식 답 없음 | AA LOCATION, 터널 |
| S12 | mgevs.com post 164670 (MG EV) | COMMUNITY / 하 | 2023-02 | AA 8.8 + Google Maps "GPS Signal Lost"는 차와 무관한 앱 버그, 이후 수정 | (무관) |
| S13 | macheforum.com 스레드 25306 "Google Maps via CarPlay – Vehicle Heading Incorrect" | COMMUNITY / 중 | 2023-01 | Mach-E 무선 CarPlay: 30분 이상 주행 뒤 방위가 90-180° 틀어져 계속 재탐색, Apple Maps도 동일, 순정 SYNC는 정상; CarPlay를 끊으면 사라짐 | G3·방위(폰 융합 실패 사례) |
| S14 | macheforum.com 스레드 8270 (Apple 문서 인용, 원문 제목 미확인; taycanforum 403) | COMMUNITY가 인용한 Apple 문서 / 중 | 2022-23 | "Speed information is used by iPhone for dead reckoning and is critical if the vehicle does not have a GPS or GLONASS receiver"; 차량 위성 위치는 iPhone 센서에 더해 사용 | CarPlay 비교 |
| S15 | discussions.apple.com/thread/254846416 | COMMUNITY / 하 | 2023-05~2024-01 | Apple Maps가 유럽·시드니 터널에서 길을 잃음; 사용자: Google Maps·HERE WeGo는 터널에서 계속 됨 | 터널 연속성 |
| S16 | abrp.featurebase.app CarPlay DR 요청 | COMMUNITY / 하 | 2023-02-27 | "The official Apple Maps app supports it(DR)" (사용자 주장, 답변 없음) | 터널 |
| S17 | docs.here.com HERE SDK "get-locations-optimization" | PUBLIC DOC(SDK) / 상 | — | 폰 IMU VDR은 1-2분 안정 주행 보정 필요, "Avoid … tunnels, parking garages … during startup"; AA에서 `enableVehicleSensors(hwManager)`로 차 GNSS·움직임 센서 사용 가능 | (3)(4), 네이버 대안 |
| S18 | developer.android.com Car Hardware API / `CarSensors` | PUBLIC DOC / 상 | — | Car App Library로 차의 compass·accelerometer·gyroscope·location 구독, `ACCESS_FINE_LOCATION` 필요 | AA LOCATION(앱 쪽) |
| S19 | mobile-review.com, rozetked.me (2GIS 센서 내비) | 미디어 / 중 | 2026-07-29 | 폰 가속도·자이로·나침반·기압계로 GPS 없이 추정, **거치대 고정 필수**, 1-2 s 지연; AA/CarPlay 언급 없음 | (4) |
| S20 | comss.ru (Yandex "По шагам") | 미디어 / 중 | 2026-06-16 | GPS 불안정 때 단계별 안내 모드, CarPlay·AA 포함 — 수동 폴백 | (3) |
| S21 | giga.de, phonescoop, slashdot (Google Maps BT 터널 비콘) | 미디어 / 중 | 2024-01~2026-10 | Android 전용, 기본 꺼짐, Waze 비콘 설치 터널에서만 | (3) |
| S22 | github berumiya/CAN_DBC_6thGenMazda `MX5ND_6thGenMazda_HSCAN.dbc` (gh api) | COMMUNITY DBC / 중 | — | 0x075 `YawRate_Raw` 39\|16 (0.01 deg/s, −180), `Lateral_Acc_Raw`; 0x079 `YawRate_Corr` 31\|16, 주석 "[deg/s] = [N-0x200]/0x200*114.4"(사실상 10비트 ±114.4); **0x078은 BrakePressure + Longi_Acc_Corr만**; 0x215 4×16비트 (0.01, −100) km/h; 0x202 속도 0.01 km/h | yaw 디코드 |
| S23 | github silverchris/Mazda3_Canbus_Messages `skyactive_new.kcd` | COMMUNITY KCD / 중 | — | 0x078 길이 6: `Yaw` offset 15 length 12 big-endian, `LongitudinalAcceleration` 13비트, `BrakePressure` 13비트; 0x079 `LateralAcceleration` 13비트; 구 `skyactive.kcd`는 0x078 "YAW_ACCEL_BRAKE" TODO | yaw 디코드 |
| S24 | github commaai/opendbc `mazda_2017.dbc` (raw) | COMMUNITY DBC(comma) / 중 | — | 0x078 BRAKE: `VEHICLE_ACC_X` 5\|13 (0.01, −40), `VEHICLE_ACC_Y` 8\|13 (0.001, −4.096), `BRAKE_PRESSURE` 39\|8; 0x075는 조향각·토크, 0x079는 EPB(미해독); 0x215 WHEEL_SPEEDS, 0x202 ENGINE_DATA SPEED 0.01 | yaw 디코드 |
| S25 | github timurrrr/RaceChronoDiyBleDevice `mazda_mx5_nd.md` | COMMUNITY / 중 | — | 0x202 속도·RPM·가속페달, 0x078 브레이크 위치 비트 28-39, 0x086 조향; "yaw rate, individual wheel speeds" 미해독이라 명시 | yaw 디코드 |
| S26 | github f1xpl/aasdk `SensorTypeEnum.proto`, `GPSLocationData.proto` | 오픈소스 프로토콜 정의 / 상 | 2018 | AA 센서 종류: LOCATION 1, COMPASS 2, CAR_SPEED 3, … DEAD_RECONING 14, ACCEL 19, GYRO 20, GPS 21; GPSLocation = timestamp, lat, lon, accuracy, altitude, speed, bearing | AA LOCATION(기존 사실 재확인) |
| S27 | tech9autorepair.com, techradar (AA 설명 기사) | 블로그/마케팅 / 하 | — | "AA는 차 안테나를 절대 쓰지 않는다" | **우리 사실과 모순(틀림)** |
| S28 | clien.net cm_car 17827033 | COMMUNITY(KR) / 하 | 2023-01-03 | BMW: 댓글 "CarPlay는 차 GPS(터널 OK), AA는 폰 GPS(터널·지하 끊김)" | AA LOCATION(부분 오류) |
| S29 | mracommunity (MyRoute-app), xdaforums 4563963 (403) | COMMUNITY / 하 | 2024 | AA 화살표 반대 — 앱 자체 버그 또는 폰 나침반 설명 | (참고) |

## 3. 열린 문제별로 무엇을 말해주는가

**yaw 영점(0x078 계단)**: 공개 자료 어디에도 0x078 영점·계단·DSC 초기화 논의가 없습니다(ND 요 센서 영점 절차 검색은 Toyota류 일반 자료만). 새로운 것은 **DBC 불일치**입니다. S22는 0x078에 yaw가 없다고 보고, S24는 같은 바이트를 가속도 X/Y로, S23은 12비트 Yaw로 봅니다. 우리 펌웨어 사실(`VBS_BUS_CAN_YawRate_BrakeControl`이 0x078 필드로 0x0116을 구성, 포화 ±77 deg/s 실측)이 가장 강하고, 기존 기록은 S22의 0x075/0x079를 이미 반영하고 있으므로 결론은 바뀌지 않습니다. S22의 0x079 식은 10비트(±114.4 deg/s)이며, VIP가 0x079를 2바이트로 보관한다는 기존 사실과 양립합니다.

**터널 연속성(G1)**: 다른 차·앱에서도 "AA는 터널에서 멈춘다"가 보편적 불만이고(S11, S15, S17), 해법으로 나온 것은 (a) 앱이 차 위치를 받는 경우(Google Maps, HERE SDK, 네이버), (b) 폰 IMU DR(2GIS, HERE VDR: 거치 고정·사전 보정 필요), (c) BT 비콘, (d) 수동 모드(Yandex)뿐입니다. 헤드유닛이 DR 위치를 주입하는 우리 방식과 같은 공개 사례는 없습니다. CarPlay는 Apple 문서(S14)대로 차 속도로 폰이 DR을 하지만 실제 체감은 엇갈립니다(S13, S15).

**최초 연결 방위(G3)**: S1이 핵심입니다. 헤드유닛이 보내는 진행 방향이 틀리면 AA 내비가 거꾸로 가고 재탐색을 반복했으며, Google이 폰 쪽에 우회책을 넣었습니다(내용은 비공개). INFERENCE: 현재 gearhead에는 헤드유닛 bearing을 불신·보정하는 휴리스틱이 있을 수 있고, 그렇다면 우리가 저장 방위를 보내도 폰이 무시하거나 폰 나침반과 섞을 수 있습니다. S2는 OEM이 "ACC OFF 지점에서 500 m 이상 떨어지면 불신"이라는 출발 이동 검사를 쓴다는 선례, S3·S5는 1세대 순정도 시동 직후·정지 중에는 방위를 보장하지 않는다는 사실입니다.

**지하주차장 방위(G2)**: S2의 "Driving immediately after parking in an enclosed area"가 2세대 순정 DR도 밀폐 주차장 뒤 30 s 이상 흔들린다는 OEM 인정입니다. 해법 제시는 없습니다.

**AA LOCATION 동작**: S11(Google Maps는 차 안테나, Waze는 폰), S8(헤드유닛 위치에 타임스탬프·정확도), S18(Car App Library로 앱이 차 센서 구독)은 기존 결론(폰이 차 위치를 받아 앱에 전달, 디코드 시각 스탬프)과 부합합니다. S27·S28의 "AA는 차 GPS를 안 쓴다"는 틀렸습니다.

**네이버**: 해외 자료 없음.

## 4. 이미 알던 것 vs 새로 안 것

- 이미 알던 것: AA 센서 종류와 LOCATION 필드(S26), 헤드유닛 DR 때 위성 0 지침, 네이버 이중 선택기, berumiya 0x075/0x079, NNG DR 구조.
- 새로 안 것: ① 아우디 MIB2 "역방향" TPI와 Google의 폰 쪽 우회책(S1); ② 마쯔다 2세대 DR 입력에 **조향각**이 포함되고 500 m 출발 이동 검사가 있음(S2); ③ 1세대 FAQ의 공식 "시동 직후 위치 불인식은 정상"(S3); ④ opendbc가 0x078을 가속도로 라벨(S24)·berumiya는 0x078에 yaw 없음(S22); ⑤ Waze는 AA에서 차 위치를 쓰지 않음(S11) → Google Maps가 우리 주입 LOCATION의 둘째 확인 앱이 될 수 있음; ⑥ 폰 IMU DR 제품(2GIS 2026-07, HERE VDR)의 전제 조건(거치 고정, 1-2분 보정) — 우리 센서 경로의 장점이 확인됨; ⑦ CarPlay 무선에서 폰 융합이 90-180° 틀어진 사례(S13).

## 5. 따라갈 만한 단서와 가장 싼 확인

1. **첫 연결 LOCATION의 bearing 실태 감사** (G3, S1 동기): 기존 shadow/beta 로그에서 연결 직후 처음 N개의 LOCATION에 bearing이 있는지, 값이 직전 주행 방위인지 0인지 확인. 비용: 로그 grep. 결과에 따라 "G3는 bearing을 보내는 것이 아니라 stale bearing을 막는 것"으로 바뀔 수 있습니다.
2. **gearhead의 bearing 휴리스틱 존재 여부** (S1): 이미 허가된 AA APK 정적 분석에서 헤드유닛 bearing을 폰 위치·나침반과 비교/대체하는 경로가 있는지 확인(클래스/메서드명만 기록). 비용: 기존 jadx 출력 검색.
3. **0x078 라벨 대조** (S22-S24): 기존 재생 자료에서 0x0116 값과 휠 속도로 계산한 곡률(v·yaw ↔ v²/R)을 비교해 이미 yaw로 확정됐는지 기록에 한 줄 남기기. 이미 포화 ±77 deg/s 실측이 있으므로 사실상 확인된 상태로 보입니다(INFERENCE). 비용: 기록 추가뿐.
4. **Google Maps를 둘째 수용 앱으로**: 다음에 **이미 계획된** 주행이 있을 때만, 네이버 대신 Google Maps로 같은 터널을 지나며 주입 LOCATION 수용 여부를 기록(새 주행 요청 아님). DHU로는 S11의 "Waze 폰 전용" 차이를 재현할 수 없습니다.
5. **G3 출발 이동 검사 임계**: S2의 500 m는 OEM 값입니다. 우리 저장 방위 복원에 "저장 지점에서 X m 이내에서 출발" 조건이 없다면 추가 검토. 비용: 설계 검토.
6. HERE SDK의 VDR 전제(1-2분 보정, 터널·주차장 시작 회피)는 폰 IMU로 yaw 영점을 보충하려는 아이디어(소유자 S25 정지 IMU 측정과 연결)에 대한 반례 자료로 보관.

## 6. 찾지 못한 것(정직하게)

- 마쯔다 1세대 + AA + 터널/주차장 DR 문제를 다룬 글(영·독·일·러·한·중·불 모두 없음).
- 네이버 지도의 해외 논의, ND DSC yaw 영점/계단 논의, 0x078 영점에 대한 어떤 공개 언급.
- 외부 센서(폰 IMU, OBD, 자이로 동글)로 헤드유닛이나 내비 앱에 yaw 영점/방위를 공급하는 공개 제품·프로젝트. u-blox ADR/UDR 모듈은 존재하나 AA 내비 앱과 연결한 사례는 없음.
- MZD-AIO/Trevelopment 쪽의 센서·방위 접근 흔적, HUR/OpenAuto의 "차 GPS 사용" 설정.
- Google 헤드유닛 지침·Apple CarPlay 센서 문서의 원문 URL(포럼 인용만 확보).
- 403으로 못 읽은 것: bolidenforum, xdaforums, autosportlabs ND CAN 매핑, miata.net ND1 PID 스레드, taycanforum Apple 문서 인용, autoevolution.

## 7. 우리 결론과 어긋나는 것

- S27·S28 "AA는 차 GPS를 쓰지 않는다": 우리 펌웨어 사실·S11·S8과 모순이며 틀렸습니다.
- S24 opendbc의 0x078 가속도 라벨: 우리 yaw 판독과 다릅니다. 펌웨어 사실과 포화 실측이 더 강한 증거이므로 결론 유지, 5-3으로 기록만 보강.
- S16 "Apple Maps는 터널 DR을 지원한다"와 S15 "Apple Maps는 터널에서 길을 잃는다"는 서로 충돌하는 사용자 주장이며 어느 쪽도 검증되지 않았습니다.
