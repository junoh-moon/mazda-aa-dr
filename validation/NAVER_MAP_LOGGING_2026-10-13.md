# 네이버 지도 자체 판단을 폰에서 기록할 수 있는가: 정적 분석 (2026-10-13)

소유자가 AGENTS.md에서 허용한 네이버 지도 역공학 범위입니다(오프라인·로컬, 폰과 앱은 바꾸지 않음). 분석 대상은 6.8.0.5(versionCode 60800007, 이전 기록과 같은 번들)이고, 설치된 6.10.0.16은 분석하지 않았습니다. jadx 1.5.6으로 필요한 클래스만 `--single-class`로 디컴파일했고 classes5는 통째로 풀었습니다. 정의 위치는 dexdump 헤더로 찾았습니다. APK, 디컴파일 코드, 덤프는 저장소에 두지 않았습니다. 폰, 차, DHU에서 실행한 것은 없습니다. 배경은 [1차](NAVER_MAP_APP_STATIC_RE_2026-10-12.md), [2차](NAVER_MAP_ROUND2_2026-10-12.md), [출구 점프](EXIT_JUMP_BLEND_2026-10-13.md) §0을 보십시오.

표기: **FACT** = 클래스, 메서드, 상수, 문자열. **INFERENCE** = 추론. **DOC** = 공개 문서 지식(이번에 다시 확인하지 않음).

## 1. NaviBuildConfig (FACT)

- 정의 위치는 `classes.dex`의 `com.naver.maps.navi.buildconfig.internal.NaviBuildConfig`입니다. **`p()`는 상수 `false`를 돌려줍니다.** 원격 설정, 개발자 토글, 인텐트, 시스템 속성, 파일 어디에서도 값을 읽지 않으므로 실행 중에 바꿀 수 없습니다. 같은 클래스의 `k()`, `l()`, `o()`, `s()`, `t()`도 상수 false이고 `r()`만 true입니다. `c()`는 `BuildConfigExtensions_androidKt.a()`(classes3)를 부르는데, 이 함수도 상수 false를 돌려줍니다. 디버그 빌드인지 판정하는 함수로 보입니다. `InternalBuildConfig`(classes3)의 불리언 상수도 하나(k=true)를 빼면 모두 false입니다. 버전 문자열은 "2.17.7", 빌드일은 "2026-07-01"입니다.
- 따라서 `MMLogger.a()`(debug)와 `d()`(verbose)는 릴리스에서 **절대 출력되지 않습니다**. 앱을 고치지 않는 한 방법이 없습니다.

## 2. NaviLogger와 출력 대상 (FACT)

- `NaviLogger`(classes.dex)의 난독화 메서드 대응은 e=debug, f=error, g=info, h=verbose, j=warn입니다. 그러므로 `MMLogger`는 b=error, c=info, e=warn(게이트 없음), a=debug, d=verbose(`p()` 게이트)입니다. `NaviLogger`는 `LogUtils.getNaviLogger()`에 넘기기만 합니다. 기본 로거는 아무것도 하지 않는 `EmptyLogger`이고, `NaverNaviInnerControl` 생성자가 `InternalNaviLogger`로 바꿉니다.
- `InternalNaviLogger.r()`(classes5)이 출력 대상을 정합니다.
  - `TimberLogger`(logcat)는 `NaviBuildConfig.c()`가 true일 때만 붙으므로 릴리스에서는 **없습니다**.
  - `ConsoleLogger`는 `k()`가 true일 때만 붙고(릴리스는 false), 붙더라도 메서드가 비어 있습니다.
  - `FileLogger`는 **`NaviConfig.p()`가 true일 때만** 붙습니다. 이 값은 아래 §3의 설정 파일에서 `trace=1`로 켭니다.
  - Nelo 로거 3개는 항상 붙고, `c()`가 false인 릴리스에서는 **error 수준만** 원격 서버(`kr-col-ext.nelo.navercorp.com`)로 보냅니다. `NaviSdkNeloLogger`는 NET_WORKING을 뺀 모든 태그, `NaverMapApiNeloLogger`는 NET_WORKING만, `KsaNaviSdkNeloLogger`는 NETWORK_META만 보냅니다.
  - 태그와 문자열 필터(`include_tag`, `exclude_tag`, `keyword`)는 비어 있으면 모두 통과시킵니다.
- **결론: 릴리스에서 내비 SDK가 logcat에 쓰는 것은 없습니다.** READ_LOGS를 쓰는 녹화 앱으로는 이 판단들을 얻을 수 없습니다. 출력 경로는 Nelo(error만, 네이버 서버로 감)와, 설정 파일로 켜는 `FileLogger`뿐입니다.
- `FileLogger`가 쓰는 파일은 `<외부 앱 파일 폴더>/NaverNavi/trace/trace_<현지시각>.log`이고, 한 줄은 "시각 pid-tid 태그 [V/D/I/W/E] 메시지" 형식입니다. 쓰기는 `BufferSyncFileWriter`가 맡습니다. 30줄을 모으면 **호출한 스레드에서 잠금을 잡은 채** 동기적으로 이어 씁니다. 나머지는 `release()`(내비 종료) 때 기록되며, 프로세스가 죽으면 최대 29줄을 잃습니다. 파일을 돌려 쓰거나 크기를 제한하는 장치는 없습니다.

## 3. 숨은 설정 파일 `navi.config` (FACT)

- `NaviModule.s()`는 외부 앱 파일 폴더를 `Context.getExternalFilesDir(null)`로 정합니다(없으면 내부 폴더). 그 폴더의 `navi.config`를 `NaviConfig.q()`가 읽습니다. 한 줄에 `키=값` 하나를 쓰고, `#`으로 시작하는 줄은 무시하며, `=`가 정확히 하나여야 합니다. 값이 정확히 "1"이어야 켜지므로 CRLF 줄끝은 "1\r"이 되어 켜지지 않습니다(INFERENCE).
- 키 목록은 `trace`, `debug_ui`, `save_guidance`, `save_pbf`, `native_crash_test`, `include_tag`, `exclude_tag`, `keyword`, `map_matching.local_location_threshold`(기본 20), **`ignore_input_location`**입니다. **`ignore_input_location`은 절대 넣으면 안 됩니다.** `LocationController.update()`가 모든 입력 위치를 버리게 됩니다.
- 파일은 SDK 초기화 때, 그리고 `NaverNaviInnerControl.o()` 안에서 다시 읽습니다. `o()`는 생성자, 경로 안내 시작(`c()`), 안전운전 시작(`f()`), 안내 종료(`i()`)에서 불립니다. 즉 **앱을 다시 설치할 필요 없이 다음 안내 시작부터 적용됩니다**.
- `trace=1`이 하는 일은 기록뿐입니다. `Config.Trace`를 참조하는 곳은 `NaviConfig.p()` 하나이고, `p()`를 부르는 곳은 `InternalNaviLogger.r`과 `SessionLogger.init` 둘뿐입니다. 이 때문에 판정 동작은 바뀌지 않는다고 봅니다. 다만 파일 쓰기가 동기식이라 시간 지연이 조금 생길 수 있습니다(INFERENCE).
- `trace=1`일 때 `SessionLogger`는 `NmeaSessionLogger`도 켭니다. 경로는 `NaverNavi/nmea/log/<시각>_<routeId>.nmea`입니다(`SessionLogger.o`, `AbstractSessionControl`/`SafeSessionControl`에서 안내마다 엽니다). **차와 폰의 입력 `Location` 하나마다** `$NMALC` 한 줄을 씁니다(`NmeaUtils.b`). 내용은 앱 수신 시각(currentTimeMillis), `getTime()`, 위도, 경도, 고도, 정확도, 방위, 속도(없으면 −1), provider(모의 위치면 "mock"), AvnConnect 값입니다. `GuidePointSessionLogger`(`NaverNavi/guidePoint`)도 함께 켜집니다.
- `debug_ui=1`이면 `NaverMapMatchingControl.run`이 `DebugDataHelper`를 통해 `NaverNavi/debug/.DEBUG_DATA.txt`에 맵매칭 디버그 점을 씁니다. `RendererFactory`도 지도 위에 `DebugLayer`와 `RouteLayer`를 그립니다. `DebugDataHelper`를 생성하는 위치는 찾지 못했고, AA 화면에도 그려지는지는 판정하지 못했습니다.
- 앱 안에 이 파일을 만드는 화면은 없습니다. 문자열 "navi.config"는 classes5에만 있고, SDK API `createNaviConfigFile`을 부르는 곳도 없습니다. 앱의 `SettingLabDebugFragment`(classes33)는 피드백 트리거용 디버그 화면이며 위치와는 관련이 없습니다.

## 4. 판단별 기록 (FACT, `trace=1`일 때 파일에 남는지 여부)

| 판단 | 클래스와 메시지 | 수준 | 릴리스 + trace |
|---|---|---|---|
| 차↔폰 전환 | `ValidGpsSelector`: "[MM][GpsSelect][Valid][모드] GPS type changed: A -> B (mobile: m, avn: m…)" | info | 남음 |
| 3회 버퍼 전환 | `SwitchBuffer`: "Switching to X (switchRequestCount/invalidCount ≥ 3)", "First selection", "Recovery from both invalid + stopped" | info | 남음 |
| R2 점프 | `ValidGpsSelector`: "[MM][GpsSelect][Valid] Jump detected: d m in t ms = v km/h" | warn | 남음 |
| R2 결과 | "AVN jumped -> Mobile", `DualGpsSelector` "type changed by Jump", "Buffering switch request" | debug | **안 남음** |
| 무효 쪽 선택 | `InvalidGpsSelector`, `HalfValidGpsSelector` "Prefer invalid…", "Mobile resumed…" | info | 남음 |
| 소스 무효 | `GpsChecker` INVALID_LOCATION, INVALID_DIRECTION_IN_SHADOW | warn | 남음. INVALID_ACCURACY와 INVALID_TIME_DIFF는 따로 남지 않지만 NMEA 줄(정확도, 수신 시각)로 다시 계산할 수 있음 |
| R1 점프 | `GpsAdjuster`: "[MM][Gps] GPS jump detected." | warn | 남음(거리를 담은 "GPS jump check" 줄은 debug라 안 남음) |
| 나쁜 정확도·이상값 | `GpsAdjuster` "not GoodAccuracy (accuracy:…)", "abnormal (speed…)", "minus speed", "speed anomaly", "wasVirtualDriving and new naviLocation is not Good"; `GpsValidator` "Invalid Accuracy / Angle / distance / Timestamp" | warn/info | 남음 |
| 회색(10틱) | `GpsLocationStatusManager`: "[MM][Gps] LocationStatus is INVALID (n)" | warn | 남음(INVALID인 틱마다) |
| 가상주행 | `VirtualDriveNoGpsChecker` "Status -> NoGpsDriving", `VirtualDriveShadowChecker`, `VirtualDrive` "(check) 상태 <= last…", "VIRTUAL_DRIVE_STOP…", `VirtualDriveController` "forwardLocation…", `AvnVirtualDrivingFilter` "forcing virtual driving", "AVN resumed after virtual drive" | info/warn | 남음 |
| 틱 상태 요약 | `LocationBankLogger`: "[MM][Bank] status: {virtualDriveStatus, isNoGpsDriving, isShadowDriving…}" | info | 남음 |
| 매칭 입력 | `RouteMatching`: "(matching)<id> start 좌표, speed, course…" | info | 남음(틱마다) |
| 재탐색 | `RerouteBufferManager` "rerouteCount : n => ignore(b)", `RerouteChecker` "rerouteStatus", `RerouteSilentChecker`, `FixedStatusChecker` | info/warn | 남음 |
| 화면 속도 | 맵매칭 출력 속도를 남기는 줄은 찾지 못함 | — | 판정 못함. "(matching) speed"와 "forwardLocation"으로 근사(INFERENCE) |

## 5. 다른 관측 경로

- 원격 전송: Nelo로 가는 error 줄(`MMLogger.b`는 초기화와 경로 오류 3곳뿐)과 `TripReport.RawGps`(`LocationController.d`, 모의 위치이고 실서버면 생략)는 네이버 서버로 가며 폰에서 읽을 수 없습니다(FACT/INFERENCE). 원시 dex 문자열에서 "GPS 상실" 분석 이벤트 이름(gps_lost 등)은 찾지 못했습니다(FACT, 문자열 검색만 함).
- "로그 보내기" 기능과 위치 개발자 메뉴는 찾지 못했습니다(클래스 이름 검색 기준). 보행 내비에만 `WalkNaviDebug*` 화면이 있습니다(FACT).
- AA 화면의 회색 아이콘(`LocationStatusControl`)은 기록되지 않습니다. 대신 위 표의 INVALID 줄이 같은 사실을 담습니다(INFERENCE).

## 6. 폰에서 수집할 수 있는가

- logcat 경로(READ_LOGS를 `adb shell pm grant`로 1회 부여): 안드로이드 13부터는 앱이 logcat을 열 때마다 사용자 동의 대화상자가 뜹니다(DOC: Android 13 behaviour changes). 그보다 근본적으로 **네이버가 logcat에 쓰지 않으므로 소용이 없습니다**(FACT, §2).
- 파일 경로: `/sdcard/Android/data/com.nhn.android.nmap/files/`에 `trace=1`(LF 줄끝) 한 줄짜리 `navi.config`를 `adb push`로 한 번 넣습니다. 주행이 끝나면 `NaverNavi/trace`와 `NaverNavi/nmea`를 `adb pull`합니다. 녹화 앱도 앱 수정도 필요 없습니다. adb 셸이 안드로이드 15/16 삼성 기기에서 `Android/data`를 읽고 쓸 수 있는지는 확인하지 못했습니다(INFERENCE: 11~14에서는 된다는 보고가 일반적임). 다른 앱이 이 폴더를 읽는 것은 안드로이드 11부터 막혀 있습니다(DOC).
- **주의: 이 방법은 개발자용 디버그 스위치를 켜는 것입니다.** 앱 바이너리나 시스템은 바꾸지 않지만 앱 데이터 폴더에 파일을 넣으므로 "일반 설정"의 범위를 넘습니다. 그래서 과제 조건상 소유자가 따로 승인해야 합니다. 이 스위치는 문서화되지 않았고 6.10.0.16에서 바뀌었을 수 있습니다.
- 위험(INFERENCE): 파일에는 정밀 좌표, 시각, 기기 모델이 담깁니다. private-data에만 보관하고 저장소, 이슈, 공유 링크에는 원문을 올리지 않으며, pull한 뒤에는 폰에서 지웁니다. 크기는 대략 시간당 수~수십 MB로 추정하며 돌려 쓰지 않습니다. 동기식 쓰기로 맵매칭 스레드가 아주 짧게 지연될 수 있습니다. 실수로 `ignore_input_location`을 넣으면 내비가 멈춥니다.
- 노력(INFERENCE): 한 번 설정하는 데 adb 명령 1개, 주행 뒤 pull 2개가 듭니다. 분석 스크립트는 NMEA 입력과 상태 줄을 CMU 저널과 시각으로 맞춥니다. 이렇게 하면 "25 m 상한 뒤에 차 쪽이 유효했는지, 폰으로 전환됐는지, INVALID와 가상주행이 몇 틱이었는지"를 답할 수 있고, AA 호스트가 `getTime()`을 어떻게 채우는지도 직접 보입니다.

## 7. 판정과 첫 단계

- **READ_LOGS 녹화 앱으로는 안 됩니다.** `navi.config` `trace=1`이라는 파일 하나로는 됩니다. 이때 debug 수준인 "AVN jumped -> Mobile" 문구 자체는 남지 않지만, 같은 사실이 "Jump detected"와 "GPS type changed" 줄에 남습니다. 남지 않는 것은 debug와 verbose 줄 전부와 화면 속도 값입니다.
- 첫 단계(무비용, 아무것도 바꾸지 않음, 소유자 승인 후): 폰을 PC에 연결하고 `adb shell ls /sdcard/Android/data/com.nhn.android.nmap/files`를 실행합니다. 이것으로 접근할 수 있는지와 `NaverNavi` 폴더가 있는지 확인합니다. 그다음 단계(파일을 넣고, 주차한 채 안내를 1분 시작했다가 종료하고, pull)는 폰을 바꾸는 행동이므로 소유자가 결정합니다.

## 판정하지 못한 것

6.10.0.16에서도 같은지, adb가 Android/data에 접근할 수 있는지, 실제 파일 크기, `DebugDataHelper` 생성 여부와 AA 화면에 그려지는지, 화면 속도 로그, Nelo로 무엇이 전송되는지(error 줄 외).
