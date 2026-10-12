# LIVI 시험 헤드유닛(rig) 계획: 차 없이 네이버의 터널 증상을 Mazda 형태로 재현하기 — 2026-10-12

**상태: 계획만 있습니다. rig는 Mac·폰·Electron에서 한 번도 실행되지 않았습니다.** 오프라인 검증은
`tools/livi-patch/TEST_RESULTS.txt`(Linux, Rust·driver 시험)와 `tests/tools/test_livi_rig_plan.py`뿐입니다. 이 계획은 차량
방문·주행·ASSIST 활성화·배포를 승인하지 않습니다.

**한 줄 요약.** DHU 대신 패치한 f-io/LIVI(`tools/livi-patch/livi-mazda-test-hu-v2.patch`)를 소유자의 Mac에서 헤드유닛으로
쓰고, Mazda와 같은 센서 구성(위치 특성 256, 센서 1,3,7,8,10,13,21, GPS_SATELLITE, 1 Hz 재송신, 구독하지 않은 센서는 보내지
않음)으로 S25에 시험 11의 입력을 **바이트 그대로** 보내며, 터널 구간 안에서 한 가지씩만 바꿉니다. 처음으로
(1) S25의 Android Auto가 **어떤 센서를 구독하는지**(특히 GPS_SATELLITE type 21), (2) 위성 0/5와 정확도 상한 20 m의
효과, (3) 실제 111초 터널 모양 입력과 순정(mode-0) 재송신에서 속도 고정이 나오는지를 봅니다.

## 1. 질문과 가설 이름

목표 이름(G1 터널 속도, G2 주차장 방위, G3 첫 연결 방위)과 **가설 이름은 다릅니다.** 아래 가설 이름은
[DIAG_BUILD_A20_S5](../DIAG_BUILD_A20_S5_2026-10-12.md)의 사전 등록 절을 따릅니다.

| 가설 | 내용 | 출처 |
| --- | --- | --- |
| H-G1 | 지도 맞춤 잔차(옆 오차)가 보고 정확도보다 크면 회색 | DIAG 사전 등록(INFERENCE), DHU V4 3/3 |
| H-G2 (= S) | 위성 0 × RAW_GNSS_ONLY가 일정 시간 뒤 비-GNSS 취급 | DIAG, HUIG v1.3.0 인용(공개 캐시본) |
| H-G3 | 40 m 상한의 장기 유지 | DIAG (DHU V2는 정상이라 약함) |
| H-G4 | 송신 공백 또는 첫 무정확도 메시지로 폰 위치로 전환 | DIAG, [로그 감사](../LOG_AUDIT_TUNNELS_2026-10-12.md)(공백 1.5 s 초과 없음) |
| H-P | 폰/네이버 자체 DR 수명(일정 시간 뒤 멈춤) | DIAG 보충 2(COMMUNITY 일화, INFERENCE) |
| H-F1 | 네이버 자체 GPS 상실 모드(우리 fix를 버리고 마지막 속도로 외삽) | DIAG(COMMUNITY 2022 클리앙, INFERENCE) |
| H-W | 무선 AA 동글 경로가 다름 | rig도 유선이라 **시험 불가** |

## 2. rig가 DHU·차와 다른 점(재현할 수 없는 것)

| 항목 | 차(74.00.324A) | DHU(시험 11) | rig(이 계획) |
| --- | --- | --- | --- |
| 전송 | 무선 AA 동글 | USB(ADB 포워딩, 헤드유닛 서버) | USB 유선 AOAP. **동글(H-W)은 재현 못 함** |
| 위치 특성 | 256(DESIGN_V1 §4.5 분기 추정) | 256(v4 설정, 기본값과 같음) | 256 |
| 광고 센서 | 1,3,7,8,10,13,21 ("7 sensors", 로그 감사) | location, driving_status | 1,3,7,8,10,13,21 |
| 구독 안 된 센서 | 캐시만, 송신 없음(로그 감사의 펌웨어 판독) | 해당 없음 | 같은 동작(P9). 미광고 type 요청은 -9(P7) |
| GPS_SATELLITE | LOCATION과 1:1, 터널 0 / fix 5 | 보낼 수 없음 | 매 push 1회, LOCATION 앞(구독된 경우만) |
| SPEED(type 3) | 바퀴 속도 약 10 Hz | 없음 | 1 Hz, 값이 바뀔 때만(LIVI dedupe). **10 Hz는 재현 못 함** |
| LOCATION timestamp | 0 | DHU 형식 | 필드 없음(LIVI proto) |
| 메시지 순서·묶음 | Mazda aap_service의 순서 | DHU | LIVI 순서(위성 → 위치). **실제 Mazda wire 순서는 재현 못 함** |
| 헤드유닛 신원·인증서·프로토콜 버전 | Mazda, 오래된 GAL | Google DHU 2.0 | LIVI(upstream 신원·인증서, AA 17.6 분석 기반). **재현 못 함** |
| 화면 | 800x480 | 800x480 dpi 160 | 800x480 dpi 160 fps 30으로 설정(아래) |
| 폰 자체 GNSS | 터널에서 실제로 잃음 | 실내/책상(합성 위치와 다름) | DHU와 같음(H-P 일부를 재현 못 함) |

**양성 결과가 뜻하는 것**: 어떤 회차가 실차의 회색 또는 속도 고정을 보이면 그 현상은 동글·실제 터널·실제 Mazda wire
없이도 생긴다는 뜻이고, 그 회차의 입력 차이가 후보가 됩니다. **음성 결과의 한계**: 재현되지 않아도 H-W(동글), 폰 GNSS의
실제 상태, Mazda의 실제 순서·10 Hz SPEED·신원 차이가 남으므로 그 입력이 무해하다는 증거가 아닙니다. 모든 결과는 이 폰
(S25)·이 네이버 버전·이 경로(부산 백양터널 합성 경로)에 대한 관찰입니다.

## 3. 파일

| 파일 | 내용 |
| --- | --- |
| `scenarios.json` | 운전기(driver) 시나리오 R0-r1…R5-r1과 반복 R0-r2·R1-r2·R2-r2·R4-r2. 기반 파일, 손실 구간 `loss_t`, 바뀌는 값 |
| `commands/R4-r1.txt` | R4 입력(시험 11 접근·복귀와 같고 터널 111초만 새로 만듦, DHU 문법) |
| `commands/T0-MOVE.txt` | Tier 0 움직임 확인: 시험 11 `V4-r1.txt`의 처음 20초(사진 태그만 바꿈) |
| `rig-run-plan.json` | 회차별 사진마다 보낸 값(위성, 정확도, LOCATION 속도, SPEED 속도, 형태 fix/beta/stock_mode0), 속도 점검 τ, 세션 구성, LIVI 환경변수·설정 |
| `gen_rig.py` | 위 파일 생성기(시험 11·7 생성기를 불러 씀). 시험: `python3 tests/tools/test_livi_rig_plan.py` |
| `OWNER_SETUP_KO.md` | 소유자가 할 일 |
| 기반 입력(바이트 그대로) | `../DHU_NAVER_EXP11_PLAN_2026-10-11/commands/V4-r1.txt`, `V2-r1.txt`, `V4T60-r1.txt`, `prep/` 9개 |
| rig 코드(git 제외 경로) | `tools/livi-patch/`(패치 v2, `driver/`, README, TEST_RESULTS). 실행 에이전트가 받는 방법은 9절 |

**R4 입력(합성, INFERENCE).** 비공개 초별 기록은 복사하지 않고 공개 기록의 수치에 맞췄습니다: 111초, 속도 13/42/61 km/h
(공개값 13/41/61), 20 km/h 미만 19초(τ 69~100; 공개값 +68~101 s에 18회), 정지 없음, 바퀴 거리 1199 m(공개 1143 m),
끝 옆 오차 왼쪽 118.5 m(공개 118.5 m), 방위 오차 선형 −0.131°/s로 끝 −14.4°(공개 −16~−22°: 한 매개변수 모형이라 옆 오차를
맞추면 방위 오차가 작게 나옴), 첫 1초 순정 mode-0 메시지 뒤 정확도 30 → 34 → 37 → 40 m(τ=4에 상한, 공개 +4.0 s), 위성 0.
출처: [G1 기록](../G1_FIRST_TUNNELS_2026-10-11.md) (b) 열, [로그 감사](../LOG_AUDIT_TUNNELS_2026-10-12.md) 표.

## 4. rig 준비(Mac, 회기 전 1회, 25분 회기에 포함하지 않음)

LIVI의 패치 코드는 모두 `livi-core` 실행 파일 안에 있습니다(`livi-aa-stack`이 여기에 링크됨). 두 경로 중 A를 먼저 씁니다.
자세한 명령은 `tools/livi-patch/README.md` "Build and run on the Mac"입니다.

- **경로 A(권장, 관리자 암호 불필요, 약 15~25분):** upstream이 같은 커밋 de03c81로 만든 `LIVI-9.4.0-macos-arm64.dmg`
  (CI run 38171186518 artifact, **2026-10-18T20:19Z 만료**; 2026-10-12에는 `nightly` 릴리스도 같은 커밋)를 받아
  `~/livi-rig/LIVI.app`으로 복사 → 포크를 de03c81에서 패치 v2로 `cargo build --release -p livi-core`(rustup이 고정 툴체인
  beta-2026-10-03 설치, OpenSSL을 소스로 빌드하므로 Xcode Command Line Tools 필요) → `Contents/Resources/core/livi-core`
  교체 → ad-hoc 재서명 → quarantine 속성 제거. 재서명 방법은 추론이며 실행되지 않았습니다(Tier 0이 확인).
- **경로 B(대안, 약 40~60분):** upstream 전체 빌드(`pnpm run build:mac:arm64`). `gst-video-host`·비디오 애드온이
  GStreamer.framework 1.28.7(runtime+devel pkg, `sudo installer`)와 `pkg-config`를 요구합니다. **관리자 암호가 필요하므로
  소유자 손이 듭니다.** 경로 A가 실패할 때만 소유자에게 알리고 씁니다.
- **LIVI 설정:** 처음 실행 전에 `~/Library/Application Support/LIVI/config.json`을 `config.json.before-rig`로 복사해 두고,
  `projectionWidth 800, projectionHeight 480, projectionDpi 160, projectionFps 30`을 넣습니다(DHU `dhu.ini`와 같은 화면이라
  준비 절차의 탭 좌표가 그대로 맞습니다. 800x480 tier에는 inset이 없음: `config.rs`).
- **실행(매번 터미널에서, 환경변수는 `rig-run-plan.json` `livi_env`):**
  `LIVI_LOC_CHARACTERIZATION=256 LIVI_SENSORS=1,3,7,8,10,13,21 LIVI_GPS_SATELLITES=1 LIVI_RESEND_FIXES=1
  LIVI_REJECT_UNOFFERED_SENSORS=1 LIVI_SEND_ONLY_REQUESTED_SENSORS=1 LIVI_TEST_TAPS=1
  LIVI_LOG_SENSOR_REQUESTS=<회차 폴더>/sensor-log.jsonl ~/livi-rig/LIVI.app/Contents/MacOS/LIVI > <회차 폴더>/livi.log 2>&1 &`.
  `open LIVI.app`은 환경변수를 넘기지 않습니다. `livi.log`의 `[overrides]` 줄에 8개 값이 모두 보여야 합니다.
- **driver(Node ≥ 22, 패키지 없음):** 준비 파일은 `node tools/livi-patch/driver/livi-driver.mjs --mazda --tag <TAG> --shots
  <사진 폴더> --events <회차 폴더>/events.jsonl <prep 파일>`, 본문은 `... --scenario validation/LIVI_RIG_PLAN_2026-10-12/scenarios.json
  <TAG> --shots ... --events ...`. 사진은 `LIVI_WINDOW_ID=$(swift tools/livi-patch/driver/livi-window-id.swift)`로
  `screencapture -x -o -l <id>`를 씁니다. 창 사진에 지도 영상이 비면(LIVI는 macOS에서 영상을 창에 직접 그림) `--shot-cmd
  "screencapture -x -R$(swift .../livi-window-id.swift --bounds) {path}"`로 바꿉니다.
- **폰 쪽 기록(읽기 전용):** `adb logcat -d`(지우는 `-c` 금지)와 `adb shell dumpsys activity service
  com.google.android.projection.gearhead`에서 `characteriz|RAW_GPS|LocationCharacter|satellite|SensorStart|sensor` 줄.
  AOAP 연결 중 adb가 안 보이면 그 사실만 적고(ADB-UNAVAILABLE) 진행합니다.

## 5. Tier 0(연기 시험, 회기 1에서 약 7~9분, 회기 2에서는 짧게 4~5분)

1. 환경 기록: `env.txt`(macOS·Node·rustc·LIVI 경로, 패치 sha256, 경로 A/B, artifact 이름·sha256), `versions.csv`(네이버·AA·Google
   Play 서비스 버전, `dumpsys package`의 versionName·lastUpdateTime). 생성기 일치: `python3 validation/LIVI_RIG_PLAN_2026-10-12/gen_rig.py
   /tmp/rig-check`의 결과가 이 폴더와 같은지 `diff -r`, `python3 tests/tools/test_livi_rig_plan.py`, `node --test
   tools/livi-patch/driver/test/*.test.mjs`(다르거나 실패하면 멈추고 보고).
2. LIVI 시작(위 명령) → `T0-BUILD`: `livi.log`에 `[overrides] Overrides { location_characterization: Some(256), sensors:
   Some([...7개]), gps_satellites: true, resend_fixes: true, sensor_log: Some(...), reject_unoffered: true, only_requested: true,
   test_taps: true }`. 없으면 ABORT.
3. 폰 USB 연결 상태에서 60초 안에 LIVI 창에 AA 화면 → `T0-CONNECT`(사진 `T0-connect.png`). 처음 연결에서 폰에 동의 창이
   뜨면 소유자에게 그 창만 알립니다.
4. 연결 20초 뒤 `sensor-log.jsonl` 판독 → **`SUB-LIST`**: 폰이 요청한 type과 `update_rate`, 응답(accepted/거부)을 표로.
   라벨 `SUB21-YES|NO`, `SUB3-YES|NO`, `SUB1-YES|NO`, 거부된 type 목록 `REJ-<n,...>`, `service_discovery_response`의
   characterization이 256인지. 폰 로그에서 `CHAR-SEEN|OTHER|UNSEEN`(시험 11 v4와 같은 정의). **이 표는 그 자체로 결과이므로
   Tier 0 업로드에 반드시 넣습니다.**
5. 관문 A(시험 11과 같음: `pidof com.nhn.android.nmap`, `mCallState=0`, 겹친 창 없음; DHU의 `controls` 항목 대신 LIVI 창
   사진에 네이버 지도) → 준비(시험 11과 같은 B2 단계, `--mazda --tag T0-MOVE`) → 관문 S·L·G(시험 11 정의 그대로) →
   `commands/T0-MOVE.txt`(`--mazda`, 20초, 사진 0·5·10·15·19초) → **`PASS-MOVE`**: 다섯 사진 모두 파란 화살표·안내·회색 없음이고
   남은 거리 D가 첫 사진보다 마지막 사진에서 0.2~0.35 km 줄어듦(14 m/s × 19초 = 266 m). `T0-TAP`: 준비의 탭이 화면을 바꿨는지
   (관문 S 통과가 증거). `T0-SHOT`: 사진에 지도 영상이 있음(검은 화면이면 `-R` 대안으로 한 번 더).
6. `end-guidance` → LIVI 종료(`kill -TERM`) → 5초 → 같은 명령으로 재시작 → 60초 안에 AA 화면이 폰 조작 없이 돌아오면
   `RESTART-OK`, 아니면 `NO-RESTART`(이후 회차는 LIVI 재시작 없이 안내 종료만 하고 회차마다 `SESSION-CARRIED` 표시).
7. 판정: BUILD·CONNECT·SHOT 중 하나라도 한 번 재시도 뒤 실패 → **ABORT**(진단 묶음 업로드 후 멈춤). MOVE·관문 실패는 한 번 재시도,
   다시 실패하면 ABORT. 통과하면 **바로 Tier 0 업로드**(사진 전부, `tier0/`, `sensor-log.jsonl`, `livi.log`, 폰 로그 발췌,
   `env.txt`, `versions.csv`). 회기 2의 짧은 Tier 0은 1(생성기 확인 제외)·2·3·4·5(준비 + T0-MOVE)만 합니다.

## 6. 회차 절차(모든 R 회차 같음)

1. 앞 회차 안내 종료(`prep/end-guidance.txt`, `--tag <TAG>`, 사진) → `RESTART-OK`이면 LIVI 재시작(회차 폴더의 `sensor-log.jsonl`,
   `livi.log`) → 관문 A.
2. 준비: 시험 11 README "회차 절차" 1과 같은 B2 단계(search → 필요하면 search-again → route-y<줄> → 필요하면 route-retry →
   gate 또는 gate-nopopup), 모두 `--mazda --tag <TAG>`. 관문 S·L·G(D0 3.9~4.7 km).
3. 관문 G 통과 뒤 5초 안에 `node .../livi-driver.mjs --scenario .../scenarios.json <TAG> --shots <사진 폴더> --events
   <회차 폴더>/events.jsonl > <회차 폴더>/pushes.jsonl`. 시작 시각을 `run-log.csv`에.
4. 끝나면 `events.jsonl`에서 push 간격 최대값과 `late_ms` 최대값, 사진 실패 수를 `run-log.csv`에. 간격이 1.5초를 넘은 push가
   있으면 `GAP>1.5s`(H-G4 대조에 필요). `sensor-log.jsonl`에서 손실 구간의 `sensor_batch` type 21 수와 값(aa01020800 = 0,
   aa01020805 = 5), type 3 수를 적습니다(실제로 나간 것의 증거).
5. 제외·실패·시간 규칙은 시험 11과 같습니다: 본문 전 관문 실패는 재시도(최대 3시도), 본문 중 LIVI·driver 오류·전화·알림은 제외,
   회색·속도 고정·안내 사라짐·재탐색은 **관찰 결과**. 회기 시간은 Tier 0 시작부터 25분, 남은 시간이 5분보다 적으면 새 회차를
   시작하지 않음. 같은 원인으로 연속 2회차 FAILED이면 멈춤. 버전이 바뀌면 `VERSION-CHANGED`.
6. `SUB21-NO`이면 R1은 wire상 R0와 같으므로(P9: 구독 안 된 type 21은 나가지 않음) 돌리지 않고 `R1-SKIPPED-NOSUB`로 적습니다.

## 7. 회차와 순서(정보 가치 순, 데이터 전에 고정)

| 회기 | 순서 | 시간 |
| --- | --- | --- |
| 1 | Tier 0(7~9분) → **R0-r1** → **R1-r1** → **R2-r1** → R3-r1(남은 시간 ≥ 5분일 때만) | 9 + 3 × 5 = 24분(+R3 5분) |
| 2 | 짧은 Tier 0(4~5분) → **R0-r2** → **R4-r1** → **R5-r1** → R3-r1(회기 1에서 못 했으면) | 5 + 3 × 5 = 20분(+R3 5분) |
| 3(선택) | 짧은 Tier 0 → R1-r2 → R2-r2 → R4-r2 (`scenarios.json`의 r2: r1과 같은 입력·변수, 사진 이름만 다름) | 5 + 3 × 5 = 20분 |

회차당 5분 = DHU 실측 4.5분 + LIVI 재시작 약 0.5분(추정). 회기 2의 첫 회차 R0-r2는 rig가 회기 사이에 바뀌지 않았는지 보는 반복입니다.

| 회차 | 기반 입력(바이트 그대로) | 손실 구간(초) | 바꾸는 것 | 묻는 것 |
| --- | --- | --- | --- | --- |
| R0 | 시험 11 `V4-r1.txt`(정확도 30→40 m 램프, 옆 오차 119 m까지, 위성 0) | 20~130 | 없음(SPEED와 Mazda 센서 구성만 추가) | rig가 DHU처럼 65~85초 회색 + 속도 따라감을 보이는가 |
| R1 | 같음 | 20~130 | 위성 0 → **5** | 위성 수 단독(H-G2/S) |
| R2 | 같음 | 20~130 | 정확도 **min(·, 20 m)** | 보고 정확도 단독(A20: H-G1 대 H-G3) |
| R3 | 시험 11 `V2-r1.txt`(40 m 120초, 옆 오차 0) | 20~139 | 위성 5 → **0** | 옆 오차 없는 40 m 유지 + 위성 0 (DHU V2: 위성 센서 없이 정상) |
| R4 | `commands/R4-r1.txt`(실제 111초 모양) | 20~130 | 첫 1초 **순정 mode-0**, 위성 0 | 실차 같은 입력에서 속도 고정이 나오는가(현실성 회차, 한 변수 회차 아님) |
| R5 | 시험 11 `V4T60-r1.txt` | 20~79 | 60초 전부 **순정 mode-0 재송신**(마지막 fix, 정확도 없음, 캐시 속도 50.4 km/h·방위, 위성 0) | 수정 없는 Mazda에서 폰이 무엇을 보이는가 |

모든 회차에서 손실 구간 밖의 fix에는 위성 5, 모든 fix에 SPEED = 기반 파일의 속도(Mazda 바퀴 속도에 해당, 순정 재송신 중에도
계속 변함). 사진 시각·보낸 값은 `rig-run-plan.json` `runs.<TAG>.frames`.

## 8. 관찰 규칙

- **판독자는 리드입니다.** 실행 에이전트는 사진을 모두 올리고 `frames.csv`에 잠정 판독을 적지만, 결론은 리드가 사진을 직접 보고
  정합니다. 판독 항목·라벨 이름은 시험 11 README "화면마다 기록할 것", "판정 이름"을 그대로 씁니다(`IND-GREY/NORMAL/UNREADABLE`,
  표시 속도, `NAV-*`, `POS-*`, `t_grey`, `t_stop`, `SPEED-FOLLOWS/FROZEN/NONE/MIXED`, `REPRO/GREY-ONLY/FROZEN-ONLY/NORMAL/OTHER`,
  `t_recover`, `SINGLE/CONFIRMED/INCONCLUSIVE`). `frames.csv`에는 `rig-run-plan.json`의 보낸 값 옆에 `speed_sensor_kmh`와
  `form`을 붙입니다.
- **속도 점검.** R0~R2: 시험 11 V4와 같은 τ 25·30·35·50·55·60·75·80·85·100·105·110, 구간 속도 ±5 km/h면 FOLLOW. R3: V2와 같은 점검.
  R4: τ 60·70·75·80·85·95·100·105·110(보낸 속도가 접근 속도 50.4 km/h와 10 km/h 이상 다르고 3초 범위 ≤ 4 km/h인 사진),
  표시 속도가 그 τ와 앞 2초에 보낸 속도 중 하나와 ±5 km/h면 FOLLOW, 아니면 MISS. R4의 `t_freeze` = 처음 MISS인 점검 τ와 직전
  FOLLOW τ(그 사이에 멈춤).
- **R5 라벨(순정 형태).** 점검 τ 25·30·35·50·55에서 표시 속도가 캐시 값 50.4 ± 5면 `CACHED`, 그 τ의 SPEED 값(`speed_sensor_kmh`)
  ± 5면 `SPEED-SENSOR`(구간 속도가 50.4와 10 km/h 이상 다른 점검만 구분 가능). 75% 이상이면 회차 라벨 `R5-SHOWS-CACHED` 또는
  `R5-SHOWS-SPEED-SENSOR`, 속도 표시 없음 75% 이상 `R5-NONE`, 그 밖 `R5-MIXED`. 위치: D가 τ=0 사진의 D에서 0.05 km 안이면
  `POS-HELD`, `progress_if_frozen_m`(접근 속도 외삽) 쪽이면 `POS-EXTRAPOLATES`, `progress_m`(참) 쪽이면 `POS-FOLLOWS`.
- **관찰과 해석을 나눕니다.** 사진 한 장의 순간 표시를 '고정'으로 쓰지 않고 위 점검 규칙으로만 라벨을 붙입니다.

## 9. 판정표(데이터 전에 고정, 위에서 처음 맞는 행)

**Tier 0**
| # | 결과 | 해석 |
| --- | --- | --- |
| T-1 | `SUB21-NO` | S25의 AA가 GPS_SATELLITE를 구독하지 않음: 차에서도 type 21은 나가지 않으므로(펌웨어 판독, 로그 감사) **H-G2(S)는 wire 경로로는 작동할 수 없고 beta.12의 S5 군은 효과가 없어야 함**. R1은 건너뜀. 단 다른 폰·다른 AA 버전·무선 동글 경로의 구독은 별도 |
| T-2 | `SUB21-YES` | 폰이 위성 수를 받음: R1이 H-G2를 직접 시험. `update_rate`를 기록 |
| T-3 | `SUB3-NO` 또는 `SUB1-NO` | 폰이 SPEED 또는 LOCATION을 구독하지 않음: 해당 센서는 이 폰의 판단에 쓰일 수 없음(특히 SPEED-NO면 R5의 `SPEED-SENSOR`는 불가능해야 함) |
| T-4 | `REJ-…` 있음 | 폰이 광고되지 않은 type을 요청했고 -9를 받음: 무엇을 요청했는지가 폰이 원하는 센서 목록(차에서도 같은 거부가 일어남) |
| T-5 | `CHAR-OTHER` | rig의 위치 특성이 256으로 전달되지 않음: 결과는 돌리되 '특성 256 조건' 결론은 보류 |

**회차(비정상 = REPRO, GREY-ONLY, FROZEN-ONLY; 시험 11 정의)**
| # | 결과 | 해석과 가설 |
| --- | --- | --- |
| R0-1 | R0 = OTHER/GREY-ONLY, `t_grey` 50~90, `SPEED-FOLLOWS` | **rig ≈ DHU**(DHU V4 3/3: τ65~85 회색, 속도 12/12): 위성 0·SPEED·Mazda 센서 구성을 더해도 같음. R1~R3의 차이는 그 변수 탓으로 읽을 수 있음. 옆 오차 > 보고 정확도 시점(τ60, 2배는 τ85)과 겹치면 H-G1 지지 유지 |
| R0-2 | R0의 `t_grey` < 30 | rig에서 회색이 훨씬 일찍: DHU에 없던 위성 0 센서(또는 SPEED·센서 목록)가 원인 후보. 실차 (b)의 '처음부터 회색'과 맞음. **H-G2를 R1로 바로 판정** |
| R0-3 | R0가 `SPEED-FROZEN` 포함(REPRO/FROZEN-ONLY) | **차 밖에서 처음 속도 고정 재현**: 동글(H-W) 없이도 생김. DHU와 다른 것(위성 0 구독, SPEED 센서, 센서 목록, LIVI 신원) 중 원인. R1·R4·R5로 좁힘 |
| R0-4 | R0 = NORMAL | rig가 DHU와 다름(회색이 사라짐): 차이를 기록(위성 센서 존재, SPEED, 신원). R1~R3은 R0 대비로만 해석 |
| R1-1 | R0 비정상, R1이 개선(회색 없음·늦음·고정 사라짐), `SUB21-YES`, 손실 구간 type 21 batch가 aa01020805 | **H-G2(S) 지지**: 위성 수가 폰 판단에 들어감. beta.12 S5 군의 예측과 같은 방향. 단 R1은 순정 첫 메시지가 없으므로 실차의 '첫 0'은 별도 |
| R1-2 | R1 = R0 (`SUB21-YES`) | 이 폰·이 입력에서 위성 수 효과 없음: H-G2 약화(작은 표본, r2 필요) |
| R1-3 | R1이 악화 | 위성 5 주장이 해로움: S5 군 유지 근거로 쓰지 않음(HUIG 지침과도 반대) |
| R2-1 | R2의 `t_grey`가 R0보다 10초 이상 이르거나 회색 비율 증가 | **H-G1 지지**(옆 오차가 20 m를 τ50에 넘음). A20 군은 회색을 늘릴 것으로 예측 |
| R2-2 | R2가 개선 | **H-G3 지지**(낮은 보고 정확도가 채택을 유지). H-G1과 반대 |
| R2-3 | R2 = R0 | 정확도 상한 단독 효과 없음: H-G1·H-G3 모두 약화 |
| R3-1 | R3 비정상, DHU V2(위성 센서 없음)는 정상 | 옆 오차 없이도 **위성 0 × 40 m 유지**가 비정상을 만듦: H-G2(또는 G2×G3) 지지 |
| R3-2 | R3 정상 | 옆 오차 없는 40 m + 위성 0은 무해: R0의 회색은 옆 오차(H-G1) 쪽 |
| R4-1 | R4가 `SPEED-FROZEN` 포함, `t_freeze` ≤ 10초 | 실차 (b)와 같은 '처음부터 고정': **H-F1 또는 H-G4(첫 무정확도 메시지)** 지지. 다음: R4에서 첫 순정 메시지만 뺀 회차(새 계획) |
| R4-2 | R4가 고정, `t_freeze` > 30초 | 일정 시간 뒤 고정: **H-P(자체 DR 수명)** 또는 저속 구간(τ 69~100) 반응. 고정 시작 τ를 저속 시작 τ69와 비교 |
| R4-3 | R4 회색만(속도 따라감) | 60초 터널(c)과 같은 화면: 속도 고정은 이 rig 입력으로 재현 안 됨 → H-W·폰 GNSS·실제 wire가 남음(입력 무해의 증거 아님) |
| R4-4 | R4 = NORMAL | rig가 실차와 다름: 위 남은 차이 목록을 결과에 적음 |
| R5-1 | `R5-SHOWS-CACHED` + `POS-HELD`/`POS-EXTRAPOLATES` | 순정 재송신은 네이버에 '마지막 속도 유지'로 보임: 실차 (b)의 고정 화면이 순정과 같다면 **우리 fix가 순정처럼 취급(H-F1/H-G4)**됐을 가능성. 회색 여부를 함께 적음 |
| R5-2 | `R5-SHOWS-SPEED-SENSOR` | 네이버(또는 AA)가 SPEED 센서를 표시 속도에 씀: 실차에서도 SPEED가 계속 나갔으므로 실차의 속도 고정은 SPEED 미사용 또는 동글 경로의 SPEED 손실(H-W/F3)을 시사 |
| R5-3 | 그 밖 | 기록만, 결론 없음 |
| X-1 | 회차에 `GAP>1.5s` 또는 `SESSION-CARRIED` | 해당 회차의 H-G4·세션 상태 관련 결론은 보류 |

`SINGLE` 회차 하나로는 제품 변경을 채택하지 않습니다. CONFIRMED(r1·r2 같은 분류)가 필요하면 회기 3을 씁니다. 결과는
`DIAG_BUILD_A20_S5`의 사전 등록 표(차량 beta.12)와 **별개의 증거**로 적고, 둘이 맞는지 대조만 합니다.

## 10. 결과물·업로드·정리

- 작업 폴더 `results/livi-rig-<YYYYMMDD>/`: `tier0/`, 회차별 폴더(`sensor-log.jsonl`, `livi.log`, `events.jsonl`, `pushes.jsonl`),
  `photos/`(준비·본문 사진 전부, 시도마다 `photos/<TAG>-a<시도>/`로 복사), `progress.json`, `run-log.csv`, `frames.csv`,
  `evidence.json`, `env.txt`, `versions.csv`, 한국어 `report.md`(Tier 0 표, 회차별 라벨·적용 행, 제외·FAILED 이유, 관찰과 해석 구분),
  `SHA256SUMS`.
- 업로드(시험 11과 같음): Tier 0 직후 `livi-rig-tier0-<날짜>.zip`, 회기가 끝나거나 멈춘 직후 `livi-rig-results-<날짜>.zip`(텍스트)와
  `livi-rig-photos-<날짜>.zip`(**모든 시도의 모든 사진**, 원본), 한국어 한 문단 요약 `livi-rig-summary-<날짜>.md`.
  `curl --upload-file ./<파일> https://transfer.sixtyfive.me/<파일>`. 결과가 준비되면 바로 올리되, 매일 약 06:00 KST 삭제 때문에
  05:55~06:10 KST에는 업로드를 시작하거나 진행하지 않습니다. 텍스트 결과에 계정, ADB 키, 인증 토큰, 폰 일련번호를 넣지 않습니다
  (`sensor-log.jsonl`의 `phone_make_and_model`은 기종 이름이라 둡니다).
- 정리(회기 끝): `end-guidance` → LIVI 종료 → `config.json.before-rig`를 `config.json`으로 되돌리고 sha256 기록(rig가
  `lastKnownGps`에 합성 부산 좌표를 저장하므로) → 폰 설정은 바꾸지 않았음을 확인. `~/livi-rig/`와 빌드 산출물은 다음 회기를 위해
  남깁니다(지우라는 요청이 있을 때만 지움). 레포에 커밋·푸시하지 않습니다.

## 11. 실행 에이전트에게 붙여 넣을 지시문

```text
LIVI rig 회기 <1|2|3>을 수행하십시오. 계획은 GitHub 레포 junoh-moon/mazda-aa-dr 커밋 COMMIT_TO_PIN의
validation/LIVI_RIG_PLAN_2026-10-12/README.md입니다. gh로 받아(git clone 후 git checkout COMMIT_TO_PIN) README 전체와
validation/DHU_NAVER_EXP11_PLAN_2026-10-11/README.md의 "관문", "회차 절차", "화면마다 기록할 것", "판정 이름" 절을 끝까지 읽고
README를 그대로 따르십시오. README가 이 지시문보다 자세하며, 둘이 다르면 README를 따르고 차이를 보고하십시오.

rig 코드: 그 커밋에 tools/livi-patch/가 있으면 그것을 씁니다. 없으면 RIG_ZIP_URL을 받아 sha256이 RIG_ZIP_SHA256과 같은지
확인하고 레포 루트의 tools/livi-patch/로 풉니다(다르면 멈추고 보고). 패치 파일의 sha256을 env.txt에 적으십시오.
준비(README 4절)는 회기 시간 밖에서 하며, 경로 A를 먼저 씁니다. 경로 B(관리자 암호 필요)가 필요해지면 그때 소유자에게만
알리고 기다리십시오. ~/Library/Application Support/LIVI/config.json은 바꾸기 전에 config.json.before-rig로 복사하고
회기 끝에 되돌리십시오.

명령 파일·scenarios.json·준비 파일의 글자는 하나도 바꾸지 마십시오. 이전 결과 폴더는 지우지 말고 새 폴더
results/livi-rig-<날짜>/를 쓰십시오. 소유자는 폰을 USB로 연결하고 잠금을 풀어 두는 것 외에는 손대지 않습니다. 폰 설정을 바꾸지
말고, 폰에서는 읽기 전용 명령(adb logcat -d, dumpsys)만 쓰십시오. 폰이나 Mac에 동의·권한 창(AA 연결 동의, ADB 허용, macOS
화면 기록 권한)이 뜨면 그것만 소유자에게 알리십시오. 차량이나 다른 기기는 건드리지 마십시오.

실행: (1) README 5절 Tier 0(회기 2·3은 짧은 Tier 0). 끝나면 곧바로 Tier 0 결과를 올리십시오(sensor-log.jsonl의 구독 표 포함).
ABORT이면 진단 묶음을 올리고 멈추십시오. (2) 7절의 회기 순서대로 회차를 돌리십시오. 시간 제한은 Tier 0 시작부터 25분,
남은 시간이 5분보다 적으면 새 회차를 시작하지 마십시오. SUB21-NO이면 R1을 건너뛰십시오. 진행은 progress.json에 시도마다
적으십시오. (3) 사진은 모두 올리고 frames.csv에 잠정 판독을 적되, 판정은 리드가 사진으로 다시 합니다. 관찰과 해석을
구분하십시오.

결과물과 업로드는 README 10절 그대로입니다(결과가 준비되면 바로, 05:55~06:10 KST만 피함). 링크·sha256·로컬 경로를 마지막
답에 적고 로컬 사본은 지우지 마십시오. 레포에 커밋·푸시하지 마십시오.
```

`COMMIT_TO_PIN`은 리드가 이 폴더를 커밋한 뒤 그 커밋으로, `RIG_ZIP_URL`/`RIG_ZIP_SHA256`은 `tools/livi-patch/`를 커밋하지 않을
경우 리드가 묶어 올린 ZIP으로 바꿉니다(현재 `tools/livi-patch/`는 이 작업 사본의 `.git/info/exclude`로 제외되어 있습니다).

## 12. 시험과 근거

- `python3 tests/tools/test_livi_rig_plan.py`: 생성기 결정성·커밋 파일 일치, 시험 11 입력의 바이트 재사용(T0-MOVE = V4-r1 처음 20초),
  회차마다 한 변수(R0 대 R1은 위성, R0 대 R2는 정확도만), R4의 공개 수치 일치, R5가 순정 형태, 회기 시간, 그리고 driver가 실제로 내보내는
  push(`--dry-run`)가 독립 파이썬 구현과 같음(driver가 없으면 그 시험만 생략).
- rig 코드 시험: `tools/livi-patch/TEST_RESULTS.txt`(Rust livi-aa-stack 85·livi-core 243 통과, clippy 경고 0, 패치 적용 결과가 시험한
  트리와 동일, driver 14 통과).
- 출처 구분: 위 2절의 차 쪽 사실은 로그 감사·DESIGN_V1의 기록(FIRMWARE FACT 인용, 이 작업에서 재판독하지 않음), LIVI 동작은 패치한
  소스(파일·함수 이름은 `tools/livi-patch/README.md`), R4 모양은 공개 기록 수치에 맞춘 합성(INFERENCE)입니다. HUIG·클리앙 등은
  DIAG 기록의 인용을 그대로 씁니다.
