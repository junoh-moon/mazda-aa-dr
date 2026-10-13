# 카카오내비 4.51.1 정적 역공학: 안드로이드 오토 위치 경로, 유효성 판정, 터널 처리 (2026-10-13)

소유자가 AGENTS.md에 허용한 범위(2026-10-13, 소유자가 APK를 제공한 경우의 카카오내비 `com.locnall.KimGiSa`)입니다. 입력은 소유자가 제공한 APKMirror 번들 `com.locnall.KimGiSa_4.51.1-45101`(versionName 4.51.1, versionCode 45101, minSdk 26, targetSdk 36, APKMirror 게시 2026-10-02, `capabilities: auto`, 번들 SHA-256 `23ac47d9…`, base.apk `1b6b317f…`)입니다. base.apk의 dex 10개 중 패키지 검사로 위치·AA 코드가 몰린 classes6(위치 SDK, KNSDK, `com.kakaomobility.common.auto`), classes(androidx car app 라이브러리), classes7(앱 주행 UI `com.kakaomobility.navi.drive`)만 jadx 1.5.6(-Xmx1500m, --no-res, nice 19)으로 하나씩 디컴파일했고 나머지는 원시 dex 문자열만 검사했습니다. 매니페스트와 resources.arsc는 바이너리 문자열 풀을 직접 디코드했습니다. arm64 분할의 네이티브 라이브러리는 기호·문자열만 보았고 지도 렌더러(`libkmrenderengine`, `libK3fAndroid`)와 보안·크래시 라이브러리뿐이며 내비 엔진은 Kotlin(KNSDK, 내부 버전 문자열 26092302)입니다. 카카오맵(`KAKAO_MAP_APP_STATIC_RE_2026-10-13.md`, KNSDK 23102601)보다 난독화가 강해 대부분 `compiled from` 이름이 없고 Kotlin SMAP 이름으로 원래 클래스를 확인했습니다. 서버·계정과 상호작용하지 않았고 폰·차 실행은 없습니다. APK, 디컴파일 코드, 덤프는 저장소에 두지 않습니다(작업 파일은 git 제외 `tools/kakaonavi-*`). 서버 원격설정 값은 조회하지 않았으므로 상수는 **앱 내장 기본값**입니다.

## 1. 위치 출처와 안드로이드 오토
- (FACT) **카카오내비는 정식 AA 내비 앱입니다.** 서비스 `com.kakaomobility.common.auto.service.NavigationCarAppService`(액션 `androidx.car.app.CarAppService`, 범주 NAVIGATION과 FEATURE_CLUSTER, `androidx.car.app.action.NAVIGATE`), `com.google.android.gms.car.application` 메타데이터, `androidx.car.app.minCarApiLevel` = 1, `car-app-api.level` = 8, Car App Library 1.7.0. 세션 `NavigationSession`/`ClusterSession`, 화면 `NaviScreen`, 표면 렌더러 `SurfaceRenderer`가 있습니다. 경로 안내 엔진은 폰에서 도는 KNSDK이고 AA 화면은 그 결과를 그립니다.
- (FACT) **차 위치 수신 경로가 없습니다.** R8로 이름이 바뀐 `CarSensors` 인터페이스(`knmsdk.c2.b`, compiled from CarSensors.java)에는 나침반·가속도·자이로 리스너 추가/해제 메서드 6개만 남았고 `addCarHardwareLocationListener`/`removeCarHardwareLocationListener`는 **사용처가 없어 제거되었습니다**. 구현체 `ProjectedCarSensors`(`knmsdk.c2.c`)는 생성자에서 `CarHardwareLocation` 기본값 필드만 만듭니다. `CarHardwareLocation` 타입은 10개 dex 중 classes.dex(라이브러리)에만 있고, 문자열 "Car-GPS", NMEA 리스너, `setTestProviderLocation`은 어디에도 없습니다.
- (FACT) `CarHardwareManager`(`knmsdk.z1.a`)의 유일한 앱 사용자는 `AutoHardwareManager`이며, 세션 시작 시 모델·에너지·톨카드·속도·주행거리·나침반·자이로·가속도를 항목당 10000 ms 제한으로 한 번 읽어 분석 이벤트(`NavigationSession.q0` → `AutoTracker` START)로 보냅니다. 내비 계산에는 쓰이지 않습니다.
- (FACT) AA 연결 효과: `NavigationCarAppService.onCreate`가 KNBaseSDK `x0`(`KNInterfaceType_IVI`) → `KNGPSManager.O` → 위치 SDK `l0` → `isCarConnected`=true. 이 값은 서버 LTE 측위(FIN) 요청 필드, 실내·주차 층 감지(`FloorChangeDetector`), 로그에만 쓰이고, `VehicleOutdoorPositionUseCase.z`는 값을 저장만 하며 읽는 곳이 없습니다. `onDestroy`에서 SmartPhone으로 되돌립니다.
- (FACT) 실제 위치 입력은 카카오맵과 같은 구조입니다. `KNGPSManager`(`knmsdk.u50.b`)는 서버 설정 `positioning_opt`(기본 0)가 0 또는 3이면 카카오모빌리티 위치 SDK(`KMLocationManager`, 500 ms 출력)를, 그 밖이면 레거시 LocationManager/Fused를 씁니다. 위치 SDK의 `VehicleOutdoorPositionUseCase`는 폰 "gps"(LocationManager, `GnssStatus` 동반), "fused"(Play 서비스, 제공자 "flp"는 "fused"로 개명), 서버 FIN 측위를 합쳐 1000 ms마다 우선순위 finOn > KF(칼만) > finOff > lastLoc 중 하나를 냅니다.
- (FACT) 휴면 경로: 선택 로직에 "external" 분기와 서버 설정 `EtcConfig.useExternalGps`(기본 true)가 있으나, `PositionInfo.externalFlag`는 복사 생성자 외에 설정하는 코드가 없고 "external"을 내는 `ForwardPropagator` `s`에 위치를 넣는 호출이 없으며 `getUseExternalGps`는 DTO 변환에만 쓰입니다(INFERENCE, 높음: 이 빌드에서 외부 위치 입력은 비활성).
- 결론(INFERENCE, 높음): 카카오내비는 AA 화면에 내비를 띄우지만 **위치는 폰 GNSS/Fused(+FIN)만** 씁니다. 안드로이드 오토 앱의 Fused 주입 경로는 17.9에서 도달 불가로 분석됐으므로(`ANDROID_AUTO_APP_STATIC_RE_2026-10-12.md`) 헤드유닛 LOCATION(우리 치환 위치 포함)은 카카오내비에 도달하지 않습니다. Play 서비스 Fused 내부는 범위 밖입니다. 차/폰 전환 규칙은 존재할 대상이 없습니다.

## 2. 유효성 판정 (FACT, 앱 내장 기본값)
- 모의 위치: `KNGPSManager.onLocationChanged`/`onRawDataChanged`가 `isMock`(API 31+) 또는 `isFromMockProvider`이면(위치 SDK 경로는 `KMLocation.isFakedGps`) 버리고, 샌드박스 서버일 때만 허용합니다. 리소스에 "모의 위치 정보가 감지되어 길안내가 제한됩니다. 모의 위치 앱 종료 후 이용해주세요."(`gps_abusing_info_text`, `knu_toast_drive_gps_abusing`)가 있습니다.
- `ValidCheckConfig` 기본값: distBoundary1St 500, legacyPriorityTime 2000 ms, timeBoundary1St 120000 ms, speedThLow 0.8333 m/s, accuracyThLowSpeed 14 m, accuracyThNormalSpeed 35 m, timeDiffForVelocityFromDist 120000 ms, velocityDiffTh 13.89 m/s, timeDiffForFlpOnlyCount 10000 ms, minFlpOnlyCount 3, accuracyThFlpOnly 19 m, gpsMaxError 50 m, reverseTimeThMin −30000 ms, reverseTimeThMax −300 ms.
- `ValidChecker`(`knmsdk.o90.g`): gps는 정확도 ≤ 35+30 = 65 m이고 속도 ≥ 0; fused는 정확도 ≤ 35 m, 속도 < 0.833 m/s이면 ≤ 14 m, 속도 ≥ 0, 방위 ≥ 0. 직전 수용 위치 대비 |보고 속도 − 거리/시간| > 13.89 + 50/Δt(Δt < 120 s)면 거부. 같은 좌표가 반복되고 `speedAccuracy` = 0인 gps 거부. (위성시각 차 − 수신시각 차) > 2000 ms 또는 수신 간격 < 100 ms면 거부. 역행 < −30 s 거부, −30~−0.3 s면 기준 초기화. gps(0) > fused(1) 우선, 낮은 소스는 2000 ms 뒤 수용. **연속 10회 거부되면 강제 수용**. 방위 정확도 < 0이면 직전 방위로 덮어씀.
- 출력: 마지막 측정 뒤 2000 ms(`AlgorithmConfig.maxValidTimeGapMs`)를 넘으면 valid=false, 정확도 최소 50 m. 칼만 `maxEstimateTime` 2000 ms, `maxTimeGap` 30000 ms. `KNGPSData.isOrigin` = 추정 지연 < 500 ms. 레거시 경로: 정확도 ≤ max(`positioning_hdop`, 40).
- 위성: `GnssStatus`(사용 수, `usedInFix`)는 openSky 정확도(`OpenSkyChecker`)와 로그·DB에 쓰이며 `ValidChecker`에는 위성 조건이 없습니다.
- 신뢰 플래그(`KNGPSData.e`): 속도·방위는 정수로 잘림(km/h, 도); 속도 > 0과 방위 ≥ 0이 10회 연속이어야 speedTrust/angleTrust.

## 3. GPS 상실과 터널 (FACT, 표시는 일부 INFERENCE)
- 감지: 위 2000 ms 규칙. 레거시 경로는 100 ms 틱 10회 동안 새 위치가 없으면 직전 값의 무효 사본(+1 s).
- 경로 진행(`KNMapMatching`, `knmsdk.i60.j`): 무효 위치일 때 현재 또는 50 m 이내 인접 링크가 터널(시설 2)·지하차도(4)면 **max(마지막 정합 속도의 80%, 링크 교통 속도, 5) km/h로 경로를 따라 기간 제한 없이 전진**(`KNMapMatchingSt_Simulated`, 로그 "시뮬레이션 매칭(터널)"); 그 밖에서는 80%로 2회까지. 정합 점수 ≥ 998074 Trusted, ≥ 994629 Doubtful. 카카오맵과 같습니다.
- 표시(카카오내비 자체 UI `KNUGpsValidUseCase`, `knmsdk.yh0.a`): 정합 GPS가 무효가 된 시각부터 **5 s 이상이면 `invalidWith5`**(`KNUSpeedUseCase`가 속도를 무효로 표시), **20 s 이상이면 `invalidWith20`**(지도 차 아이콘 교체 `KNUMapLocationAnchorUseCase`, AA 차 아이콘 정보 `CarIconInfoType.INVALID_GPS` = "GPS 신호 약함"(`car_gps_not_valid`), 보험 주행 토스트 "GPS정보가 수신되지 않습니다. 보험은 유지됩니다."). 이 소비처들에서 터널 예외는 찾지 못했습니다(카카오맵의 `KNComponentInfoView` 터널 예외와 다름). 정합 사본은 입력의 valid를 그대로 복사하므로 터널 가상 진행 중에도 무효로 셈(INFERENCE, 중상).
- 폰 센서: 위치 SDK(`knmsdk.o80.a`)가 가속도·자이로·기압·중력·회전벡터·걸음을 등록하지만 소비처는 이동 판정, 실내·주차 층 감지, 급변 이벤트(`MovementDetector`, `FloorChangeDetector`, `InOutChecker`, `FeatureDetector`)이고 실외 위치 선택 입력에는 없습니다(INFERENCE, 중상). 차 속도는 쓰지 않습니다.
- 재획득: 첫 유효 위치에서 후보 탐색 반경 38 m→500 m(카카오맵과 같은 상수).

## 4. 방위
방위는 정수 도로 잘리고 0은 유효, 없음은 −1(angleTrust 리셋). 위치 SDK는 `Location.getBearing()`을 존재 여부와 무관하게 읽습니다(없으면 0.0). 정합되면 링크 방향으로 대체될 수 있습니다. 첫 연결 특별 처리는 찾지 못했습니다. 저속 방위 유지 규칙은 이번에 재확인하지 않았습니다.

## 5. 이탈·재탐색
`KNGuidance`(`knmsdk.l60.m`): 위치가 유효하고 원본(isOrigin)이며 `KNMapMatchingSt_OutOfRoute`일 때만 이탈 처리. 무효 위치로는 재탐색하지 않습니다. 보고 정확도로 가중하는 분기는 찾지 못했습니다. 구조는 카카오맵 기록과 같고 이탈 거리 상수는 개별 재확인하지 않았습니다.

## 6. 네이버와 비교
| 항목 | 카카오내비 4.51.1 | 네이버 6.8.0.5 | 카카오맵 5.17.8과 차이 |
|---|---|---|---|
| AA 앱 등록 | 예(NAVIGATION, 클러스터) | 예 | 카카오맵은 등록 안 됨 |
| AA 차 위치 | 사용 안 함(리스너 메서드 제거) | `CarHardwareLocation` 병렬 사용 | 같음 |
| 정확도 | gps ≤ 65, fused ≤ 35(저속 ≤ 14), 레거시 ≤ 40 | < 30 유효, ≥ 50 나쁨 | 같음 |
| 속도·방위 | gps는 속도 음수만 거부, fused는 둘 다 음수 거부 | 없으면 무효 | 같음 |
| 시각 | 도착 대비 측정 시각 2 s 어긋남·< 100 ms 간격 거부, 2 s 무측정 → 무효 | 수신 시각 같으면 무효 | 같음 |
| 위성 | 로그·openSky만 | 사용 안 함 | 같음 |
| 모의 위치 | 거부 + 안내 제한 문구 | 이름표만 | 문구는 카카오내비만 확인 |
| GPS 상실 표시 | 5 s 속도 무효, 20 s "GPS 신호 약함"(터널 예외 없음) | 10틱 무효 → 회색 | 카카오맵은 20 s 배너, 터널 예외 |
| 터널 진행 | 경로 따라 80%/링크 속도, 무기한 | 마지막 속도 가상주행 | 같음 |

## 7. 제품 함의
1. (FACT+INFERENCE, 높음) 25 m·40 m·없음, 횡오차, 위성 S5, beta.12 arm 모두 카카오내비 동작에 영향이 없습니다. 네이버용 설계를 바꿀 이유가 없습니다. 위험: 없음.
2. (INFERENCE) 카카오내비 사용자의 G1은 폰 GNSS와 앱 자체 터널 진행에 달려 있고, 긴 터널(20 s 이상)에서는 "GPS 신호 약함"이 뜰 것으로 예상됩니다. 헤드유닛 LOCATION으로는 바꿀 수 없습니다.
3. (FACT) 폰 쪽 모의 위치 주입은 대안이 아닙니다: 카카오내비가 버리고 안내를 제한합니다.
4. (INFERENCE) 이후 버전이 `CarHardwareLocation`을 쓰기 시작하면 결론이 바뀝니다. 제공자 이름이 gps/fused가 아니면 현재 `ValidChecker`는 우선순위 최하위로 취급합니다. 설치 버전 매니페스트와 이 메서드 존재 여부를 재확인하는 것이 싸고 확실합니다.

## 버전과 미해결
분석 대상은 4.51.1(versionCode 45101, KNSDK 26092302, Car App Library 1.7.0)입니다. 소유자 폰 설치 버전은 확인하지 않았습니다(소유자 작업 가정: 동일 동작). 서버 원격설정(`positioning_opt`, `positioning_hdop`, 위치 SDK 설정, FIN·외부 GPS 기능)은 조회하지 않았습니다. `VehicleOutdoorPositionUseCase.m`과 `location.navi.g.h`의 일부 코루틴 본문은 jadx가 복원하지 못해 선택 세부(특히 FIN 분기)는 부분 해석입니다. classes2~5, 8~10은 문자열 검사만 했습니다. 이탈 거리·저속 방위 상수, AA 화면의 속도 숫자 회색 처리 여부, Play 서비스 Fused가 차 위치를 쓰는지는 미해결입니다.
