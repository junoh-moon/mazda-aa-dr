# T map 11.8.4.4075 정적 역공학: 안드로이드 오토에서의 위치 출처, 유효성 판정, 터널 처리 (2026-10-13)

소유자가 AGENTS.md에 허용한 범위(2026-10-13, `com.skt.tmap.ku`만). 입력은 소유자가 제공한 APKMirror 번들 `com.skt.tmap.ku_11.8.4.4075-4004`(release 11.8.4.4075, versionCode 4004, min API 30, `capabilities: auto`, 번들 SHA-256 `f1ac206a…`, base.apk `f39ca787…`). dex 32개 중 키워드가 몰린 classes30, classes, classes24, classes31, classes28만 jadx 1.5.6(-Xmx1500m, --no-res)으로 하나씩 디컴파일했고, 디컴파일이 실패한 `LocationReliabilityChecker.isReliableLocation`은 단일 클래스 bad-code 출력으로 읽었습니다(일부 제어 흐름이 깨져 있어 해당 부분은 INFERENCE로 표시). 네이티브 라이브러리(`libNavigationSDK.so`, `libtmapnavigationengine.so`)는 심볼·문자열만 보았고 디스어셈블하지 않았습니다. 서버·계정과 상호작용하지 않았고 폰·차 실행은 없습니다. APK, 디컴파일 코드, 덤프는 저장소에 두지 않습니다(작업 파일은 git에 추가하지 않은 `tools/tmap-*`).

## 1. 위치 출처 (FACT)
- 앱 위치 제공자는 둘뿐입니다: `GpsProvider`(이름 "gps", 폰 `LocationManager` GPS, 1000 ms/0 m)와 `com.skt.tmap.location.c`(이름 "fused", Play 서비스 Fused, 고정확도 1000 ms). 등록은 `com.skt.tmap.engine.c.b/c`. 그 밖은 디버그용 NMEA/CSV 재생과 모의주행(`AutoDriveLocationProvider`)입니다.
- **차량 위치를 읽는 경로가 없습니다.** `CarSensors`, `addCarHardwareLocationListener`, `addSpeedListener`, `addCompassListener`의 메서드 이름 문자열은 32개 dex 전체에서 classes.dex(androidx 라이브러리 정의)에만 있고, classes.dex의 앱 코드에도 호출자가 없습니다. `CarHardwareManager`는 `TmapCarSession`에서 `CarInfo`(차 모델, 에너지 프로필·잔량)에만 쓰입니다. 문자열 "Car-GPS"도 없습니다.
- SDK에는 차량 연동 고리(`AutomotiveDataInterface.isDrGPS` → 제공자 "dr_gps", 우선순위 20)가 있으나 앱에 구현·호출(`setAutomotiveDataInterface`)이 없고, `TunnelLocationProvider`("tlp")도 생성되지 않습니다.
- 안드로이드 오토 차 앱(`TmapCarAppService`/`TmapCarSession`/`car.x`)은 폰과 같은 `TmapLocationListener` 파이프라인을 씁니다. AA 연결 시 위치 extras에 `androidAutoConnected=true`가 붙을 뿐입니다(`location.m.onLocationChanged`).
- 선택: `location.m.d`가 1000 ms마다 큐의 위치 중 `LocationPrioritySelector` 우선순위가 가장 높은 것 하나를 엔진에 넣습니다(GPS 30 > FUSED 40 > SAVED 50 > DUMMY 70; 음영구역+약신호일 때 FUSED 70, DUMMY 40). 차/폰 전환 규칙은 존재할 대상이 없습니다.
- 결론(INFERENCE, 높음): AA 연결 중에도 T map은 **폰 자체 GNSS/Fused만** 씁니다. 안드로이드 오토 앱이 차 LOCATION을 폰 위치 스택에 주입하는 경로는 17.9에서 도달 불가로 분석됐으므로(`ANDROID_AUTO_APP_STATIC_RE_2026-10-12.md`), 헤드유닛 LOCATION(우리 치환 위치 포함)은 T map에 도달하지 않습니다. Play 서비스 Fused 내부는 범위 밖이라 확인하지 못했습니다.

## 2. 유효성 판정 (FACT, 상수)
- 시각: `TmapLocationManager.onLocationChanged`와 `m.onLocationChanged`가 받은 위치의 `setTime`을 앱 수신 시각(`System.currentTimeMillis`)으로 덮어씁니다. 네이티브에는 `getElapsedRealtimeNanos`와 덮어쓴 `getTime`이 함께 갑니다.
- 위성 수: 폰 `GnssStatus.usedInFix` 개수(`GpsStatusListener.fixedSatelliteCount`)를 extras `satelliteCount`로 붙입니다(모의 위치면 4). 쓰이는 곳: V1 도달성 검사 면제(≥ 4), "정확한 GPS 받은 적 있음"(정확도 0~50 그리고 ≥ 4), 음영구역 해제(> 0), 기압 터널 모델(= 0), V2 역방향 강제수용(≥ 3), 재밍 탐지, 네이티브 입력.
- V1(`LocationReliabilityChecker`, 기본): 제공자 "gps"에는 정확도 문턱이 없고, 그 밖(fused)은 정확도 없음 또는 > min(200, 45 × 경과초)이면 보류(초기 60~120 s는 하한 100). 수신 간격 < 900 ms(위성 < 4 또는 비 GPS)면 불규칙, 속도 0이면서 직전 > 10 m/s이고 방위 없음 등은 버퍼링, 버퍼 3개 이상이고 10 s 넘게 수용이 없으면 가중평균 위치로 복구(가중치 1/(정확도+1)). 방위가 시간당 50°를 넘게 바뀌면 좌표 기반 방위로 교체(AA 연결 시 생략). 점프: 두 위치 사이 계산 속도(km/h)가 1.85 × max(20, 평균속도 km/h × 경과초)를 넘으면 보류(상한 1000), AA 연결이거나 위성 ≥ 4면 면제. 터널 출구 구역 200 m/30 s/해제 300 m, 출구 점프 > 2 × 속도 × 시간이면 경로각으로 외삽. AA 연결이면서 위성 < 4일 때 방위와 경로각 차 > 20° 또는 2~10 s 공백이면 엔진 모의주행 위치(20 m, 10°)와 비교하는 분기가 있으나 디컴파일이 깨져 정확한 결과는 미확인(INFERENCE).
- V2(`LocationReliabilityCheckerV2`, 원격설정 `united_json_use_gps_filter_v2_11_7`일 때): 정확도 ≤ 30 즉시 수용, ≤ 50은 직전이 거부였을 때 수용, 그 밖·없음은 거부(연속 20회 거부 뒤 강제 수용), 수신 간격 < 900 ms 거부, 시각 차 > 10 s 거부, 속도 > 300 km/h 점프 거부, 모의 위치 거부(설정 시 신뢰), 방위 변화율 60°/s 제한, 터널(분기 없음)에서 속도 없고 위성 0이면 거부, 터널 방위와 경로각 차 > 90° 거부, 출구 반경 200 m·45° 조기 수용.
- `NavigationManager`: 상태 = 가상 GPS 1이면 TUNNEL, 2면 UNDERPASS, 아니면 정확도 ≤ 100 m이고 더미가 아니면 GOOD, 그 밖 BAD. 가상 GPS 중 fused 정확도 > 100은 버림. 정합점과 200 m 이상 차이는 점프 이벤트(로그·UI).
- 기타: 속도 평활(원격설정, 정확도 > 30이면 직전 속도 유지), 칼만 필터는 SAFE_DRIVE(비경로 안전운전 모드)에서만.

## 3. GPS 상실과 터널 (FACT)
- 수용 위치가 없으면 2 s(직전이 더미이거나 음영구역이면 1 s)마다 "dummy" 위치(마지막 좌표, 정확도·속도·방위 없음)를 엔진에 넣습니다. 터널·지하차도의 경로 따라 진행은 네이티브 엔진의 가상 GPS(`RGData.eVirtualGps`, `NcMapMatch.GetVirtualGpsFlag`)가 합니다(INFERENCE: 진입·전진 규칙은 네이티브라 미확인).
- 화면: BAD가 15 s 이어지면 `noLocationSignal`; AA 화면(`car.screen.e1.q`)은 (noLocationSignal 또는 음영구역+약신호)이고 실주행이면 `GpsSearchingView`를 띄우며 문구는 터널 링크면 `gps_searching_in_tunnel`, 아니면 `gps_searching`, 재밍이면 `gps_searching_jamming`. 음영구역 진입 시 토스트 `msg_shadow_area_notice`. 리소스에 "GPS 탐색중", "터널 주행중", "GPS 신호 간섭 구역", "터널안이나 지하도에서는 속도가 정확하지 않을 수 있습니다."가 있으나 ID 대응은 미해결.
- 자체 추정: (a) 터널 기압 ML(`TmapTunnelLocationAnalyzer`, 터널별 TFLite 모델, 입력 진입속도·경과시간·기압고도 → 진행률, 출력 정확도 10 m, 방위는 직전값)이지만 **AA 연결 시 결과를 버립니다**(`n.g`가 AA이면 null 반환). (b) V1/V2의 분기 없는 터널 경로각 외삽. 폰 센서는 기압(6)과 게임 회전벡터(15)만, 자이로 적분 DR은 없습니다. 차 속도는 쓰지 않습니다.

## 4. 방위 (FACT)
- 엔진 입력에서 방위가 정확히 0.0이면 359.9로 바꿉니다(`TmapNavigation.setLocationDataWithDR`); 방위 없음은 −1. V2는 방위가 없으면 직전 위치에서의 좌표 방위를 채웁니다. 원격설정 `united_json_fop_enabled`이면 Play 서비스 기기 방향(오차 ≤ 45°, 500 ms 이내, 50°/s 이하)으로 방위를 대체합니다. 표시 방위는 정합 결과(`nPosAngle`). 첫 연결 특별 처리는 V1 초기 600 s 규칙 외에 없습니다.

## 5. 이탈·재탐색
네이티브(`CNaviController`, `NcMapMatch`, `GetTvasReRouteData`) 안에 있어 정확도·횡거리 문턱을 확인하지 못했습니다. 앱 쪽은 재탐색 응답 처리와 `BlockReroute` 호출뿐입니다.

## 6. 네이버와 비교 (게이트)
| 항목 | T map 11.8.4 | 네이버 6.8.0.5 |
|---|---|---|
| AA 차 위치 | 사용 안 함(폰만) | `CarHardwareLocation` 병렬 사용 |
| 정확도 | V1: gps 무제한, fused ≤ min(200,45·dt); V2: ≤30 즉시/≤50 조건부; 상태 GOOD ≤100 | < 30 유효, 50 이상 나쁨 |
| 속도·방위 | 없으면 직전값으로 채움 | 없으면 무효 |
| 시각 | 앱 수신 시각으로 덮어씀, < 900 ms 간격 거부 | 수신 시각 같으면 무효 |
| 위성 | 폰 GnssStatus 사용(≥4/>0/=0 규칙) | 사용 안 함 |
| 모의 위치 | V2에서 거부 | 이름표만 |
| 회색/경고 | BAD 15 s → "GPS 탐색중"; 터널은 가상 GPS | 10틱 무효 → 회색 |

## 7. 제품 함의
- (FACT+INFERENCE) 25 m·40 m·없음, 횡오차, 위성 S5는 T map 동작에 영향이 없습니다. T map 사용자에게 G1~G3를 제공하려면 헤드유닛 LOCATION이 아닌 다른 경로가 필요하며, 현재 설계로는 불가능합니다.
- 네이버용 설계(A20/상한 25, 속도·방위 항상 포함, 0.0 회피)는 그대로 유지해도 T map에 해가 없습니다.

## 버전과 미해결
분석 대상은 11.8.4.4075(versionCode 4004)입니다. 소유자 폰의 설치 버전은 확인하지 않았습니다(소유자 작업 가정: 동일 동작). 동작 상당 부분이 원격설정(V2 필터, 칼만, FOP, 터널 모델, 속도 평활)에 달려 있고 서버 값은 조회하지 않았습니다. 네이티브 정합·가상 GPS·재탐색 문턱, Play 서비스 Fused가 차 위치를 쓰는지, 리소스 문구 대응은 미해결입니다.
