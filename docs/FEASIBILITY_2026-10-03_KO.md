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

## B. 폰의 수용: 강하게 지지됨, 단 staleness 관문은 없다

**AA wire LOCATION의 timestamp 필드(offset 0x00, uint64)를 OEM 인코더가 0으로
박는다.** capstone 분석에서 `MakeLocation / SendLocation / OrderSendVehicleData`는
`clock_gettime / gettimeofday / time`을 전혀 호출하지 않는다 — 송신 시 현재 시각을
찍지 않는다는 뜻이고, 기존 문서의 "wire timestamp=0" 기록과 일치한다.
([archive/DESIGN_V1_KO.md](archive/DESIGN_V1_KO.md) §4, §7)

함의가 중요하다. 터널에서 폰 지도에 (멈춘) 점이 뜬다는 것은 폰이 **timestamp=0을
그대로 수용한다**는 뜻이다. 즉 "낡았다고 버리는 staleness 관문이 없다." 우리가 DR
좌표를 주입할 때 timestamp를 신선하게 맞출 필요가 없고, **좌표(lat/lon)만 올바르면
폰이 표시한다.** 레포 규칙("wire timestamp는 0으로 유지, 새 fix로 위장 금지,
좌표는 DERIVED로 계산")은 이 사실 위에 서 있다.

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

이 관문은 다음 한 번의 관찰로 가른다(코드·설치·SSH 불필요).

1. 폰에서 Android Auto(또는 네이버 지도)의 위치 권한을 거부로 설정한다.
2. **유선**으로 S25를 연결하고 네이버를 AA로 띄워 지상 도로를 평범하게 주행한다.
3. 점이 차 움직임을 따라가면 → 네이버가 차 GPS를 쓴다 → 우리 DR 주입이 의미를 가진다.
   "위치 없음"으로 죽으면 → 네이버가 차 GPS를 안 쓴다 → 접근을 바꿔야 한다.

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
| B. 폰의 수용 (timestamp=0, 터널 중 1Hz 재송신) | 정적 + VM으로 강하게 지지 |
| C-1. 네이버가 fused 위치를 읽고 표시 | 모의 위치 실측으로 확인 (폰 단독) |
| C-2. 네이버가 자체 DR로 덮어쓰기 | 리스크 크게 감소 (모의 위치를 따름) |
| C-3. AA 투영 경로에서도 동일한가 | **미확인 — 실차 유선 관찰 필요** |
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
