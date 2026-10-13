# 해외 커뮤니티 조사: 같은 문제를 겪은 사람과 답이 있는가 (OPUS, 2026-10-13)

범위: 공개 웹(WebSearch 약 30회, WebFetch, `gh`로 공개 GitHub 이슈·코드 조회)만 읽었습니다. 가입·로그인·게시·연락은 하지 않았고, 403/로그인/tollbit(봇 과금 관문) 페이지는 우회하지 않았습니다. 펌웨어·바이너리·설치 파일은 내려받지 않았습니다(공개 PDF 공지문, 공개 소스 저장소 `git clone`, 이미지 1장만 받았고 아무것도 실행하지 않았습니다). 소유자·차량 식별 정보는 검색어에 넣지 않았습니다. 질의 기록과 막힌 페이지 목록: `$CLAUDE_JOB_DIR/tmp/cro.uvzAnR/NOTES_cro.md`. 세션 검색 예산(200회)이 마지막 질의에서 소진되어 독일어·프랑스어·중국어 질의는 하지 못했습니다.

표기: FACT = 출처가 글자 그대로 말하는 것, INFERENCE = 제 해석, 미확인 = 확인하지 못한 것. 어떤 출처도 74.00.324A 또는 이 차에서 검증된 것으로 취급하지 않습니다.

## 1. 출처 표 (모두 2026-10-13 조회)

| # | 출처 | 종류 | 게시일 | 신뢰도 |
| --- | --- | --- | --- | --- |
| S1 | Audi PSS 2047294/1 "Android Auto: Google Maps Navigation shows backwards direction of travel", NHTSA 사본 https://static.nhtsa.gov/odi/tsbs/2017/MC-10128232-9999.pdf | 제조사 기술 공지 | 2017-03-23 | 1차(제조사) |
| S2 | Audi TPI 2050566/1 같은 제목, MIB2 High Scale, https://static.nhtsa.gov/odi/tsbs/2018/MC-10133316-9999.pdf | 제조사 기술 공지 | 2018-02-27 | 1차(제조사) |
| S3 | gartnera/headunit 이슈 #110 "Google Maps shows car running backwards" https://github.com/gartnera/headunit/issues/110 | 마쯔다 AA 개조 개발자 토론 | 2017-12 | 개발자 실험 기록(구 펌웨어) |
| S4 | gartnera/headunit #150, #11 https://github.com/gartnera/headunit/issues/150 , /issues/11 | 사용자 보고 | 2019-04, 2016-08~2017-10 | 포럼 일화 |
| S5 | Trevelopment/MZD_Speedometer `speedometer.js` `updateGPSHeading` https://github.com/Trevelopment/MZD_Speedometer | 공개 소스(마쯔다 CMU 앱) | 미상(저장소) | 개발자 주석 |
| S6 | manio/aa-proxy-rs (커밋 b99fed2, 2026-10-12) `src/mitm.rs`, README https://github.com/manio/aa-proxy-rs | 공개 소스(AA 중간자 동글) | 2026-10 | 1차 코드(동작 주장은 주석) |
| S7 | AAWireless 지원 글 "What about the remove tap restriction?" https://support.aawireless.io/support/solutions/articles/80000500873 | 제조사 FAQ | 미상 | 로그인 필요, **검색 요약만** |
| S8 | matsumo0922/OneNavi 이슈 #91, #105 https://github.com/matsumo0922/OneNavi/issues/105 | 일본 개인 내비 앱 개발 기록 | 2026-06 | 개발자 조사(문서 기반) |
| S9 | ABRP 요청 "Select to use phone position, not car, when using Android Auto" https://abrp.featurebase.app/p/select-to-use-phone-position-not-car-when-using-android-2 | 앱 기능 요청 | 2024-07-18 | 사용자 일화 |
| S10 | ABRP 요청 "Dead reckoning navigation when driving in tunnels" https://abrp.featurebase.app/p/dead-reckoning-navigation-when-driving-in-tunnels-2 | 앱 기능 요청(상태 Completed) | 미상 | 사용자 일화 |
| S11 | WWDC 2016 Session 722 녹취 https://asciiwwdc.com/2016/sessions/722 | Apple 공개 발표 | 2016 | 1차(Apple) |
| S12 | shihabal3amri/DiPlay `docs/BYD_NAVIGATION.md` https://github.com/shihabal3amri/DiPlay/blob/f2d06951b4e8114dbb62f551c12a32a845a3042f/docs/BYD_NAVIGATION.md ; xorob0/rigPlay #41 | 공개 소스(CarPlay 수신기) | 2026 | 개발자 기록 |
| S13 | saggdi 블로그 "ロードスターにAndroidAutoを載せた話" https://saggdi.blogspot.com/2020/01/mx5AndroidAuto.html | 일본 ND 소유자 블로그 | 2020-01 | 일화 |
| S14 | みんカラ e-clover 정비수첩 "Android Auto利用時にトンネル内で自車位置が止まる時の対応方法（CX-8）" https://minkara.carview.co.jp/userid/1139133/car/3383504/7417106/note.aspx | 일본 소유자 기록 | 작업일 2023-07-09 | 일화, **해결법 본문 없음** |
| S15 | commaai/opendbc `mazda_2017.dbc`; timurrrr/RaceChronoDiyBleDevice `can_db/mazda_mx5_nd.md`; AiM MX5-ND 채널 PDF | 공개 CAN 자료 | 2021~2022 등 | 2차(역공학) |
| S16 | US6314329B1 (Visteon, 1998 출원) https://patents.google.com/patent/US6314329B1/en | 특허 | 2001 공개 | 1차(일반 기술) |
| S17 | organicmaps `CarSensorsManager.java` | 공개 소스 | 2026-09 | 1차 코드 |
| S18 | 9to5google "Android Auto still isn't playing nice with Google Maps GPS tracking" https://9to5google.com/2023/03/13/android-auto-gps/ | 기술 언론 | 2023-03-13 | 2차 |

## 2. 출처별 내용과 우리 문제와의 관계

**S1/S2 (Audi 공지, 가장 강한 1차 근거).** FACT: S1은 "Android Auto navigation uses the GPS data and other vehicle information provided by the MMI and does not use the phone's location based system"라고 쓰고, 헤드유닛이 위치는 보내지만 "gyro (compass) information, gear selected (reverse or drive), and wheel speed"를 보내지 않아 구글 지도가 진행 방향을 반대로 표시했다고 설명합니다. S2는 원인을 "the MMI not utilizing the direction of travel from the GPS antenna"로 적고 "Google provided a work-around in their Android Auto software"라고 씁니다. 관계: **G3(첫 연결 방위), AA LOCATION 동작.** 신규성: 새로운 내용입니다. 2017년에는 AA/구글 지도가 차의 방위·기어·바퀴 속도를 방향 판단에 썼고, Google이 AA 쪽에서 우회책을 넣었다는 공식 기록입니다. INFERENCE: 우리 gearhead 17.x 분석(“DR/GYRO는 AA가 요청하지 않음”, “fused 주입 경로 도달 불가”)과 시기가 다른 것이지 모순은 아닙니다. 그 사이 AA의 설계가 바뀌었다는 방증입니다. 네이버에는 직접 적용되지 않습니다.

**S3/S4/S5 (마쯔다 커뮤니티 AA 개조, 2016~2019).** FACT: 펌웨어 55~56대에서 Nav SD를 꽂으면 LDS 방위가 180° 뒤집혀 구글 지도 화살표가 거꾸로 갔고(#110), 개발자 FREESCALE009는 방위를 180° 돌려 보내도, 아예 빼도 당시 Maps/Waze가 정상이었다고 보고했습니다(“both waze and maps ignore it”). lmagder는 예전 코드가 타임스탬프를 500 s마다 갱신해 "practically we were always using the phone's GPS"였다고 썼습니다. Trevelopment 속도계 코드는 Nav SD가 있으면 방위에 180°를 더하고, 주석에 "Mazda Infotainment System GPS heading is S = 0/360 degrees even though docs say otherwise, YMMV"라고 적었습니다. #11에서 Trevelopment는 AA가 GPS 채널을 쓰는 동안 CMU 쪽 GPS를 동시에 처리하지 못한다고 썼습니다. 관계: G3, 순정 LOCATION 방위의 규약. 신규성: 우리가 순정 jciAAPA가 아닌 옛 개조 headunit의 경험이라 직접 적용은 안 되지만, **"LDS 방위 규약이 Nav SD/NNG 동작 상태에 따라 달라질 수 있다"는 경고는 새롭습니다.** 미확인: 74.00.324A에서 같은 현상이 있는지.

**S6/S7 (aa-proxy-rs, AAWireless).** FACT: aa-proxy-rs는 HU와 폰 사이에서 서비스 디스커버리의 센서 목록과 `location_characterization`, 센서 배치(LOCATION 포함)를 고쳐 쓰고, REST로 주행거리·타이어 압력·EV 배터리를 주입합니다. "Remove tap restriction"은 SDR에서 SENSOR_SPEED를 지우는 방식입니다. AAWireless FAQ 검색 요약은 이 옵션이 "may cause the GPS location to lag behind or be inaccurate"라고 하고, 사용자들은 차 GPS 대신 폰 GPS를 쓰게 되는 것 같다고 말합니다(본문 로그인 필요, 미확인). 관계: AA LOCATION 동작, 대안 주입 지점. 신규성: aa-proxy-rs 자체와 "특성값 교차검증" 주석은 이미 알고 반박한 내용입니다(`ANDROID_AUTO_APP_STATIC_RE_2026-10-12.md` 14행). **새로운 것은 "속도 센서를 광고에서 빼면 위치 품질이 나빠진다"는 사용자 보고**입니다. 우리 gearhead 분석으로는 차 LOCATION의 속도가 주행/정차 판정에만 쓰이므로 이 보고는 구글 지도의 폰 위치 쪽 현상일 가능성이 높습니다(INFERENCE). 동글을 쓰는 경우 동글이 LOCATION을 바꾸지 않는다는 전제도 함께 확인해야 합니다.

**S8/S9/S17 (앱 개발자 쪽).** FACT: OneNavi 개발자는 공식 문서만으로 `CarHardwareLocation`이 "raw location data"이고 융합 측위가 아니라고 결론짓고, 터널에서는 경로를 따라 직전 속도로 진행하는 DR(“直前速度固定”)을 설계했습니다. 남은 위험으로 "GPS 断後の停車"를 앱 단독으로는 검출할 수 없다고 적었습니다. ABRP 요청자는 구글 지도는 폰 위치를 쓰는데 ABRP는 AA 차 위치를 쓴다고 보고했습니다. Organic Maps는 차 위치 리스너 코드를 갖고 있지만 `mIsCarLocationUsed = false`(TODO)로 꺼 두었습니다. 관계: 네이버 가상 주행(속도 고정)의 정체, AA LOCATION 경로. 신규성: 우리 결론("네이버 속도 고정 = 앱의 가상 주행", "차 위치는 `CarHardwareLocation`으로만 전달")과 **독립적으로 일치**합니다. 새 사실은 아닙니다.

**S10/S11/S12 (CarPlay·다른 앱).** FACT: Apple은 "Speed information is used by iPhone for dead reckoning"이라고 발표했고(S11), CarPlay에서는 차가 `$PASCD`(속도·기어)를 보내며 자이로·가속도(`$PAGCD`, `$PAACD`)는 형식이 비공개라 오픈 구현이 보내지 못합니다(S12). DiPlay 저자도 "Whether the iPhone uses the speed in a tunnel is still to be tested"라고 적었습니다. 관계: 터널 연속성. 신규성: **AA에는 CarPlay의 PASCD에 해당하는 "폰이 차 속도로 DR"하는 경로가 현재 앱 수준에서 없다**는 대비가 뚜렷해졌습니다. 우리에게 쓸 수 있는 수단은 아닙니다.

**S13/S14 (일본 마쯔다 소유자).** FACT: ND 로드스터 소유자는 2020년에 AA가 차 GPS를 써서 터널에서도 "走行距離によって位置予測をするのでロストすることがない"고, 오히려 순정(마쯔코네) 내비가 터널에서 위치를 잃는다고 썼습니다. CX-8(2023년식, 사진의 넓은 화면으로 보아 2세대 마쯔다 커넥트로 추정되며 우리 1세대와 다를 수 있음, 미확인) 소유자는 Pixel 7에서 터널과 실내 주차장에서 자차 아이콘이 멈춘다고 쓰고 Google에서 해결법을 받았다고 했지만, 본문에 해결 단계가 없었습니다(사진 1장만 확인, 내용 없음). 관계: 터널 연속성, 지하주차장. 신규성: 같은 증상을 겪은 마쯔다 사용자가 해외에도 있다는 것은 새롭지만, **해결 방법은 얻지 못했습니다.** S13의 "터널에서 잃지 않음"이 차 쪽 DR인지 구글 지도 자체 외삽인지는 글로 판별할 수 없습니다.

**S15/S16 (CAN, yaw 영점).** FACT: opendbc `mazda_2017.dbc`(다른 플랫폼)는 ID 120(0x078)을 BRAKE로 두고 가속도 X/Y만 정의하며 yaw는 없습니다. RaceChrono ND 문서도 ID 120을 브레이크로 쓰고 yaw rate를 "would be nice to find"로 남겼습니다. AiM ND 채널 목록에도 yaw가 없습니다. Visteon 특허는 정지·브레이크·횡가속도 0 조건에서 영점을 학습하고 주행 중 갱신하지 않는 일반 방식을 설명합니다("±26 counts = ±1°/s" 예시). 관계: yaw 영점. 신규성: **없음.** 우리 0x078 해석(12비트 yaw, 계단 영점, 출발 시 재설정)을 확인하거나 반박하는 공개 자료를 찾지 못했습니다. 특허의 26카운트/°/s와 우리 CMU 계약의 약 26.5카운트/°/s가 비슷하지만 우연일 수 있습니다(INFERENCE, 근거 약함).

**S18.** FACT: 2023년 초 구글 지도가 HU GPS와 폰 GPS가 어긋날 때 오작동한다는 보고와, AA 앱의 위치 권한을 끄는 우회책이 소개되었습니다. 관계: AA LOCATION. 신규성: 낮음(구글 지도 한정).

## 3. 우리 결론과 어긋나는 것

- 정면으로 모순되는 1차 근거는 찾지 못했습니다.
- 약한 긴장 1: S1/S2는 AA가 차의 자이로·기어·바퀴 속도를 쓴다고 하지만, 2017년 기록입니다. 현재 gearhead 분석과 시기가 다릅니다.
- 약한 긴장 2: S13은 AA+차 GPS가 터널에서 위치를 잃지 않았다고 하고, S14는 같은 마쯔다 계열에서 멈춘다고 합니다. 앱·폰·시기가 달라 판별할 수 없습니다.
- S3의 실험("방위를 뒤집거나 빼도 Maps/Waze가 무시")은 2017년 구글 지도에 대한 것이며, 네이버는 방위를 읽는다는 우리 앱 분석과 충돌하지 않습니다.

## 4. 해볼 만한 단서 (가장 싼 확인 단계)

1. **순정 LOCATION 방위 규약 점검**(S3/S5). 이미 있는 주행 로그에서 순정이 보낸 bearing과 연속 좌표로 계산한 침로를 정지·저속 제외 구간에서 비교합니다(180° 또는 남=0 규약 흔적 여부). 비용: 오프라인 스크립트 하나. G3 저장 방위가 순정 방위를 쓰는 경로가 있으면 특히 중요합니다.
2. **동글 경로 확인**(S6/S7). 소유자 동글의 종류와 "tap restriction" 계열 옵션 상태를 이미 있는 기록에서 확인합니다. 차 LOCATION이 폰에 도달한다는 로그 증거(네이버의 차/폰 선택)가 이미 있다면 추가 작업은 필요 없습니다. 차량 방문 요청 사항은 아닙니다.
3. **aa-proxy-rs를 시험 도구로 활용**(S6). DHU 연결을 지원하는 MITM 프록시이므로, LIVI 포크 대신 DHU와 폰 사이에서 마쯔다식 센서 목록·특성값·위성 0·순정식 재송신을 흉내 내는 데 쓸 수 있는지 소스를 읽어 판단합니다(호스트 전용, 차 불필요).
4. **S14 해결법**. 본문이 이미지 안에만 있거나 없어서 확보하지 못했습니다. 같은 작성자의 다른 공개 글을 읽는 것 외에 할 일은 없습니다(가치 낮음).

## 5. 답을 찾지 못한 것

- 마쯔다 커넥트 1세대 + AA + 터널/지하주차장에서 **이 문제를 해결한 공개 사례는 없었습니다.** 해외 사용자는 대부분 폰 GPS 사용으로 우회하거나 체념합니다.
- 0x078 yaw 영점의 계단 동작이나 ND DSC 영점 초기화를 다룬 공개 토론은 찾지 못했습니다.
- 폰 IMU, 애프터마켓 자이로, OBD로 더 나은 방위를 **AA 헤드유닛 LOCATION이나 네이버에 공급하는 공개 제품**은 찾지 못했습니다(u-blox ADR/UDR 모듈은 존재하지만 폰 내비 연동 제품은 확인 못 함).
- NNG 순정 DR에 관한 커뮤니티 설명은 "속도와 조향 입력을 쓴다" 수준의 판매 문구뿐이었습니다.
- 막힌 출처: bolidenforum, xdaforums, autosportlabs, 5thgenrams, ranger5g, taycanforum, Samsung 커뮤니티(403), kiaevforums·718forum(tollbit), AAWireless 지원(로그인). 무선 AA + One UI 8에서 길안내 화면이 멈춘다는 보고(us.community.samsung.com, 검색 요약만)는 S25 사용 환경과 관련 있을 수 있으나 본문을 확인하지 못했습니다.
