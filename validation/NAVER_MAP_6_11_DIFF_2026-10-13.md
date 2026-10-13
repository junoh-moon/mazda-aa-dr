# 네이버 지도 6.11.1.2 대 6.8.0.5 정적 비교 (2026-10-13)

소유자가 AGENTS.md에서 허용한 네이버 지도 역공학 범위입니다(소유자가 `adb pull`로 읽기 전용 추출한 설치본, 오프라인·로컬, 폰과 앱은 바꾸지 않음). APK, 디컴파일 코드, 덤프는 저장소에 두지 않았고 클래스·메서드 이름, 상수, 문자열만 적습니다. 폰, 차, DHU에서 실행한 것은 없습니다. 6.10.0.16은 분석하지 않았으므로 아래 차이는 **6.8.0.5 → 6.11.1.2 누적 차이**입니다.

- 입력: `naver-apk-6.11.1.2.zip`(base.apk SHA-256 `0921c03f…`, split_config.arm64_v8a, split_config.xxhdpi). 내비 SDK `NaviBuildConfig.n()` = "2.20.10", `a()` = "2026-09-23"(6.8.0.5는 "2.17.7", "2026-07-01").
- 방법: 41개 dex의 문자열·클래스 정의 표를 파이썬으로 색인해 6.8.0.5와 비교했습니다. 맵매칭 클래스는 여전히 난독화되지 않았고, 6.8.0.5 classes4 → 6.11 **classes34**, classes5/7 → **classes7**로 옮겨졌습니다. jadx 1.5.6(-Xmx1500m, -j 1, 한 번에 하나)으로 classes34와 classes7 전체를 풀었고, classes.dex(`NaviBuildConfig`, `NaviModule`)와 classes6(`CarLocationSource`, `CarLocationManager`, `LocationStatusControl`)은 단일 클래스만 풀었습니다. 6.8.0.5 쪽은 기존 classes4 디컴파일과 단일 클래스(`MMNaviLocation`, `LocationController`)를 썼습니다. 맵매칭 패키지 152개 파일은 패키지 이름을 지운 정규화 diff와 파일별 숫자·문자열 상수 diff로 비교했습니다. 난독화 메서드 글자가 많이 바뀌었으므로(예: `NaviBuildConfig.p()` → `i()`) 이전 기록의 메서드 글자는 6.11에 그대로 쓰면 안 됩니다.

표기: FACT = 클래스/메서드/상수/문자열, INFERENCE = 추론.

## 1. 판정 규칙: 같음

| 항목 | 6.11.1.2 | 판정 |
|---|---|---|
| 정확도 구간 `MMNaviLocation` | VERY_GOOD ≤ 30, NEARLY_BAD 40, BAD ≥ 50 또는 < 0, VERY_BAD 70; `isNotBadAccuracy` ≥ 30 또는 < 0 | 같음(FACT, 6.8.0.5 단일 클래스와 diff 차이 없음) |
| `GpsChecker`(< 30, 속도·방위, 수신 시각, 터널 60°) | 이름 바꿈만 | 같음 |
| `VirtualDriveNoGpsChecker`, `VirtualDriveShadowChecker`, `VirtualDrive` | 정규화 diff 없음 또는 이름만 | 같음 |
| R1 `GpsAdjuster.p` | 직전 정확도 < 30, `SelectedLocation.v()` 아님, `getTime()` 차 500~1500 ms, ≥ 50 m, > 2 × max 속도 | 같음 |
| R2 `ValidGpsSelector.c`(옛 `a`) | 3000 ms 안에서 200 km/h 초과 | 같음 |
| `SwitchBuffer` 3회, 둘 다 무효면 현 소스 유지 | 같음. 단 점프·정지(아래) 우회 시 `SwitchBuffer.d()`로 요청 수를 0으로 초기화(새 줄) | 거의 같음 |
| `GpsLocationStatusManager` 연속 10틱 → INVALID | diff 없음 | 같음 |
| 방위 0과 −1 | `isBadCourse` ≤ 0, `isInvalidCourse` < 0, `isAbnormalGps` = 속도 또는 방위 없음 | 같음 |
| `DualGpsSelector`, Half/Invalid 선택기 | 이름만, 그리고 2번 항목 | 바뀜(2절) |
| `RerouteBufferManager` 30/40/50, 무시 3 | 이름만 | 같음 |
| 도로폭 `LaneUtils` 차로 × 3.3/2 + 8, `RouteMatchingDistanceData` 1.5×/2.3× | 같음 | 같음 |
| `VectorAngle`, `AvnVirtualDrivingFilter` | 이름만 | 같음 |
| 1 s 틱 `RealMapMatchingRunner` | 1000 ms 유지. 재생 모드 플래그(`c(boolean)`)가 새로 생겨, 켜지면 틱을 멈추고 입력마다 매칭 | 실시간 주행은 같음 |
| 입력 경로 `CarLocationManager.Type.a()` = 항상 Parallel, `LocationController.a/b`(provider "car" → ANDROID_AUTO, fused는 gps가 오기 전까지만) | 같음. `update()`는 재생 모드면 실시간 입력을 버림 | 같음 |
| 위치 변환 `mapmatching.LocationExtensionsKt`(수신 시계, `getTime()`) | diff 없음 | 같음 |

## 2. 새 규칙 (FACT)

1. **소스 정지(freeze) 감지와 즉시 전환** — `ValidGpsSelector.b`, `k`, `j`; `DualGpsSelector`("type changed by Freeze. bypass SwitchBuffer"). NORMAL 모드(SHADOW, 출발 구역, UNVERIFIED_AVN이 아님)에서 두 소스가 모두 유효할 때, 소스마다 카운터를 둡니다. 연속 두 위치의 `getTime()` 차가 0 초과 3000 ms 이하이고, 속도 ≥ 10 km/h이며, 이동 거리 < 1.0 m이면 +1합니다. 시각 차가 0 이하면 카운터를 그대로 두고, 그 밖의 경우는 0으로 되돌립니다. 카운터가 3 이상이면 그 소스를 "frozen"으로 보고 R2처럼 **`SwitchBuffer`를 거치지 않고 다른 소스로 바로 바꿉니다**. 상수는 `o` = 10.0, `p` = 1.0, `q` = 3, `r` = 3000입니다. 로그 문자열 "jumped", "frozen", "jumped+frozen"은 debug 수준이라 파일에 남지 않고, 결과는 info 줄 "GPS type changed"로만 남습니다.
2. **평행 도로 재탐색** — `ParallelRerouteChecker`(새 클래스)와 `RerouteChecker.j`. 지역 매칭 링크가 경로 링크와 다르고, angleRatio > 0.9, 지역 결과가 양호, fraction이 0~1이고, 경로까지 거리(`RouteCandidate` distance)가 baseLinkWidth의 1.6배를 넘는 틱이 2회 이상 이어지며 이번 틱에 1.7배를 넘으면 플래그를 세웁니다. U턴 후보와 회전교차로·교차로 링크(`LinkKindExtensionsKt.a`)는 빼고, 경로 거리가 500 m 넘게 바뀐 뒤 5틱 동안은 쉽니다. 플래그가 서면 `REROUTE_NAVER_LOCAL_GOOD` 판정이 열리고 점수 여유가 2점 줄어듭니다. 6.8.0.5에서는 이 경로가 `a0()`(옛 `Y()`)가 참일 때만 열렸습니다.
3. **정지 필터** — `AvnStopFilter.a/x/y`: 선택 위치가 ≥ 10 km/h이고 최근 5개가 8 s 안에 ≥ 8 m 움직였는데, 차 쪽 마지막 3개가 8 s 안에서 1 m 안에 머물러 있으면 "AVN 좌표 freeze - 정차 판단에서 제외"로 처리합니다. 이 경우 차 쪽은 정차 판단에서 빠집니다. `MobileStopFilter`의 "Unset Stop - accumulated moving"(`stopFilter.ConstantsKt`: 15000 ms, 7, 8.0 등)도 새로 생겼습니다. 정차 판단만 바꿉니다.
4. **CarPlay 전용**(`MMNaviLocation.isCarplay()` = `AvnConnect.CAR_PLAY`일 때만): `MmNaviLocationExtensionsKt.a/b`(대체 정확도 51/50, 나쁨 판정 > 50), `GpsStatusTracker`, `VirtualDriveController.g`의 "gpsAnchored catchUpSpeed"(SHADOW 가상주행에서 정확도가 정확히 50인 CarPlay 위치 쪽으로 속도를 1.15배 안에서 따라감). 안드로이드 오토 위치는 `LocationController.a`가 `ANDROID_AUTO`로 붙이므로 **해당하지 않습니다**.
5. 그 밖: `RouteLinkExplorer`의 "route changed … trust forward candidate (skip anti-back)", `MergeRoadChecker`의 adjacent grace/kept/released, `GoalSpotInfo`/`WaypointSpotInfo`의 상수 추가. 모두 경로 매칭 세부이며 GPS 판정과는 무관하다고 보았습니다(INFERENCE).
6. 기압계: `LocationController.updateAltimeter`, `AltimeterReading`, `AltitudeBuffer`, `AltitudeSessionLogger`. 쓰임새는 세션 로그와 `TripReport.Altitude`뿐이고 맵매칭은 참조하지 않습니다(FACT: classes34/7 참조 검색). 내비 경로에 관성 센서 기반 추측 항법이나 터널 위치 모델은 여전히 없습니다. `PedestrianDeadReckoning`, `walk/mapmatching/inertia`, `kalman`은 도보 내비 전용입니다. 모의 위치는 여전히 이름표("mock")와 트립 리포트 생략에만 쓰입니다.

## 3. 기록 스위치: 그대로 있음 (FACT)

`NaviConfig`(classes7): 파일 "navi.config", 위치 `NaviModule.g()` = `getExternalFilesDir(null)`(없으면 내부 폴더), 한 줄에 `키=값` 하나, `#` 줄은 무시, 값이 정확히 "1"이어야 켜집니다. 키 10개는 6.8.0.5와 같습니다(`trace`, `debug_ui`, `save_guidance`, `save_pbf`, `native_crash_test`, `include_tag`, `exclude_tag`, `keyword`, `map_matching.local_location_threshold`=20, `ignore_input_location`). 새 키는 없습니다. `trace`(`NaviConfig.q()`)를 켜면 `InternalNaviLogger.r`이 `FileLogger`(`NaverNavi/trace/trace_<시각>.log`, 30줄 버퍼)를 붙이고, `SessionLogger.init`이 GuidePoint, Nmea(`NaverNavi/nmea/log/<시각>_<routeId>.nmea`, `$NMALC` 형식 그대로), 그리고 **새로 Altitude(`NaverNavi/altitude/log`)** 로거를 붙입니다. 릴리스 출력 대상도 같습니다. Timber는 `NaviBuildConfig.c()` = `BuildConfigExtensions_androidKt.a()` = false라서 없고, Console은 `d()` = false, Nelo 3개는 error만 보냅니다. MMLogger debug/verbose 게이트 `NaviBuildConfig.i()`는 상수 false입니다(옛 `p()`). `ignore_input_location` 판정(`NaviConfig.e()`)에는 `NaviBuildConfig.h()`(상수 false)가 OR로 붙었습니다. 설정을 다시 읽는 곳은 `NaverNaviInnerControl.u()`이며, **생성자와 안내 종료 `g()`에서만** 불립니다(6.8.0.5 기록에서는 안내 시작 때도 읽었음). 세션 로거는 안내를 시작할 때 만들어집니다.

소유자용 절차(6.11.1.2, 소유자 승인 시):
1. LF 줄끝으로 `trace=1` 한 줄만 쓴 `navi.config` 파일을 만들어 `/sdcard/Android/data/com.nhn.android.nmap/files/navi.config`에 `adb push`합니다.
2. 네이버 지도를 완전히 종료한 뒤(앱 정보 → 강제 종료) 다시 엽니다. 이미 실행 중이었다면 안내를 한 번 시작했다가 종료해도 됩니다.
3. 안내를 시작해 주행합니다.
4. 주행 뒤 같은 폴더의 `NaverNavi/trace`, `NaverNavi/nmea/log`, `NaverNavi/altitude/log`를 `adb pull`하고, 폰에서 지웁니다.

`ignore_input_location`은 절대 넣지 마십시오. adb가 이 폰의 Android/data에 접근할 수 있는지는 확인하지 못했습니다.

## 4. 판정 (INFERENCE, 중요도 순)

제품 가정 다섯 가지(25 m 상한이 30 m 경계 아래, 긴 터널 출구에서 상한 때문에 R1이 켜짐, 차↔폰 전환 3회 버퍼와 R2, 10틱 무효 → 회색, 방위 0.0 = 나쁜 침로)는 6.11.1.2에서 **모두 그대로입니다**.

1. **freeze 즉시 전환(2-1)**: 차 쪽이 ≥ 10 km/h를 보고하면서 1 m 미만으로 3회 연속 움직이면, 폰이 유효한 순간 바로 폰으로 넘어갑니다. 우리 DR은 매 송신마다 속도에 맞춰 전진하므로 평소에는 해당하지 않습니다. 다만 beta.13/14에서 "정지·방위 유지"나 재전송 때문에 같은 좌표를 속도 ≥ 10 km/h로 보내는 경로가 없어야 합니다. 재생 테스트에 이 조건을 단언으로 넣을 것을 권합니다. 순정 유령 속도(정지 중 속도 > 0)가 10 km/h 이상이면 이 규칙이 순정 쪽에서도 발동할 수 있습니다.
2. **평행 도로 재탐색(2-2)**: 지역 매칭이 다른 링크(예: 터널 위 지상 도로, 측도)를 잡으면 경로에서 baseLinkWidth의 약 1.7배만 벗어나도 재탐색이 열립니다. 6.8.0.5의 2.3배보다 엄격합니다. 터널 안 횡오차, 그리고 출구 블렌드를 다시 검토할 때의 허용 폭을 더 좁게 잡아야 합니다. yaw 영점 개선의 우선순위를 높이는 근거가 됩니다. 가상주행이나 SHADOW 중에도 이 판정이 도는지는 확인하지 못했습니다.
3. 정지 필터 변경(2-3): 정차 판단에만 영향을 줍니다. beta.8의 정지 중 정확도 처리에 대한 영향은 낮다고 봅니다.
4. CarPlay 전용 변경(2-4), 재생 모드, 기압 로그: 안드로이드 오토에는 영향이 없습니다.

beta.12(A20/S5)의 예측은 바뀌지 않습니다. 위성 수는 6.11에서도 읽지 않습니다(`GnssStatus` 호출자 검색, classes34/7 기준).

## 5. 판정하지 못한 것

6.10.0.16의 내용, `ParallelRerouteChecker`가 가상주행·SHADOW 중에도 작동하는지, baseLinkWidth가 `LaneUtils` 폭과 같은 값인지, 앱 쪽 `NaviStore`가 SDK 상태를 회색 아이콘으로 바꾸는 대응(classes.dex는 단일 클래스만 풂; `LocationStatusControl` 구조는 같음), classes.dex에도 "navi.config" 문자열이 있는 이유, 나머지 37개 dex(클래스·문자열 비교만 함), AA 호스트가 `getTime()`을 같은 값으로 반복할 때 freeze 카운터가 어떻게 움직이는지, 그리고 모든 실행 시 동작. 디컴파일 결과는 git 제외 폴더 `tools/naver611-jadx`에 있습니다.
