# Android Auto 17.9.664004: 헤드유닛 방위(bearing)를 불신·보정·대체하는가 (정적 분석, Fable, 2026-10-13)

범위: `AGENTS.md`의 Android Auto 앱(`com.google.android.projection.gearhead`) 역공학 허가에 따른 오프라인 정적 분석입니다. 자료는 `tools/aa-jadx`의 jadx 소스(17.9.664004)이며 폰·앱은 건드리지 않았고 APK 바이트·코드·덤프는 두지 않았습니다. 클래스·메서드·상수·문자열만 적습니다. **폰에 설치된 17.7.663654와의 차이는 확인하지 못했습니다.** Google Play 서비스 내부는 범위 밖입니다. 메모: `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/abf.JifJ1R/NOTES_abf.md`. 표기: FACT(클래스/메서드), DATA(계산·기록), PUBLIC DOC, INFERENCE.

## 한 줄 결론
앱에 도달하는 경로(`CarHardwareLocation`)에서 **방위는 어떤 검사·보정·억제 없이 그대로 전달**됩니다. 방위 휴리스틱(저속 제거, 변위 침로 보완, 이전 방위 유지)은 **존재하지만 폰 위치 스택 주입 경로(`rbw`)에만 있고, 이 빌드에서는 등록 코드가 빈 함수라 실행되지 않습니다.** 180도 뒤집기, 후진 기어 처리, OEM별 예외 표, 방위 관련 서버 플래그는 없습니다.

## (1) LOCATION 경로의 방위 처리
- FACT `uej.R(xrt)`: 와이어 필드 7(존재 비트 64) × 1e-6 → `CarSensorEvent(10, 디코드 시각)`의 `d[5]`, 존재 바이트 비트 32. 범위·0 검사 없음.
- FACT `kdb.d()`(태그 `CarApp.H.Har`): `new Location("Car-GPS")` 후 비트 32가 있으면 `setBearing(fArr[5])`, 없으면 호출하지 않음 → `hasBearing=false`. 속도·정확도와의 교차 조건, 연속 위치 비교, 평활화, 첫 N건 보류 모두 없음. `kda`의 상태는 전달 횟수 카운터뿐입니다.
- PUBLIC DOC(AOSP `Location.setBearing`): 0.0을 넣어도 `hasBearing()`은 true. 따라서 **순정이 정차 중 보내는 bearing 0은 네이버에 "유효한 북향 0도"로 도착**합니다(INFERENCE: 네이버가 `hasBearing`을 그대로 믿으면).
- FACT 폰 센서 비교 없음: `SensorManager`는 `mrc`(`GH.ProximityManagerImpl`, 근접 센서 8)에서만 사용. 회전벡터·나침반·자이로 융합 코드 없음.
- FACT 헤드유닛 다른 채널과의 비교 없음: `setBearing/getBearing/hasBearing/bearingTo`를 쓰는 클래스는 `qsw`, `CarSensorEvent`, `kdb`, gms `LocationResult`, classes3의 Maps SDK 카메라 클래스뿐입니다. COMPASS(gms 1)·GEAR(gms 7)·DRIVING_STATUS(gms 11) 값은 방위와 결합되지 않습니다.
- FACT 휴면 휴리스틱 `qsw` case 10(`rbw` 전용): `hasBearing && (!hasSpeed || speed ≤ 1.0 m/s)`이면 `removeBearing`; 이전 위치가 있고 간격 500 ms 이상 2000 ms 미만이면, 방위가 없을 때 이동 > 2 m이고 속도 ≥ 1 m/s이면 `prev.bearingTo(cur)`, 아니면 이전 방위 유지; 속도 ≥ 70 m/s 또는 음수면 속도 영구 비활성("Integration error: GPS speed seems invalid"), 정확도 ≤ 1 또는 ≥ 500 m면 정확도 영구 비활성, 정확도 없으면 5.67 m; 1800 ms 무신호면 "Location is stale", 정차(속도 |v| < 0.5, 주차 브레이크, 기어 101=PARK)면 마지막 위치를 7.89 m로 1 s마다 재주입. 스크래치 `Location`이 직전 객체라(`jve` case 19) **없는 필드는 직전 값을 물려받습니다**. 결과는 `tdh.c()` = LocationServices `setMockLocation`(기능명 `set_mock_location_with_callback`)로 갑니다.
- FACT 도달 불가 근거: `rbw`는 `rbe.i()`(prepareConnectionForServiceDiscovery)에서 접속마다 새로 만들어지고 `raz.l()`이 `rbw.f(raz)`를 부르지만, `f()`가 게시하는 `pqu` case 6은 **빈 `return`** 입니다. `raz.o()`(registerOrUpdateSensorListener)를 `rbw`로 부르는 곳이 없고, `raz.j()`의 해제 코드만 남아 있습니다. `setMockMode` 호출도 없습니다(INFERENCE: 옛 주입 기능의 잔재).

## (2) 아우디 "역방향" 우회책 후보
- INFERENCE(중): 위 휴면 로직이 그 우회책의 후손으로 보입니다. 아우디 공지의 원인("MMI가 GPS 진행 방향을 쓰지 않음")은 방위 없음/0에 해당하고, 휴면 로직은 **방위가 없을 때만** 변위 침로로 보완하며, **방위가 있으면 저속에서 제거할 뿐 틀린 방위는 그대로** 둡니다. 2017년 당시의 코드는 없으므로 확정할 수 없습니다.
- FACT 플래그: `acwi`/`dor` 덤프의 센서 플래그는 `CarSensorParameters__max_parked_speed_gps_sensor`=1.5, `__max_parked_speed_wheel_sensor`=0.5, `__refine_car_sensor_rate_kill_switch`=true, `__sensor_types_to_rate_throttle_at_delivery_phase`=[SENSOR_SPEED, SENSOR_LOCATION] 넷뿐. bearing/heading/compass/gps/mock/inject 이름의 플래그는 없습니다.
- FACT 제조사·모델 예외 표 없음: classes/classes2 코드에서 "Audi"/"Mazda"/quirk 문자열 없음. "backward"/"flip"/"180 deg"는 미디어 탐색·HDR 썸네일·UI 문자열뿐입니다.

## (3) 첫 연결 동작
- FACT 세션 상태: 센서 채널 `sec`는 접속마다 새로 만들어지고 마지막 값 캐시도 그 안에 있습니다. `raz.o()`는 리스너 등록 시 **그 세션의 마지막 LOCATION을 즉시 재전달**합니다. 첫 N건 무시·지연·속도 임계는 없습니다. 접속 시 gearhead 자체가 LOCATION(rate 0, `lrg` `GH.CarLocationProvider`)을 구독하지만 **속도만** 봅니다(`lrf.g`: |v| > 1.5 m/s 주행, 5 s 지나면 알 수 없음).

## (4) 이 APK에서 차량 위치를 받는 곳
1. `kdb` → Car App Library `CarHardwareLocation`(네이버 경로, 유일한 앱 전달 경로).
2. `lrq`(`GH.LocationManager`)의 `lrg`: 주행/정차 판정, 마지막 차량 `Location`을 `lra.b()`로 노출 → 메시지 위치 공유(`kfe`/`kfg`, 주소 문자열), `GH.MsgPAdapter`(`qsq`), `GH.AssistantUtils`(`lij`, 상태 스냅샷). 방위는 읽지 않습니다.
3. `rbw` → Fused 모의 위치 주입(휴면). Play 서비스 쪽 동작은 범위 밖.
4. `opw`(`GH.CarSensorReader`): 속도·야간·주행상태·기어 마지막 값을 텔레메트리로.
5. 템플릿 지도(`gqb`/`MapViewContainerImpl`)의 위치는 앱이 `sendLocation`(`hng`)으로 보낸 것이며 차량 LOCATION이 아닙니다.
6. 차에 LOCATION이 없을 때만 폰 Fused(`lrl`)를 주행 판정용으로 씁니다.

## (5) 센서 목록과 구독
- FACT 프로토콜 enum `xvo` 26종(1 LOCATION, 2 COMPASS, 3 SPEED, 7 PARKING_BRAKE, 8 GEAR, 13 DRIVING_STATUS, 14 DEAD_RECKONING, 19 ACCEL, 20 GYRO, 21 GPS_SATELLITE 등). gms 번호 매핑은 `raz`의 표(LOCATION 10, COMPASS 1, SPEED 2, GEAR 7, DRIVING_STATUS 11, DEAD_RECKONING 15, GPS_SATELLITE 17).
- FACT 접속 시 gearhead 구독: LOCATION(10, rate 0), SPEED(2, rate 3, `CAR_SPEED` 권한), DRIVING_STATUS(11), GEAR(7, `aejf`→LiveData; 소비처 미확인). Car App Library는 앱 요청 시에만 TOLLCARD→22, ENERGY_LEVEL→5, SPEED→2, MILEAGE→4, ACCELEROMETER→14, COMPASS→1, GYROSCOPE→18, CAR_LOCATION→10. DEAD_RECKONING·GPS_SATELLITE는 아무도 구독하지 않습니다.
- DATA(`validation/AA_PIPELINE_2026-09-30.md`): 마쯔다가 보낸 타입은 1, 3, 7, 10, 13, 21로 COMPASS(2)·GEAR(8)는 없습니다. 차량 데이터로 방위를 검증하는 코드는 없습니다.

## (6) 제품 함의 (기록 전용)
- 정차 중 bearing 0.0 송신: Android Auto는 그대로 통과시키고 `hasBearing=true`로 만듭니다. "방위 없음"을 원하면 **필드 7을 생략**해야 합니다(그러면 `hasBearing=false`). 어느 쪽을 네이버가 더 잘 다루는지는 네이버 쪽 문제입니다.
- 첫 연결의 실제 방위: Android Auto가 고쳐 주지 않으므로, 보내는 값이 곧 앱이 보는 값입니다. 틀린 방위도 그대로 갑니다.
- 가장 싼 차량 없는 확인: 17.7.663654 APK(소유자 폰에서 `adb pull`, 허가 범위 내)에서 `kdb`·`pqu` case 6 상당 코드가 같은지 정적 비교. 다음으로 DHU(`gps` 명령으로 bearing 0/생략)와 기존 로깅 수단 — 새 실험 요구가 아니라 선택지로만 적습니다.

## 확인하지 못한 것
Car App Library 하드웨어 허용 목록(`opj.a`)의 채움 위치(이전 기록은 기본 전체 허용), GEAR LiveData 소비처, 17.7.663654와의 차이, Play 서비스 Fused 내부, 네이버가 `hasBearing`/0도를 어떻게 다루는지.
