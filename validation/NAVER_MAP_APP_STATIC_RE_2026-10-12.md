# 네이버 지도 6.8.0.5 정적 역공학: 차량 위치 처리, GPS 상태(회색), 속도 고정 (독립 2건, 2026-10-12)

소유자가 AGENTS.md에 허용한 범위(2026-10-12). 입력: 소유자가 제공한 APKMirror 번들 `com.nhn.android.nmap_6.8.0.5-60800007`(versionCode 60800007, 번들 SHA-256 `e6e8c7ad…`, `capabilities: auto`). jadx 1.5.6으로 키워드가 몰린 dex만 디컴파일(전체 41개를 한 번에 열면 2 GB에서 OOM). Opus(classes3/4/5/7/11/29와 classes.dex, 42개 dex 문자열·클래스 색인)와 Fable(classes3/5/6/7/11 일부와 사전 디컴파일 6개)이 독립 분석했고 **결론이 일치**합니다. APK, 디컴파일 코드, 덤프는 저장소에 두지 않습니다. 내비게이션 SDK(`com.naver.maps.navi.*`)와 AA 모듈(`com.naver.map.core.auto.*`)은 클래스 이름이 난독화되지 않았습니다. 정적 분석이며 폰·차에서 실행한 것이 없습니다.

## 위치 입력 (FACT)
- `CarLocationManager.Type.Companion.a()`는 설정과 무관하게 항상 **Parallel**입니다. 안드로이드 오토에서는 `CarLocationSource`(androidx.car.app `CarSensors.addCarHardwareLocationListener`, 제공자를 `"car"`로 재라벨)와 폰 자체 위치(`FusedLocationSource` 1000 ms/최소 1.5 m, `SystemLocationSource` `"gps"` 1000 ms)가 **둘 다** `LocationController`로 들어갑니다. `"car"`는 `AvnConnect.ANDROID_AUTO`, 나머지는 MOBILE로 분류됩니다. 차 앱 API 레벨 3 이상에서만 `CarHardwareLocation`이 동작합니다. 모의(mock) 위치는 이름표만 붙고 감점·거부가 없습니다(내비 dex 전체 검색).
- `RealMapMatchingRunner`가 **1000 ms 틱**마다 두 소스의 최신 위치 1개(`LocationSnapshot`)로 맵매칭을 돌립니다(입력 도착과 무관).
- **위성 수는 쓰지 않습니다**: `GnssStatus`/`registerGnssStatusCallback`/`getSatelliteCount`는 androidx 래퍼 안에만 있고 앱 호출자가 없으며 NMEA 리스너도 없습니다. 내비 dex에는 `SensorManager` 참조가 없고 폰 관성센서 기반 추측 항법도 없습니다. 내비·위치 엔진은 Kotlin이며 네이티브 라이브러리는 지도 렌더링(`libnavermap`)과 AR/VL 용도뿐입니다.

## 판정 규칙 (FACT, 상수는 두 분석이 일치)
- 정확도 구간(`MMNaviLocation`): 아주 좋음 ≤30, 거의 나쁨 40, 나쁨 ≥50, 없음 −1(= 나쁨). 속도·방위가 없으면 −1.
- 소스별 유효성(`GpsChecker`): **정확도 < 30 m**, 속도·방위 있음, **수신 시각이 새것**, 경로 거리 < 1.5 × 도로폭이어야 유효. 터널 안에서는 경로각 차이 ≤ 60°도 필요. 같은 수신 시각 → `INVALID_TIME_DIFF`, 정확도 ≥ 30 또는 없음 → `INVALID_ACCURACY`(가상주행 중 직전 VALID면 면제), 경로에서 벗어남 → `INVALID_LOCATION`.
- 도로폭 = 차로 수 × 3.3/2 + 8 m(차로 미상 4; 2차로 17 m, 4차로 22 m). on-route < 1.5 × 폭, **off-route > 2.3 × 폭(26~34 m)**.
- **이중 GPS 선택기**(`DualGpsSelector`, Valid/HalfValid/Invalid 선택기, `SwitchBuffer`): 전환에 3번 연속 요청이 필요, **둘 다 무효이면 현재 소스를 유지**.
- 보정 단계(`GpsAdjuster`): 나쁜 정확도면 직전 좌표·속도 유지. 같은 수신 시각, 속도·방위 없음, 점프(≥ 50 m 이고 > 2 × 속도) 등마다 카운터 +1. `GpsLocationStatusManager`: **연속 10틱 무효 → `LocationStatus.INVALID`**(valid 1회면 리셋).
- **회색 표시**: INVALID이고 링크 속성이 Tunnel이면 TUNNEL, UnderPass면 UNDERPASS, 그 밖은 INVALID 아이콘(`NaviStore.M0` → AA `LocationStatusControl`; VALID가 아니면 애니메이션 아이콘). 툴팁은 INVALID일 때만.
- **속도 고정의 정체**: `VirtualDriveNoGpsChecker`/`VirtualDriveController`. 수신 시각이 직전과 같거나 정확도 > 50이면 NO_GPS, 터널 링크에서 정확도 > 30이면 SHADOW로, **경로를 따라 마지막 양호 속도(최소 5 km/h)로 1초씩 전진(외삽)**합니다. 가상주행 속도(`SelectedLocation.s()`): 차 선택이면 차 속도(정확도 < 50, ≥ 5 km/h), 폰 선택이면 폰 속도(< 30), 둘 다 아니면 마지막 값. 화면 속도 = 맵매칭 출력 속도.
- `AvnVirtualDrivingFilter`: 터널 링크이고 폰이 정체·나쁨일 때, 차 정확도가 ≥ 50 또는 없음이거나 경로를 벗어났으면 **직전 차 위치를 그대로 반환**(고정).
- `VectorAngle` 방위 보정은 정확도 ≤ 33.3 m에서만, 재탐색 버퍼는 30/40/50 m 구간으로 가중.

## 해석 (INFERENCE)
- **폰에 갇히는 과정**: 터널 입구에서 순정 mode-0 메시지는 정확도가 없고 첫 대체 위치는 30 m라서 차 쪽이 무효 → 그동안 폰이 3회 이상 유효하면 폰으로 전환 → 폰 GPS가 죽으면 둘 다 무효라 폰을 유지 → 10초 뒤 회색 + 네이버의 가상주행으로 진입 속도 고정 → 우리 정확도가 40 m인 동안 차 쪽은 영원히 유효가 되지 못해 되찾지 못함. 111 s(b)와 53 s(trip9)의 회색 + 고정, 다른 차에서도 보고된 "진입 속도 고정" 일화와 일치. DHU에서 한 번도 고정이 재현되지 않은 것은 벤치 폰이 경로 위에서 유효한 적이 없었기 때문으로 보임.
- 60 s(c)의 회색이지만 속도는 따라옴: 차 선택이 유지된 채 옆 오차 52 m로 off-route(> 2.3 × 폭) → 강제 가상주행에 들어간 경우. 14 s(e) 정상: 연속 10틱에 못 미쳤거나 오차가 도로폭 안.

## 가설 판정
| 가설 | 판정 |
|---|---|
| G1 맵 맞춤 잔차 > 보고 정확도 | 원래 형태 **기각**(경로 판정은 정확도가 아니라 도로폭 1.5×/2.3×와 비교). 변형 **지지**: 옆 오차 > 2.3 × 폭 → 강제 가상주행 → 회색 |
| G2 위성 수 0 | 앱 수준에서 **기각**(앱이 읽지 않음). 안드로이드 오토 호스트의 처리는 판정 불가 |
| G3 40 m 유지 시간 | 시간 의존 로직 없음. 문제는 **값의 구간**(30~50 m는 위치 보정에서는 정상이지만 소스 선택에서는 무효) |
| G4 폰 위치 전환 | 네이버 내부 이중 GPS 선택기로 **지지** |
| P/F1 네이버 자체 GPS 상실 모드 | **지지**(`VirtualDrive`), 폰 관성센서는 쓰지 않음 |

## beta.12 arm 예측 (앱 코드 근거 반영; 이전 웹 조사 기반 "A20 악화" 예측을 **대체**)
- **A20**(보고 정확도 20 m): 30 m 미만이라 경로 1.5 × 폭 안에서는 차 쪽이 유효가 되고, 폰에 갇힌 상태가 약 3회 안에 풀리며, SHADOW 가상주행(정확도 > 30)도 꺼져 **회색·고정이 줄어들 것**으로 예측. 옆 오차가 도로폭 기준을 넘는 긴 터널에서는 회색이 여전히 날 수 있음.
- **S5**(위성 5): 앱이 위성 수를 읽지 않으므로 **효과 없음** 예측.
- **조절 가능한 변수**: 정확도(항상 포함, 30 m 미만, 어떤 경우에도 50 m 미만), 속도·방위 플래그 항상 포함, 1 Hz 틱마다 새 위치(2 Hz 이상이 유리), 옆 오차를 도로폭 기준 안에, 정확도 없는 첫 mode-0 메시지는 "나쁨"으로 집계됨.

## 미해결
안드로이드 오토(gearhead)가 차량 `Location.getTime()`을 어떻게 채우는지(와이어 timestamp가 0이라 앱의 "같은 시각" 판정이 어떻게 동작하는지), 이 폰에서 `CarHardwareLocation`이 실제로 제공되는지, 각 터널 진입 직전에 어느 소스가 선택돼 있었는지, 실제 터널의 차로 수와 도로폭, DHU V3(정확도 20 m, 오프셋 120 m)가 정상이었던 이유(Opus의 이탈 규칙으로는 설명되지 않고 정확도 구간 규칙으로는 설명됨), 한글 표시 문구(리소스 미디컴파일).
