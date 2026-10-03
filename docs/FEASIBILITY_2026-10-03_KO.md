# 실현 가능성 평가 — 2026-10-03

이 문서는 "GPS 단절 중 센서 기반 추정 위치가 네이버 지도 화면에 반영된다"는
최종 목표의 실현 가능성을, **오프라인 정적 분석과 공개 자료 조사만으로** 재평가한
기록이다. 새 차량 방문, 휴대폰·동글 시험, 제품 변경, live ASSIST 활성화는 포함하지
않는다. 여기의 결론은 가능성의 근거일 뿐, 우리 조합(MX-5 ND2 · NA 74.00.324A ·
Galaxy S25 · 무선 동글 · 네이버 지도)의 성공을 증명하지 않는다.

분석 방법: 사용자가 제공한 펌웨어 패키지에서 rootfs를 추출하여 `blmjciaapa.so`를
capstone으로 직접 디스어셈블했고, 레포의 기존 정적 분석·VM 재현 기록과 대조했다.
Android Auto 프로토콜 쪽은 공개 역공학 자료(aasdk/openauto)와 당사자 보고를
조사했다. 원본 바이너리·디스어셈블 덤프·추출 키는 공개하지 않는다.

## 질문을 세 조각으로 나눈다

최종 목표가 성립하려면 세 가지가 모두 참이어야 한다.

- **A. 차가 폰에 위치를 보내는가?** — 송신 경로의 존재와 동작.
- **B. 폰이 그 위치를 받아서 쓰는가?** — 프레임워크 수준의 수용.
- **C. 네이버 지도가 그것을 화면에 반영하는가?** — 앱별 동작.

A와 B는 오프라인 증거가 상당히 쌓였다. C는 여전히 실차 확인이 필요한 유일한 관문이다.

## A. 차→폰 송신 경로: 확인됨 (강함)

NA 74 순정 `blmjciaapa.so`가 Android Auto에 위치 센서를 **광고하고 송신한다**.

- 광고 센서 집합 `[1,3,7,8,10,13,21]` = LOCATION, SPEED, PARKING_BRAKE, GEAR,
  NIGHT_MODE, DRIVING_STATUS, GPS_SATELLITE. 원시 GYROSCOPE/COMPASS/DEAD_RECKONING은
  광고하지 않는다. ([archive/DESIGN_V1_KO.md 참조](archive/DESIGN_V1_KO.md), §7)
- 송신 체인: `LdsGetPositionCb → worker → RequestSendPosition → MakeLocation →
  SendLocation → OrderSendVehicleData → RaceAap::SendVehicleData →
  aap_send_vehicle_data → aap_service`.
- 독립 대조: capstone 디스어셈블에서 `VehicleDataManager::MakeLocation @0xC7CB0`,
  `SendLocation @0xC78E0`, `OrderSendVehicleData @0xBD004`를 확인했고, 기존 문서의
  주소와 일치했다. 즉 기존 RE는 신뢰할 수 있다.

Android Auto 프로토콜 쪽 공개 자료도 일치한다. AA 센서 채널로 헤드유닛이 GPS·속도
등 차량 센서를 폰으로 보낸다([aasdk GPSLocationData.proto](https://github.com/f1xpl/aasdk),
[openauto](https://github.com/f1xpl/openauto)). Mazda 오너 보고에도 "GPS를 폰이 아니라
차에서 가져온다"는 사례가 있다([mazda3revolution](https://www.mazda3revolution.com/threads/android-auto-gps-problem.248138/)).

## B. 폰의 수용: 지지되나, timestamp 처리는 미확인

**AA wire LOCATION의 timestamp 필드(offset 0x00, uint64)를 OEM 인코더가 0으로
박는다는 정적 근거가 있다.** capstone 분석에서 `MakeLocation / SendLocation /
OrderSendVehicleData`는 `clock_gettime / gettimeofday / time`을 전혀 호출하지
않는다 — 이 VDM 단계에서 현재 시각을 찍지 않는다는 뜻이고, 기존 문서의 "후단
인코더가 wire timestamp=0" 기록과 일치한다.
([archive/DESIGN_V1_KO.md](archive/DESIGN_V1_KO.md) §4, §7)

**단, "폰에 최신성(staleness) 검사가 없다"는 결론은 나오지 않는다.** 기존 실행
기록은 송신 전 native 구조체를 관찰했을 뿐, 최종 직렬화 메시지와 폰 수신은 검사하지
않았다. 또 Android `Location`은 UTC 시각(`getTime`)과 경과시간
(`getElapsedRealtimeNanos`)을 구분하므로, AA 수신 과정에서 폰이 시각을 부여하거나
다른 기준으로 오래됨을 판단할 가능성이 남는다. 즉 **timestamp=0을 폰이 항상
수용한다는 주장도, 반드시 거절한다는 주장도 입증되지 않았다.** 터널에서 (멈춘) 점이
뜨는 것이 "ts=0 수용"의 증거라고 단정하지 않는다 — 그 점이 차의 좌표인지 폰의
마지막 fix인지도 아직 구분되지 않았다. 따라서 주입 시 timestamp 처리는 실측으로
확인해야 하는 열린 항목이다. ([Android Location.getElapsedRealtimeNanos](https://developer.android.com/reference/android/location/Location#getElapsedRealtimeNanos()))
레포 규칙("wire timestamp는 0으로 유지, 새 fix로 위장 금지, 좌표는 DERIVED로
계산")은 이 불확실성 위에서 보수적으로 둔 것이다.

또 하나 결정적인 점: **GPS 단절 중에도 송신 채널이 살아 있다.** OEM은 mode=0 캐시
좌표를 약 1Hz로 계속 재송신한다. 송신이 "새 GPS fix"가 아니라 **AA 쪽 1Hz 요청
루프**에 걸려 있기 때문이다. 정적 분석과 VM 재현 모두에서 확인됐다.
([validation/AA_PIPELINE_2026-09-30.md](../validation/AA_PIPELINE_2026-09-30.md),
[IMPLEMENTATION_REVIEW_KO.md](IMPLEMENTATION_REVIEW_KO.md))
따라서 우리 DR 좌표를 그 1Hz 재송신에 끼워 넣으면 폰은 움직이는 좌표를 받는다.

주의: 최신 Android Auto는 폰 GPS를 우선하고 차 GPS는 fallback으로 쓰는 경향이
보고된다. 다만 폰에서 AA(또는 지도 앱)의 위치 권한을 끄면 차 GPS를 쓰도록 강제된다는
보고가 있다([tech9autorepair](https://tech9autorepair.com/does-android-auto-use-your-cars-gps-antenna/),
[mazda3revolution](https://www.mazda3revolution.com/threads/android-auto-gps-problem.248138/)).
소스 간 미세한 충돌이 있어 우리 조합에서는 실측이 필요하다.

## C. 네이버 수용: 미확인 (유일한 실차 관문)

"차 GPS냐 폰 GPS냐"는 네이버가 정하는 것이 아니라 AA 프레임워크 수준에서
정해지므로, 네이버 APK를 뜯어도 답이 나오지 않는다(네이버는 표준 위치 API를
호출할 뿐). 위 사례들은 구글맵·Waze 기준이며 **네이버 지도 수용을 보장하지 않는다.**

이 관문을 가르는 테스트 후보는 아래와 같다.

**주의 — "위치 권한 OFF"는 깨끗한 분리법이 아니다.** Google 차량 센서 API는 차량
위치를 받는 앱에도 위치 권한을 요구한다. 권한을 끄면 폰 GPS뿐 아니라 **차량 위치
소비까지 막힐 수 있다.** 따라서 "권한 OFF 후 위치 없음 → 차 GPS 미지원"이라는
판정은 성립하지 않는다. ([Car Hardware API](https://developer.android.com/training/cars/apps/library/car-hardware-api))

**더 판별력 있는 후보(차·주행 불필요): Google 공식 DHU(Desktop Head Unit).**
노트북에서 DHU로 AA를 투영하고 S25의 네이버를 띄운 뒤, DHU가 **구별되는 가상 차량
궤적(GPS 센서)을 주입**한다. 네이버 점이 그 궤적을 따라가면 입력 출처를 깨끗이
구분한 것이다. 한계: S25·배포 네이버 앱의 DHU 호환성은 미확인이고, 이 시험도 Mazda의
timestamp 처리까지 증명하지는 않는다. ([DHU 센서](https://developer.android.com/training/cars/testing/dhu#sensors))

**실차 후보(최종 확인):** 정상 권한을 유지하고 **유선**으로 S25+네이버를 AA로 띄워
주행하며, 차 GPS와 폰 GPS가 어긋나는 구간(터널/지하)에서 어느 쪽을 따르는지 본다.
이는 Lockito 결과(폰 단독)와 달리 AA 투영 경로를 직접 본다.

유선부터 하면 무선 동글이 센서 채널을 통과시키는지의 변수를 제거하고 시작할 수 있다.

### 2026-10-03 실측: 네이버 모의 위치 테스트 (C-1 통과)

사용자가 Galaxy S25에서 Android 모의 위치(Lockito)로 가짜 위치를 주입한 결과,
**네이버 지도가 모의 위치를 그대로 따라갔다.** 이는 네이버가 표준/fused 위치
공급자의 값을 신뢰·표시한다는 뜻이다. AA가 차 GPS를 주입하는 곳이 바로 이 fused
공급자이므로, 네이버가 차에서 주입된 위치(및 우리 DR 좌표)도 표시할 것이라는 강한
근거다. 네이버가 모의 위치를 차단하지 않았다.

동시에, 네이버가 주입값을 무시하고 자체 폰 센서 DR로 덮어쓸 것이라는 우려(C-2)도
크게 줄었다 — 덮어썼다면 모의 위치를 따르지 않았을 것이다. 사용자의 "스르르륵"
주행 기억은 자체 DR이 아니라 네이버의 도로 스냅/평활화로 설명하는 편이 맞다.

한계(과장 금지): 이 테스트는 **폰 단독** 상태이며 AA 투영 경로가 아니다. AA 투영 중
네이버가 동일한 위치 소스를 쓰는지, 그리고 무선 동글이 센서 채널을 통과시키는지는
여전히 미확인이다. 최종 확인은 유선 S25 + 네이버로 실차에서 한다.

### 2026-10-03 실측: DHU 차량 위치 주입 (C-3 지지)

Google DHU로 차량 GPS를 주입하자 Android Auto로 투영된 네이버 지도가 해운대로 이동했고,
1 Hz 경로를 따라 움직이며 주입한 속도(35 km/h)까지 표시했다. 주입 중단 뒤에는 마지막
위치에서 회색·GPS 끊김 표시로 머물렀다. 폰 GPS가 강한 상황의 우선순위와 Mazda 실물
인코딩은 시험하지 않았다. 상세는
[validation/DHU_NAVER_2026-10-03.md](../validation/DHU_NAVER_2026-10-03.md).

이 결과와 실차 터널의 "정속 외삽" 관찰을 합치면, 실차에서는 Mazda의 터널 재송신
(mode=0 캐시, accuracy 표시 제거, ts=0)이 네이버에 유효 fix로 받아들여지지 않는 것으로
보인다.

**후속 DHU 실험(2차)에서 이 의심의 일부가 재현됐다.** 길안내를 켜고 마지막 좌표를 1 Hz로
반복 주입하자, **정확도 없음 + 0이 아닌 속도** 조합에서만 GPS 끊김("GPS 탐색 중", 위성-X)이
떴다. 정확도 5 m를 주면(속도 14 m/s) 유지됐고, 정확도 없이 속도 0이어도 유지됐다.
어떤 조합에서도 경로 외삽은 나타나지 않았다(시험 도로가 지도상 터널이 아니다). 각 조합은
1회씩이고, DHU `NAN`이 wire에서 필드 생략인지는 확인하지 못했다. 상세와 한계는
[validation/DHU_NAVER_EXP2_2026-10-03.md](../validation/DHU_NAVER_EXP2_2026-10-03.md).

설계 함의: [설계 문서](archive/DESIGN_V1_KO.md)는 DR 좌표 송신에서 `hasAccuracy=false`를
쓰기로 했는데, 이동하는 DR 좌표를 정확도 없이 보내면 네이버가 이를 무효 fix로 볼 수 있다.
주입 좌표는 정확도를 가진 정상 fix 형태로 시험해야 한다. 정확도 값의 허용 범위와 Mazda
wire 형식에서의 재현은 아직 모른다.

### 2026-10-03 실측: DHU 3차 (정확도 한계, 반복, 실제 터널)

세 가지가 새로 확인됐다. 상세와 한계는
[validation/DHU_NAVER_EXP3_2026-10-03.md](../validation/DHU_NAVER_EXP3_2026-10-03.md).

- **정확도 값이 fix의 유효성을 가른다.** 이동 좌표를 정확도 5·30 m로 주입하면 따랐고, 100·300 m
  또는 정확도 없음(NAN)이면 무시됐다. 허용 상한은 30~100 m 사이다(각 1회). 2차의 조합별
  관찰은 3회 반복에서도 같았다.
- **지도상 실제 터널(백양터널)에서 네이버는 입력이 없어도 마지막 속도(14 m/s)로 경로를
  외삽한다**(60초에 약 800 m). 사용자가 실차에서 본 동작과 같은 모양이다. Mazda식 정지 좌표
  재송신(정확도 없음)도 이를 막지 못했다.
- **정확도 20 m의 이동 좌표(5 m/s)는 외삽을 이기고 주입 궤적을 따랐다**(60초에 약 300 m,
  표시 속도 18). 정확도를 지운 같은 좌표는 무시됐다. 정확도를 10→100 m로 키운 시험은 30~40초 사이
  GPS 끊김이 됐지만 후반 거동이 외삽과 구별되지 않는다.

폰 자체 GPS가 정상 fix(정확도 약 15 m, 위성 8개)를 내는 동안에도 차량 위치가 무효이면 네이버는
GPS 끊김을 표시했고 폰 GPS로 넘어가지 않았다(1회 관찰).

설계 함의: 주입 LOCATION은 정확도를 가진 정상 fix여야 하며(R1의 `hasAccuracy=false`와 충돌),
검증된 정확도는 30 m 이하다. 설계의 기본 오차 한도 100 m는 DHU에서 무시되는 값이다. 주입은 보고
정확도가 임계 이하인 동안으로 제한하고 이후 네이버의 자체 외삽으로 되돌아가는 구성이 관찰과
맞는다. 정확도를 낮춰 보고하는 것은 이 결과로 정당화되지 않는다. DHU `NAN`의 wire 표현, Mazda의
실제 LOCATION 인코딩, 터널 출구, 60초 이상은 미검증이다.

## 계산(DR) 자체의 실현성

공개 평가는 차량 차속·자이로 기반 DR의 실현성을 지지한다. Eagleye는 실차 자료에서
100 m DR 구간 종점 2D 오차의 95%가 1 m 이내였다([논문](https://staff.aist.go.jp/aoki.takanose/assets/pdf/IV2021_ws.pdf),
[코드](https://github.com/MapIV/eagleye)). 단 별도 IMU·GNSS Doppler 입력을 쓰므로
CMU 입력만으로 같은 성능을 기대할 수 없다. 또 나선 램프(다층 주차장)는 평면 DR의
체계적 방위 오차로 터널보다 어렵다([u-blox 회고](https://www.u-blox.com/en/blogs/innovation/boosting-gnss-sensor-fusion)).
평면 터널 성공을 주차장 성공으로 확대하지 않는다.

## 한국 환경의 함의

순정 내비(NNG)의 mode=3 DR도 AA 경로로 갈 수 있으나, SD/내비 없는 구성의 위치
공급자는 LDS이고 터널에서 LDS는 멈춘 좌표를 준다. 한국에서는 순정 내비를 쓸 수 없으므로
**순정 DR 재사용은 이 환경에서 기대할 수 없고, LDS 경로에 우리 DR을 주입하는 접근이
맞다.** ([archive/DESIGN_V1_KO.md](archive/DESIGN_V1_KO.md) §4, [native_dr_followup.md](native_dr_followup.md))

## 종합 판정과 미확인 항목

| 고리 | 근거 강도 |
| --- | --- |
| A. 차→폰 위치 송신 존재·사용 | 바이너리 + 프로토콜 자료로 확인 |
| B. 폰이 차 위치를 수용 (터널 중 1Hz 재송신 채널 존재) | 지지됨 |
| B'. 폰의 timestamp=0 최신성 처리 | **미확인 (ts=0은 정적 근거, 폰 해석은 불명)** |
| C-1. 네이버가 fused 위치를 읽고 표시 | 모의 위치 실측으로 확인 (폰 단독) |
| C-2. 네이버가 자체 DR로 덮어쓰기 | 리스크 크게 감소 (모의 위치를 따름) |
| C-3. AA 투영 경로에서도 동일한가 | DHU 실측으로 지지 (위치·속도 모두 사용) |
| C-4. 정확도 없는 재송신(Mazda 방식)을 네이버가 무효로 보나 | DHU 2·3차로 재현(조합별 3회 일치, 실제 터널에서 외삽). 순정은 정확도를 wire에서 **생략**함을 펌웨어로 확인([기록](../validation/AA_LOCATION_WIRE_2026-10-03.md)). DHU `NAN`이 생략인지는 문서 문구에 따른 추정 |
| C-5. 정확도가 있는 이동 좌표가 터널에서 외삽을 이기나 | DHU 3차 C3로 지지(정확도 20 m). 허용 상한은 30~100 m 사이, 각 1회 |
| 무선 동글의 센서 채널 통과 | **미확인** |
| 터널 DR 정확도(평면) | 공개 평가가 지지, 입력 조건 상이 |
| 나선 주차장 DR | **미확인, 별도 난제** |

종합하면, "차가 보내고 폰이 받는다"(A·B)는 오프라인 증거로 거의 긍정으로 좁혀졌다.
**남은 핵심 미지수는 네이버의 실제 수용 하나이며, 이는 위 유선 관찰 한 번으로 가른다.**
계속할 근거는 릴리스 수가 아니라 이 관문의 증거다.

이 문서는 오프라인 평가이며 다음을 주장하지 않는다: live ASSIST 활성화, v1.0 완료,
실차·휴대폰 검증, 네이버 수용. "스르르륵"이라는 사용자의 흐릿한 주행 기억은 네이버의
도로 스냅으로도 설명될 수 있어 결론 근거로 쓰지 않는다. 관련 근거는
[STATUS_KO.md](STATUS_KO.md), [V1_READINESS_KO.md](V1_READINESS_KO.md),
[archive/DESIGN_V1_KO.md](archive/DESIGN_V1_KO.md)를 따른다.
