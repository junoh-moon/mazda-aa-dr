# 카카오맵 5.17.8 정적 역공학: 안드로이드 오토 위치 경로, 유효성 판정, 터널 처리 (2026-10-13)

소유자가 AGENTS.md에 허용한 범위(2026-10-13, 소유자가 APK를 제공한 경우의 카카오맵)입니다. 입력은 소유자가 제공한 APKMirror 단일 APK `net.daum.android.map_5.17.8-8430200`(versionName 5.17.8, versionCode 8430200, min API 28, arm64-v8a/armeabi-v7a, dex 7개, SHA-256 `519dfde8…`)입니다. jadx 1.5.6(-Xmx1500m, --no-res, nice 19)으로 키워드가 몰린 classes5(내비 SDK와 위치 라이브러리), classes4(`com.kakao.map.navi`), classes.dex(androidx car app)를 하나씩 디컴파일했고, classes2/3/6/7은 원시 dex 문자열만 보았습니다. 매니페스트는 바이너리 XML 문자열 풀을 직접 디코드했습니다. 내비 엔진은 네이티브가 아니라 Kotlin(`com.kakaomobility.knsdk`, KNSDK 23102601, 패키지 난독화, `compiled from` 파일명은 남아 있음)이고 네이티브 라이브러리는 지도 렌더러·Realm·Sentry뿐입니다. 서버·계정과 상호작용하지 않았고 폰·차 실행은 없습니다. APK, 디컴파일 코드, 덤프는 저장소에 두지 않습니다(작업 파일은 git 제외 `tools/kakao-*`). 서버 원격설정 값은 조회하지 않았으므로 아래 상수는 **앱 내장 기본값**입니다.

## 1. 위치 출처와 안드로이드 오토 (FACT)
- **카카오맵은 안드로이드 오토 앱으로 등록되지 않습니다.** 매니페스트에 `androidx.car.app.CarAppService` 액션을 가진 서비스와 `com.google.android.gms.car.application` 메타데이터가 없습니다. 권한(`androidx.car.app.NAVIGATION_TEMPLATES`, `ACCESS_SURFACE`), `CarAppPermissionActivity`, `res/xml/automotive_app_desc.xml`(`uses template`), `car-app-api.level`=6, Car App Library 1.3.0-rc01은 들어 있지만, R8이 `androidx.car.app.z`로 이름을 바꾼 `CarAppService`를 상속하는 클래스가 디컴파일한 세 dex 어디에도 없고 그 타입 문자열은 classes.dex(라이브러리 정의)에만 있습니다.
- KNSDK의 차 화면 코드(`KNIntroSession` → `KNRequestPermissionScreen`, `KNMainSession`, `KNSurfaceRenderer`)는 포함되어 있으나 이를 만드는 `onCreateSession` 구현이 없어 도달 불가입니다(INFERENCE, 높음: 카카오맵은 AA 화면에 나타나지 않으며, AA 내비는 별도 앱인 카카오내비 몫).
- `CarHardwareManager`, `CarSensors.addCarHardwareLocationListener`, `CarHardwareLocation`은 androidx 라이브러리 클래스에만 있고 앱 호출자가 없습니다. 문자열 "Car-GPS"도 없습니다.
- KNSDK의 차 연결 플래그(`KNSDK.handleCarInterfaceConnected` → `KNGPSManager.setCarConnected` → 위치 라이브러리 `isCarConnected`)는 호출자가 없고, 쓰이는 곳도 서버 텔레메트리 필드(`carConnected`, "IVI"/"SmartPhone")와 차로 안내 표시뿐입니다. `CarConnectionDataSource.checkCarConnectionSystem`(UiModeManager 차 모드 = 3)은 `ValidCheckerVehicle.setCarConnectionDatSource`에 넘겨지지만 그 메서드가 비어 있습니다.
- 실제 위치 입력(`KNGPSManager`): 서버 설정 `positioning_opt`(기본 0)가 0 또는 3이면 카카오모빌리티 위치 라이브러리(`KMLocationManager`, 필터 `VEHICLE`, 출력 500 ms)를, 그 밖이면 `KNLegacyLocationManagerN`(LocationManager)과 `KNFusedLocationManager`(Play 서비스 Fused)를 씁니다. 라이브러리 `VehicleRepository`(알고리즘 1.2.0)는 폰 "gps"(LocationManager), "fused", 서버 LTE 셀 측위 "fin"(`FinRepository`, 원격 기능 플래그), 실내 측위를 합칩니다. 소스 우선순위 gps 0 > fused 1 > fin 2, 낮은 소스는 마지막 유효 측정 뒤 2000 ms가 지나야 받습니다(`ValidCheckerVehicle.b`).
- 결론(INFERENCE, 높음): 카카오맵은 AA 연결 여부와 무관하게 **폰 자체 GNSS/Fused(+LTE 측위)만** 씁니다. AA 앱의 Fused 주입 경로는 17.9에서 도달 불가로 분석됐으므로(`ANDROID_AUTO_APP_STATIC_RE_2026-10-12.md`) 헤드유닛 LOCATION은 카카오맵에 도달하지 않습니다. Play 서비스 Fused 내부는 범위 밖입니다.

## 2. 유효성 판정 (FACT, 앱 내장 기본값, 서버로 바뀔 수 있음)
- 모의 위치: `KNGPSManager.onLocationChanged`/`onRawDataChanged`가 `isMock`(API 31+) 또는 `isFromMockProvider`이면 버립니다(샌드박스 서버일 때만 허용).
- 시각: `KNGPSData.timestamp` = `Location.getTime()`. 라이브러리는 측정 도착 시스템 시각(`measurementUpdateSystemTime`)과 측정 시각 차를 비교해 (측정 간격 − 도착 간격) > 2000 ms 또는 도착 간격 < 100 ms면 "invalid time difference"로 거부, |측정 시각 − 현재| > 600000 ms면 현재 시각으로 교체(GPS 롤오버), 역행 < −30000 ms 거부.
- 정확도(라이브러리 `ValidCheckerVehicle.c`): gps는 > 35+30 = 65 m 거부, 속도 < 0 거부(방위 정확도가 없으면 방위를 직전 유효 방위로 덮어씀); fused는 속도 < 0.833 m/s이면서 > 14 m 거부, > 35 m 거부, 속도 < 0 또는 방위 < 0 거부(INFERENCE: 라이브러리가 '없음'을 음수로 표현한다고 가정, 미확인), fin 직후의 fused는 `speedAccuracy` = 0이면 거부. 레거시 경로: 정확도 ≤ max(`positioning_hdop`, 40) 그리고 정확도 존재.
- 점프: 2분 이내 두 측정에서 |보고 속도 − 거리/시간| > 13.89 m/s + 50 m/Δt면 거부. 같은 좌표가 반복되고 `speedAccuracy` = 0인 gps는 "abnormal legacy gps"로 거부.
- 위성: `GnssStatusDataSource`가 폰 `GnssStatus`를 받아 `OpenSkyChecker`의 openSkyAccuracy(0~1)를 만들지만 소비처는 로그·텔레메트리뿐입니다(INFERENCE: 유효성 조건 아님). 위성 사용 수를 요구하는 분기는 찾지 못했습니다.
- 출력(`AlgorithmVehicle.run`, 500 ms 틱): 등속 칼만 예측은 마지막 측정 뒤 2000 ms(`maxEstimateTime`)까지, 2000 ms(`maxValidTimeGapMs`)를 넘으면 valid=false, 정확도 최소 50 m. `KNGPSData.isOrigin` = 추정 지연 < 500 ms. 신뢰 플래그: 속도 > 0이 10회 연속이면 speedTrust, 방위 ≥ 0이 10회 연속이면 angleTrust, 둘 다면 posTrust(무효 1회로 리셋).

## 3. GPS 상실과 터널 (FACT)
- 감지: 위 2 s 규칙으로 무효가 됩니다. 레거시 경로는 100 ms 틱 10회(1 s) 동안 새 위치가 없으면 마지막 값의 무효 사본(시각 +1 s)을 냅니다.
- 경로 진행(`KNMapMatching`): 무효 위치일 때 현재·인접(50 m 이내) 링크가 터널(시설 2) 또는 지하차도(4)이면 **경로를 따라 max(마지막 정합 속도의 80%, 링크 속도, 5) km/h로 계속 전진**(`KNMapMatchingSt_Simulated`, 기간 제한 없음, 로그 "시뮬레이션 매칭(터널)"); 그 밖에서는 80% 속도로 2회까지만 전진 뒤 정지. 폰 관성센서(가속도·자이로·기압)는 `ContextEventV`의 급가속·급회전 등 운전 이벤트에만 쓰이고 위치 추정에는 없습니다. 차 속도는 쓰지 않습니다.
- 화면: 무효가 20 s 이어지면 정보 배너 `GPSFail`(`info_gps_fail_*`, 아이콘 `img_toast_gps_off`)인데 **현재 링크가 터널이면 띄우지 않습니다**(`KNComponentInfoView`). 속도 숫자는 무효일 때 회색(`KNComponentSpeedView`). 첫 유효 위치에서 음성 `KNVoiceCode_GPSConnected` 1회.
- 재획득: 유효 위치가 오면 후보 탐색 반경 38 m→500 m, 점수 ≥ 998074 Trusted, ≥ 994629 Doubtful. 경로에서 40 m 넘게 벗어나면 OutOfRoute.

## 4. 방위 (FACT)
방위는 정수 도(`(int) getBearing()`)로 잘리고 0은 유효(≥ 0), 없음은 −1(angleTrust 리셋). 정합되면 링크 방향으로 대체될 수 있습니다. 4 km/h(1.11 m/s) 미만에서는 마지막 방위를 유지합니다. 첫 연결 특별 처리는 찾지 못했습니다.

## 5. 이탈·재탐색 (FACT)
`KNGuidance`: 유효하고 원본(isOrigin)인 위치가 OutOfRoute이고 목적지에서 50 m 넘게 떨어져 있으면 즉시 이탈 음성("DEVIATION")과 재탐색. 무효 위치로는 재탐색하지 않습니다. 후보 판정은 거리 37~38 m와 각도(일반 30°/40 m, 터널·링크 경계 근처 45°/150 m)를 씁니다. 보고 정확도 값으로 가중하는 분기는 찾지 못했습니다. `KNRouteErrorDetector`는 서버 오류 보고용입니다.

## 6. 네이버와 비교 (게이트)
| 항목 | 카카오맵 5.17.8 | 네이버 6.8.0.5 |
|---|---|---|
| AA 차 위치 | 사용 안 함(AA 앱 아님, 폰만) | `CarHardwareLocation` 병렬 사용 |
| 정확도 | gps ≤ 65 m, fused ≤ 35 m(저속 ≤ 14 m), 레거시 ≤ 40 m | < 30 유효, ≥ 50 나쁨 |
| 속도·방위 | gps는 속도 음수만 거부, fused는 속도·방위 음수 거부 | 없으면 무효 |
| 시각 | 도착 시각 대비 측정 시각 2 s 어긋남·100 ms 미만 간격 거부, 2 s 무측정 → 무효 | 수신 시각 같으면 무효 |
| 위성 | 로그용 openSky만 | 사용 안 함 |
| 모의 위치 | 거부 | 이름표만 |
| GPS 상실 표시 | 20 s 무효 → 배너(터널 링크 제외), 속도 회색 | 10틱 무효 → 회색 아이콘 |
| 터널 진행 | 경로 따라 80%/링크 속도 무기한 | 마지막 속도로 가상주행 |

## 7. 제품 함의
- (FACT+INFERENCE) 25 m·40 m·없음, 횡오차, 위성 S5는 카카오맵 동작에 영향이 없습니다. 카카오맵 사용자에게 G1~G3를 제공하는 경로는 헤드유닛 LOCATION 설계로는 없습니다.
- 네이버용 설계(정확도 상한 25, 속도·방위 항상 포함, 0.0 회피)는 카카오맵에 해가 없습니다.
- AA에서 카카오 계열 내비를 쓰려면 별도 앱(카카오내비)이 필요하며 이 기록은 그 앱을 다루지 않습니다(허용 범위 밖).

## 버전과 미해결
분석 대상은 5.17.8(versionCode 8430200, KNSDK 23102601)입니다. 소유자 폰 설치 버전은 확인하지 않았습니다(소유자 작업 가정: 동일 동작). 서버 원격설정(`positioning_opt`, `positioning_hdop`, 위치 라이브러리 설정, `fin` 기능)은 조회하지 않았습니다. 이후 버전에서 CarAppService가 추가될 수 있으므로 설치 버전의 매니페스트 확인이 필요합니다. 정합 점수 계산 세부, 링크 속도 필드의 의미, 리소스 문자열 ID 대응은 미해결입니다.
