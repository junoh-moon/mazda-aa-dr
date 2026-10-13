# T map 11.8.4.4075 가상 GPS(eVirtualGps) 심층 분석 (2026-10-13)

범위: 소유자가 AGENTS.md에 허용한 T map 앱(`com.skt.tmap.ku`, 번들 네이티브 라이브러리 포함)만 다룹니다. 입력은 `validation/TMAP_APP_STATIC_RE_2026-10-13.md`와 같은 번들입니다. Java는 기존 jadx 출력(`tools/tmap-jadx`, git 제외)을 읽었고, 네이티브는 arm64 `libNavigationSDK.so`(`split_config.arm64_v8a.apk`에서 추출한 것, armeabi-v7a 분할은 분석하지 않음)를 Ghidra 12.1.4 headless로 분석 없이(-noanalysis, image base 0) 가져와 필요한 함수 약 40개만 디컴파일했습니다. PLT 호출 대상은 `.rela.plt` 순서로 이름을 붙였고 스텁의 GOT 주소를 디코드해 대조했습니다. 호출자 탐색은 `.text` 전체의 BL/B 명령과 재배치 항목을 스캔했습니다. 서버, 폰, 차와는 상호작용하지 않았습니다. 디컴파일·디스어셈블 결과물은 저장소에 두지 않으며, 아래에는 함수 이름, 주소, 상수, 동작을 말로만 적습니다. 주소는 arm64 파일 오프셋입니다.

표기: FACT = 코드·상수·문자열에서 직접 확인, DATA = 바이너리 데이터를 계산해 얻은 값, INFERENCE = 해석입니다.

## 1. Java 쪽: 앱은 가상 GPS를 켜고 끄지 않습니다

- (FACT) `RGData.eVirtualGps`는 엔진이 내보내는 값뿐입니다(`RGConstant.VirtualGpsType`: REAL 0, TUNNEL 1, UNDERPASS 2). 앱에는 이 값을 켜는 JNI가 없습니다. 엔진은 `TmapNavigation.createInstanceWithDR(..., isSupportDR=false)`로 만들어집니다(`VSMUtil`). 숨은 설정을 켰을 때만 `TmapNavigationEngine`("Protobuf", `libtmapnavigationengine.so`)이 쓰이며, 이 엔진도 내부에 `TmapNavigation`을 감쌉니다.
- (FACT) 엔진 입력은 `InputLocationData(정확도, 고도, 방위, elapsedRealtimeNanos, 위도, 경도, provider, 속도 m/s, 앱 수신 시각 ms, 위성 수, -1, activityType, isDR)` 하나입니다. 값이 없으면 -1을 넣고, 방위 0.0은 359.9로 바꿉니다. 마지막 실측 이후에는 `TmapLocationManager.dummyLocationSender`가 provider "dummy"(마지막 실측 좌표, 정확도·속도·방위 없음)를 넣습니다. 첫 번째는 2 s 뒤에 오고, 그다음은 직전이 더미였거나 음영구역+약신호이면 1 s마다 옵니다(`m.scheduleDummyLocation`).
- (FACT) 엔진 결과의 쓰임새는 다음과 같습니다. `NavigationManager`는 eVirtualGps가 1이면 GPS 상태 TUNNEL(3), 2이면 UNDERPASS(4)로 정합니다. eVirtualGps가 0이 아니면 fused 정확도가 100 m를 넘는 위치를 버립니다. `ShadeAreaCheckerForSDK`는 eVirtualGps가 1 또는 2이면 음영구역을 설정하고, 위성 수가 0보다 크면 해제합니다. 주기 재탐색은 서비스 구역 안이면서 eVirtualGps가 0이 아닐 때 `ArrivalTimeRecalculate`로 바뀝니다. 터널 링크에서 주기 재탐색을 요청하면 출발 링크가 현재 터널 링크(mesh/link/direction)로 고정됩니다. SAFE_DRIVE 칼만 필터에는 eVirtualGps 또는 터널 링크 여부가 전달됩니다.
- (FACT) 안드로이드 오토의 영향: 기압 ML 분석기(`location.n.g`)만 AA 연결 시(`car.data.b.g...c()`) null을 돌려줍니다. `n.a.a(context)`는 AA 판정이 아니라 "tunnel config available" 설정값입니다. 더미 공급, 엔진 가상 GPS, eVirtualGps 소비 경로에는 AA 분기가 없습니다. 따라서 AA에서도 엔진 가상 GPS는 그대로 동작합니다(INFERENCE, 높음). ML 분석기는 비 AA에서 provider "tlp", 정확도 10 m, 엔진 `GetAutoDrivePosition(속도)` 좌표를 넣습니다.

## 2. 네이티브 입력 처리 (FACT)

경로: `CNaviController::InputLocationData`(0xeb038) → `CNaviModule::API_InputLocation`(0xf32b0) → `CNaviModule::ReceiveGps`(0xf3300) → `CNaviModule::MapMatch`(0xf7c40) → `NcMapMatch::ICalcMapMatching`(0x1095cc).

- 속도는 m/s × 3.6으로 km/h 정수가 되고, 좌표 (-1, -1)은 (0, 0)이 됩니다.
- `getProviderType`(0xff224): fused 2, dummy 1, gps 3, tlp 4, fake_gps 5, dr_gps 6, 그 밖 0.
- 경로에 정합된 상태(내부 상태 2)일 때마다 마지막 정합 위치·각도·속도를 저장합니다. 더미의 좌표는 이 정합 위치로 바뀝니다. 방위가 0 초과 360 이하가 아니면 정합 각도를, 속도가 음수이면 정합 속도를 씁니다(속도 있음 플래그는 0). activityType이 3(STILL)이고 속도가 2 km/h 미만이면 방위를 버립니다.
- 유효(상태 5) 조건은 더미가 아니고 정확도가 0 이상 500 m 미만인 것입니다. 정확도가 NaN이어도 통과합니다. 그 밖은 무효(4)입니다.

## 3. 가상 GPS 알고리즘 (FACT, 의미 해석은 INFERENCE로 표시)

**진입 구역** `NcMapMatch::CheckTunnelLink`(0x109934): 다음 중 하나이면 구역 안입니다. 현재 링크 시설 코드가 {2, 4, 5}인 경우, 또는 "전방 음영 링크" 값이 {2, 4, 5}이면서 정합 데이터의 거리 필드가 100 미만인 경우입니다. 직전 에포크의 정합 결과에도 같은 검사를 합니다. 전방 값은 `NcRouteMatchEX::IsNextLinkTunnel`(0x10e7e0)이 채웁니다. 경로를 따라 누적 길이 100 이내에 시설 {2, 4, 5} 링크가 있고, 도중에 링크 각도 차가 50°를 넘지 않으면 2가 됩니다. 종류 플래그는 값이 2이면 1(TUNNEL), 4이면 2(UNDERPASS)이고, 5이면 0입니다(움직임은 하되 표시하지 않음). Java `LinkType`은 0 일반, 1 교량, 2 터널, 3 고가, 4 지하차도입니다. 5의 의미는 미확인입니다.

**가상 전환** `ICalcMapMatching`: 구역 안에서 다음 중 하나이면 해당 입력 대신 가상 위치를 만듭니다.
1. 입력이 무효
2. 유효지만 정확도 > 33.0 m
3. provider가 dummy

정확도 ≤ 33 m인 실측(dummy 제외)이 오면 즉시 실측으로 돌아가고, 종류 플래그를 0으로 지웁니다. 단, provider가 tlp이면 플래그를 유지합니다. 혼합(blend)이나 점진 전환은 없습니다. 구역 밖에서 무효 입력이 오면 위치를 움직이지 않습니다. 이때는 마지막 항목만 다시 정합합니다.

**속도 모델**: 구역에 들어간 동안 유지속도 = max(유지속도, 입력 속도)입니다. 구역 안의 좋은 실측마다 (max + 실측)/2로 갱신하고, 구역 밖에서는 0으로 초기화합니다. 가상 단계에서는 입력에 0이 아닌 속도와 속도 있음 플래그가 있으면 그 속도를, 없으면 유지속도를 씁니다. 그다음 `NcMapMatch::MakeVirtualGPS`(0x109a14)가 경로 링크 레코드(stride 0xd8)에서 목표 속도를 구합니다. 목표 속도는 정수 필드 +8 / +4 × 3.6이고, ushort +0x3a가 0이 아니면 그 값으로 상한을 둡니다. INFERENCE: 길이/소요시간 = 링크 소통 속도, 상한 = 제한속도입니다. 현재 속도가 목표보다 낮으면 한 단계에 최대 10 km/h씩 올리고 그 값을 유지속도에 저장합니다. **내리는 경로는 없습니다.** 감쇠, 하한, 상한(목표 외)도 없습니다. INFERENCE: 진입 직전에 정지했다면 유지속도 0에서 시작해 초당 10 km/h씩 링크 속도까지 가속하므로, 정체 터널에서 실제보다 앞서갑니다.

**거리와 시간**: 한 입력당 dt는 **고정 1000 ms**입니다(`ICalcMapMatching`이 상수 1000을 넘김). 실제 경과 시간은 쓰지 않습니다. 이동 거리는 (km/h ÷ 3.6) × 1 s이고, `GetVirtualPoint`(0x10a03c)가 반경 6,371,000 m 대원 공식(상수 1.5696123e-7 = 1/6371000)으로 적용합니다. INFERENCE: 그래서 앱의 1 s 더미 주기가 사실상 속도 척도입니다. 첫 공백 2 s처럼 주기가 늘면 그만큼 뒤처집니다.

**방위 모델**: 경로 안내 중이고 경로 링크 목록이 있으면, 마지막 정합 위치에서 "현재 링크 시작 정점 + 정점 오프셋 + 1" 정점을 향한 초기 방위를 씁니다. 그 정점이 10 m 안이면 다음 정점을 씁니다. 그렇지 않으면 마지막 각도를 그대로 둡니다(직선 외삽). 자이로, 나침반, 차 방위는 쓰지 않습니다. 매 단계 뒤 일반 정합(`Matching`)이 다시 정합 위치를 갱신하므로, 다음 단계는 도로에 붙은 위치에서 출발합니다(INFERENCE: 사실상 경로 폴리라인 추종).

**불확도**: 가상 항목은 직전 항목을 복사하고 시각 +1 s, 가상 표시 1을 붙입니다. 정확도 필드는 현재 입력값(더미면 -1)이며, 증가하는 오차 예산은 없습니다.

**상한과 종료**: 시간·거리 상한은 확인한 함수들 안에 없습니다. 구역 판정이 사실상 상한입니다. 가상 위치가 마지막 음영 링크를 지나 일반 링크에 정합되고 전방 100 m 안에 음영 링크가 없으면 구역 밖이 되어, 위치가 멈춘 채 실측을 기다립니다(INFERENCE: "출구 정지").

**정합·재탐색과의 관계**: `MapMatch`는 가상 플래그가 0이 아니면 정합 상태를 3으로 강제합니다. `CRGPlayer::GetDriveInfo`가 정합 데이터 +0x7c(= 플래그)를 RGDriveInfo +4로 복사하고, 변환기(0xe1b14)가 이를 Java `eVirtualGps`에 씁니다. 경로 정합기(`NcRouteMatchEX::MapMatching` 0x10ebb0)는 실패 계수가 3을 넘으면 상태 4로 갑니다. INFERENCE: 3은 보류, 4는 이탈입니다. 따라서 가상 구간에서는 이탈과 재탐색이 억제됩니다. Java 재탐색은 eRgStatus 5에서만, 직전 재탐색 후 3 s가 지나야 일어납니다.

**사용되지 않는 것·다른 것**:
- `NcRouteMatchEX::MakeVirtualGPS`(0x10e450, 경로 폴리라인을 정점 단위로 걷는 버전)와 `ForceSetMapMatchData`에는 호출자가 없습니다(BL 스캔과 재배치 모두 없음).
- `MakeVirtualMatchData`/`GetVirtualMatchData`는 직전과 현재 정합점 사이를 화면용으로 보간하는 것이지 DR이 아닙니다.
- `API_GetAutoDrivePosition`(0xfe294)은 ML 분석기용입니다. 정합 위치에서 경로를 따라 max(속도/3.6, 3) m 앞의 점과 방위를 돌려줍니다.
- (DATA) `SetTennelInfo`/`TennelMatching`은 내장 표를 씁니다. 폴리라인 325개, 정점 1,198개이고, 좌표 ÷ 524288이 도(degree)입니다. 모두 경도 126.997~127.073, 위도 37.537~37.607(서울 도심) 안에 있습니다. 반경 ±400 m 상자로 찾고, 점수가 66 이상이면 `GetLinkRoadCost`의 비용을 1.25배로 합니다. 용도는 INFERENCE: 특정 지하·터널 구간의 정합 보조입니다.

## 4. 비교

| 항목 | T map 엔진 가상 GPS | 네이버 VirtualDrive (`NAVER_MAP_ROUND2_2026-10-12.md`) | 이 제품 터널 프로파일 |
|---|---|---|---|
| 진입 | 터널·지하차도 링크 위 또는 100 m 전방 + (무효, 정확도 > 33 m, 더미) | 새 위치 없음 틱 → 시작, 터널 링크면 계속 | 차 GPS 상실(mode 0), 정책 M 진입 |
| 속도 | 유지 최대속도, 링크 속도까지 +10 km/h/s로 증가, 감속 없음 | 마지막 속도, 최소 5 km/h | 바퀴 펄스 실측(정지 = 0) |
| 방위 | 다음 경로 정점 방향(지도 필요) | 경로 링크 방향 | 요 적분 차체 방위 |
| 시간 | 입력당 1 s 고정 | 1000 ms 틱 | 실제 시각 |
| 불확도 | 없음 | 회색 카운터 | 정직 예산 기록, 보고 상한 25 m |
| 종료 | ≤ 33 m 실측 즉시, 혼합 없음; 구역 끝에서 정지 | 유효 GPS 복귀 | GPS 복귀(mode ≠ 0) |

## 5. 제품에 주는 함의

(FACT, 앞 기록) T map은 헤드유닛 LOCATION을 읽지 않으므로, 아래 내용은 T map의 반응이 아니라 설계 참고입니다.
1. **복귀 품질 게이트(가장 유력)**: T map은 정확도 33 m 이하만 "실측 복귀"로 인정하고, 네이버도 30 m 미만만 유효로 봅니다. 우리 터널 치환은 mode ≠ 0이면 끝나므로, 복귀 직후 정확도가 나쁜 순정 위치가 네이버에 그대로 갈 수 있습니다. 순정 정확도가 약 30 m를 넘는 동안 짧게 상한을 두고 DR 치환을 유지하는 안을 검토할 만합니다. 근거는 두 앱의 문턱입니다. 위험: 비 LOCATION 전달 계약 변경(BETA 예외), 복귀 직후 차 GPS 정확도 분포는 미측정입니다(기존 개인 기록에서 오프라인 계산 가능).
2. **1 Hz 간격 엄수**: T map(1 s 고정 단계)과 네이버(1000 ms 틱) 모두 주기에 의존합니다. 송신 간격을 1 Hz로 유지하는 현재 설계를 지키고, 공백을 만들지 않는 것이 중요합니다(위험 낮음).
3. **채택하지 말 것**: 링크 속도로 가속하거나 감속하지 않는 속도 모델, 정지 무시, 오차 예산 없음은 실측 바퀴 속도와 요를 가진 이 제품보다 못합니다. 경로 정점 추종 방위는 지도가 필요하므로 CMU 쪽 구현은 범위 밖입니다.

## 미해결

링크 레코드 +4/+8/+0x3a 필드의 정확한 의미, 시설 코드 5, 정합 데이터 거리 필드(+0x58)의 단위, 상태 3/4의 공식 의미, Protobuf 엔진의 동작, 원격설정이 네이티브 경로에 영향을 주는지(확인한 함수에는 설정 읽기 없음), 실제 터널에서의 동작(실행 검증 없음)이 남아 있습니다. 확인 대상은 11.8.4.4075의 arm64 라이브러리뿐입니다(armeabi-v7a 미확인). Ghidra 프로젝트는 분석 후 삭제했습니다. 작업 메모는 `$CLAUDE_JOB_DIR/tmp/tvg.QUkv48/NOTES_tvg.md`에 있습니다(저장소 밖).
