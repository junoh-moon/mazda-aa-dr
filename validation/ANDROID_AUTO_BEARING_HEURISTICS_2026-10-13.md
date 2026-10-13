# Android Auto 방위(bearing) 처리 정적 분석: 헤드유닛 방위를 불신·보정·대체하는가 (2026-10-13, Opus)

범위: 소유자가 `AGENTS.md`에 허용한 Android Auto 앱(`com.google.android.projection.gearhead`)만 다뤘습니다. Google Play 서비스, Google 지도, 네이버 지도 내부는 이 기록의 범위 밖입니다. 분석 대상은 17.9.664004(arm64 번들)의 jadx 소스이고, 폰에 설치된 버전은 17.7.663654이므로 **설치 버전에서는 재확인하지 않았습니다**. 정적 분석만 했고 폰·DHU·차량은 사용하지 않았습니다. APK 바이트, 디컴파일 코드, 덤프는 넣지 않았으며 클래스·메서드 이름, 상수, 문자열만 적었습니다. 근거 표기: FACT(클래스/메서드/상수/문자열), PUBLIC DOC(URL), DATA(계산 방법 명시), INFERENCE.

## 결론 요약
- **이 빌드의 Android Auto는 헤드유닛이 보낸 방위를 그대로 앱에 전달합니다.** 범위 검사, 속도 기준 제거, 평활화, 연속 위치에서 진행 방향 재계산, 폰 나침반/자이로 비교, 기어·차속·나침반 채널과의 대조, 180° 뒤집힘 감지, 제조사별 보정은 모두 **차 LOCATION → 앱 경로에 없습니다**(FACT, 아래 1).
- 방위 휴리스틱 코드는 딱 한 곳 있습니다: 레거시 폰 위치 스택 주입기(`rbw`, 실행부 `qsw` case 10). 이 코드는 속도가 없거나 ≤ 1.0 m/s이면 방위를 지우고, 방위가 **없을 때만** 이동 변위로 방위를 채웁니다. 이미 있는 방위를 고치지는 않습니다. 또한 **이 빌드에서는 센서 리스너로 등록되지 않아 실행되지 않습니다**(FACT + INFERENCE, 아래 2).
- 따라서 Android Auto는 잘못된 방위를 **바로잡아 주지 않습니다**. 방위 품질은 전적으로 제품과 소비 앱(네이버)이 책임집니다. 제품이 더 나은 방위를 보내는 것을 Android Auto가 막지도 않습니다.

## 1. 차 LOCATION → 앱 Location 경로의 방위 (질문 1)
- 와이어 메시지 클래스 `xrt`(protobuf-lite 스키마 문자열 기준): 필드 2 위도, 3 경도(둘 다 필수 int32), 4 정확도 uint32, 5 고도 int32, 6 속도 int32, **7 방위 int32(부호 있음)**. 필드 1(타임스탬프)은 스키마에 없습니다(FACT).
- 변환 `uej.R(xrt)`: 존재 비트 64(필드 7)가 있으면 `CarSensorEvent` 타입 10의 float 배열 5번에 `방위 × 1.0E-6f`를 넣고 출력 존재 비트 32를 켭니다. 그 외 분기·범위 검사는 없습니다. 이벤트 시각은 `SystemClock.elapsedRealtimeNanos()`(FACT).
- Car App Library 경로 `kdb.d`(로그 태그 `CarApp.H.Har`): `new Location("Car-GPS")`, 비트 32가 있을 때만 `setBearing(float[5])`, 없으면 `setBearing` 호출 없음 → `hasBearing()=false` 그대로 전달. 유일한 검사는 배열 길이 < 6일 때 오류 문자열 "Invalid location bearing data from CarService."(FACT). 레거시 변환 `CarSensorEvent.b(Location)`도 같은 방식입니다(FACT).
- 0°는 0°로, 방위 없음은 방위 없음으로 전달됩니다. 정지 중 방위 제거나 속도 조건이 없습니다(FACT: `kdb`, `uej.R`).
- 범위 정규화는 Android 프레임워크 `Location.setBearing`에서만 일어납니다: [0, 360)으로 정규화(PUBLIC DOC: https://developer.android.com/reference/android/location/Location#setBearing(float); 구현 세부는 기억에 의존하므로 INFERENCE). DATA(float32 재현, python/numpy로 `int → float × 1e-6f → fmod 360` 계산): 359.99999°(e6 = 359999990) 이상은 float 반올림으로 360.0이 되어 **0°로 바뀝니다**. 359.99998° 이하는 유지됩니다. −180°는 180°가 됩니다. 제품 `encode_beta_location`은 [0, 360)을 강제하고 360000000을 0으로 바꾸므로 문제없지만, 북쪽을 "정확히 0이 아닌 값"으로 보내려면 359.99 또는 0.01을 써야 합니다.
- 다른 차 채널: Mazda는 COMPASS(1), GYROSCOPE, DEAD_RECKONING을 광고하지 않습니다(`docs/FEASIBILITY_2026-10-03_KO.md`). Car App `Compass`(`kdg`, gms 타입 1)는 차 COMPASS 센서에서만 오고, LOCATION 방위로 합성하지 않습니다(FACT: `kcy`의 타입별 변환기 선택, 미지원이면 `kdc` INVALID_TYPE). GEAR(7)는 주차 판정(`lqt` "GH.FusedParkingLD", 기어 값 0/101/−1)과 `HeroFeature__disable_assistant_button_in_reverse`(기본 false)에만 쓰입니다. SPEED와 LOCATION 속도는 주행/주차 판정에만 쓰입니다(`lre`, `CarSensorParameters__max_parked_speed_gps_sensor` 1.5, `..._wheel_sensor` 0.5). 어느 것도 방위에 쓰이지 않습니다(FACT).
- 앱 전체에서 `getBearing/setBearing/hasBearing/removeBearing`을 쓰는 비-지도 코드는 `kdb`, `CarSensorEvent`, `qsw`뿐입니다. 나머지(`aaka`, `aaht` 등)는 내장 지도 SDK의 내 위치 아이콘(방위 있으면 chevron, 없으면 blue dot)과 지도 카메라입니다(FACT, ripgrep 전수).
- 문자열 "hmmRoadNetworkBearingErrorStdDeg", "bearingNoiseThresholdCentimetersPerSec", "bearingNoiseSpeedFalloffMs"는 GMM 클라이언트 파라미터 클래스 `pvo`(NavigationParameters)의 `toString`에만 있습니다. 이 APK에는 GMM 위치/내비 파이프라인이 없어 사용처가 없습니다(FACT). Google 지도 자체에 방위 노이즈·지도매칭 처리가 있다는 간접 흔적일 뿐이고 네이버와는 무관합니다(INFERENCE).

## 2. Audi "뒤로 가는 방향" 우회 (질문 2)
- 플래그 덤프 `dor`(1,009개)와 전체 플래그 정의(정의 패턴 210개)를 걸렀습니다. 위치·방위 관련 플래그는 `CarSensorParameters__max_parked_speed_gps_sensor` 1.5, `__max_parked_speed_wheel_sensor` 0.5, `__sensor_types_to_rate_throttle_at_delivery_phase`(SENSOR_SPEED, SENSOR_LOCATION), `__refine_car_sensor_rate_kill_switch` true, `Watevra__car_hardware_headunit_filter_list`(기본값 base64 "CgQIBBoA")뿐입니다. bearing/heading/reverse 보정 플래그는 없습니다(FACT). 제조사 이름 목록은 영상 코덱(`jbk`: Subaru, HARMAN)과 로고(`mze`)에만 있습니다(FACT).
- 우회 후보로 볼 수 있는 코드는 `rbw`/`qsw` case 10뿐입니다. (a) 방위가 있고 속도가 없거나 ≤ 1.0 m/s이면 `removeBearing`. (b) 방위가 없으면, 직전 위치와 0.5~2 s 간격이고 거리 > 2 m이며 속도가 < 1 m/s가 아닐 때 `bearingTo`(변위 방향)를 넣고, 아니면 직전 방위를 이어 씁니다. (c) 속도 ≥ 70 또는 음수이면 속도 영구 비활성, 정확도 ≤ 1 또는 ≥ 500이면 정확도 비활성 후 5.67 m로 대체. (d) GEAR 101(주차), 주차 브레이크, |속도| < 0.5이면 정지로 표시. **기어 후진에 따른 방위 반전은 없습니다**(FACT).
- 도달성: 레거시 `raz.j()`는 teardown 때 `rbw`를 타입 11, 2, 7, 6, 10에서 해제하지만, 등록 호출(`raz.o(type, rate, listener)`)에 `rbw`를 넘기는 코드는 없습니다. `rbw.f(raz)`가 게시하는 실행체 `pqu` case 6은 빈 동작입니다. 신 스택의 같은 자리 `irh`는 `aq` case 7에서 타입 11, 9, 2, 7, 6을 rate 3으로 등록하지만 **10은 포함하지 않고** 야간 모드만 처리합니다(FACT). 결론: 폰 위치 스택 주입과 그 방위 휴리스틱은 이 빌드에서 실행되지 않습니다(INFERENCE, 높음).
- Audi 공지(2017/2018)의 "work-around"가 무엇이었는지는 확인할 수 없습니다. 위 주입기의 (a)(b)가 "차가 방위를 안 주거나 엉터리로 줄 때 저속 방위를 버리고 변위로 채우는" 형태라서 그 시절 우회였을 가능성은 있으나(INFERENCE, 낮음), Google 지도나 Play 서비스 쪽에 있었을 수도 있습니다(범위 밖).

## 3. 첫 연결 동작 (질문 3)
- 처음 N개 무시, 속도 문턱, 워밍업, "stale" 판단은 앱 전달 경로에 없습니다(FACT: `kdb`, `kda.b`; "Location is stale." 문자열은 비활성인 `rbw` 쪽 `ojw`에만 있음).
- 센서 엔드포인트 `sec`는 서비스 디스커버리로 생성될 때 광고된 타입마다 빈 "마지막 메시지" 칸을 만들고, 메시지가 올 때마다 덮어씁니다(FACT). 세션마다 새로 생성되므로 세션 간 위치 상태는 남지 않습니다(INFERENCE).
- **리스너가 새로 등록되면 마지막 LOCATION 메시지를 즉시 다시 변환해 전달합니다**(레거시 `raz.o` → `raz.d`, 신 `iqt` → `iqt.i` → `gcg.k` → `uej.R`). 변환할 때 **현재 시각**이 찍힙니다(FACT). 따라서 네이버가 (재)등록하는 순간, 그 전에 받은 마지막 차 위치와 방위가 "방금 받은 것"으로 한 번 더 들어갑니다. 첫 연결 때는 순정 첫 메시지(저장된 335° 기록 또는 0°)가 이렇게 재사용될 수 있습니다(INFERENCE).

## 4. 위치를 받는 다른 구성요소 (질문 4)
- 앱용: Car App Library `CarHardwareLocation`(`kdb`)뿐입니다. 이전 기록(`ANDROID_AUTO_APP_STATIC_RE_2026-10-12.md`)과 같습니다.
- 내부용: `lrg`가 LOCATION을 rate 0으로 구독해 `lre`(속도로 주행/주차 판정)에 넘기고, `liu.b().b()`는 목록 행의 거리 표시(`kfe`, `RowView` 등)에 쓰입니다. 방위는 쓰지 않습니다(FACT).
- 다른 프로세스용: 센서 바인더(`raz`/`iqt`)는 다른 UID가 `android.permission.ACCESS_FINE_LOCATION`으로 타입 10을 구독할 수 있게 합니다(FACT: `raz.w`, `raz.v`). 원시 `CarSensorEvent`를 그대로 넘깁니다. 실제로 어떤 앱(예: Google 지도)이 이 경로를 쓰는지는 이 APK로는 알 수 없습니다.
- Fused 주입(`tdh.c` → FusedLocationProviderClient, `rbw.a`)은 2절과 같이 비활성입니다. mock provider(`setMockLocation`, `addTestProvider`) 코드는 없습니다(FACT). Play 서비스 내부는 범위 밖입니다.

## 5. 센서 목록과 구독 (질문 5)
- 앱이 아는 타입(텔레메트리 `zht` 15000~15026): LOCATION, COMPASS, CAR_SPEED, RPM, ODOMETER, FUEL_LEVEL, PARKING_BRAKE, GEAR, DIAGNOSTICS, NIGHT, ENVIRONMENT, HVAC, DRIVING_STATUS, ACCELEROMETER, DEAD_RECKONING, DOOR, GPS_SATELLITE, GYROSCOPE, LIGHT, PASSENGER, TIRE_PRESSURE, TOLL_CARD, VEHICLE_ENERGY_MODEL, TRAILER, EV_TRIP_SETTINGS. 변환기는 `raz.n`/`gcg.k`에 있습니다(FACT).
- AA가 스스로 구독하는 것: DRIVING_STATUS(11; `lri`, `lrr`, `xkt`, `mof`), LOCATION(10, rate 0; `lrg`), SPEED, GEAR, PARKING_BRAKE, NIGHT(`irh`, rate 3). COMPASS, GYRO, ACCEL, DEAD_RECKONING은 Car App 앱이 요청할 때만 구독합니다(FACT: `kcy`). 차 데이터로 방위를 검증하는 코드는 없습니다(FACT).

## 6. 제품에 대한 함의 (기록 전용)
- **정지 중 방위 0.0**: Android Auto는 0.0을 `hasBearing=true, 0°`로 그대로 전달합니다. 위험 여부는 네이버가 정합니다. 네이버 2차 분석(`NAVER_MAP_ROUND2_2026-10-12.md`)에 따르면 정확히 0°는 "나쁜 침로"로 처리되고 방위 없음은 더 나쁩니다. 제품 쪽 사실: 터널 프로파일 정지 상태는 유지한 차체 방위를 `hasBearing=1`, 속도 0으로 보냅니다(`core_bridge.cpp` hold_bearing). 비터널 정지에서는 `encode_beta_location`이 `hasBearing=0`을 보냅니다(FACT: `src/adapter/adapter.cpp`).
- **첫 연결에 실제 방위를 항상 보내야 하는가**: Android Auto 쪽에는 막는 것도 고쳐 주는 것도 없습니다. 보낸 방위가 그대로 도착하고, 3절 때문에 등록 시점에 한 번 더 도착합니다. G3 방위(예산 게이트 ≤ 30°)가 순정 0°/335°보다 낫다면 Android Auto 수준의 장애물은 없습니다(INFERENCE). 북쪽은 0.0 대신 359.99 또는 0.01로 보내고, 359.99999 이상은 쓰지 않아야 합니다(DATA, 1절).
- **잘못된 방위를 Android Auto가 고쳐 주는가**: 아닙니다. 방위가 있으면 손대지 않습니다. 유일한 보정 코드도 비활성이고, 방위가 없을 때만 채웁니다(FACT). 180° 뒤집힌 방위도 그대로 네이버에 갑니다. DHU 실험 7 H4(180°)에서 네이버가 경로 링크 방향을 따랐다는 관찰은 네이버 쪽 동작입니다.
- **가장 싼 무차량 확인**: (1) 설치 버전 17.7.663654 APK를 이미 확보했다면 `kdb.d`, `uej.R`, `rbw`/`qsw` case 10, `pqu` case 6, `irh.b`, `aq` case 7만 같은 방법으로 비교합니다(현재 로컬에는 17.7이 없습니다). (2) 기존 DHU 기록 재해석이나 다음 DHU 실험에서 첫 메시지 방위를 0.0, 359.99, 실제값으로 바꾸어 보는 것은 선택 사항이며 전제 조건이 아닙니다.

## 확인하지 못한 것
- 17.7.663654와 17.9의 차이.
- Audi 우회의 실제 구현 위치(Google 지도 또는 Play 서비스일 수 있음, 범위 밖).
- `rxh.f`(클라이언트 측 등록)는 jadx가 디컴파일하지 못했습니다. 다만 등록 시 캐시 전달은 서비스 측 `raz.o`와 `iqt`에서 확인했습니다.
- 센서 바인더를 구독하는 외부 앱이 무엇인지.
- Android `Location.setBearing` 정규화의 정확한 구현(기기 OS 버전에 따라 다를 수 있음).
