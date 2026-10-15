# OBD yaw 채널 2단계(커밋 6b9c17d) 독립 적대 검토 — Fable (2026-10-16)

대상: `6b9c17d feat(phone-imu): OBD yaw source, stage 2, RECORD-ONLY`. 읽은 것: AGENTS.md, `validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md`, `docs/OBD_YAW_CHECK_KO.md`, `validation/PHONE_IMU_CHANNEL_2026-10-14.md`, `validation/YAW_DECODE_2026-10-10/README.md`, diff 전체와 관련 1단계 소스(`Link`, `Session`, `Integrator`, `ReconnectPolicy`, `src/collector/phone_imu.h`). **차량·폰·어댑터는 쓰지 않았습니다.** 모든 재현은 호스트 JVM의 가짜 ELM327과 단위 계산입니다. 저장소는 읽기 전용으로 두고 작업 사본(`/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/obdrev.sIKx0D/repo`, 6b9c17d 체크아웃)에서 실행했습니다.

표기: **CONFIRMED** = 코드 추적으로 확정했거나 재현함, **PLAUSIBLE** = 추론. 심각도는 차량 안전 > 기록의 해석 오류 > 견고성 > 시험 품질 순입니다.

## 0. 요약

- 앱이 어댑터에 쓰는 바이트가 AT 명령과 `X`뿐이라는 **앱 쪽 불변식은 성립합니다**(우회 경로 없음, 아래 7.1). 다만 **어댑터가 스스로 리셋된 뒤(LV RESET·브라운아웃) 컨트롤러가 전체 초기화 없이 맨 `ATMA`를 보내** 최대 10 s 동안 어댑터 기본 설정(에코 켬, 헤더 끔, CAF1, 필터 없음, 프로토콜은 클론이 저장했는지에 따름)으로 모니터를 돌립니다(F1, CONFIRMED 재현). 프로토콜이 자동(0)으로 돌아가는 클론이면 이 창에서 ELM이 **자기 판단으로 프로토콜 탐색 요청을 송신**할 수 있어 "어댑터가 진단 프레임을 보내지 않는다"는 설계 문장이 그 클론에서는 깨집니다. 수정은 작습니다.
- 주차 점검 판정 `GOOD`은 **재시작(BUFFER FULL·stall) 사이의 구멍을 전혀 반영하지 않습니다**: 60 s 중 20 s가 비어도 `GOOD`·손실 0 %가 나옵니다(F2, CONFIRMED 단위 계산). 소유자 점검의 판정 표가 이 값에 기대므로 고쳐야 합니다.
- 어댑터를 뽑아 두고(문서가 권하는 대로) 선택을 지우지 않으면, CMU에 연결된 내내 **≤30 s마다 없는 기기에 BT 페이징**을 합니다(F3, PLAUSIBLE). 1단계 차단기에는 닿지 않음을 확인했지만(7.4) 3자 BT 경쟁은 미검증입니다.
- 해독(비트 배치·부호·배율·계수기·sentinel·반올림 이월), 줄 파서(퍼즈 8 MB 무예외), 스레드·소켓 수명, CMU 수신기와의 플래그 호환, 골든 40개 결정성: **결함 없음**(7절).
- 시험: 손 변이 13개 중 7개 생존(가장 뜻있는 것은 `STALE_NS` 변이: "stale → PHONE" 시험이 프레임 수 기준으로 통과해 신선도 기준은 검증되지 않음). 구성 벡터는 구현의 거울이라 레이아웃·부호·배율의 **참/거짓을 말하지 못합니다**(8절).

## 1. 실행한 것 (DATA)

- `sh phone/imu-sender/test.sh` 종료 0: golden 40, core 501/0, obd vectors 17, **OBD 324/0**, Python stream 42프레임 dθ −7587 vs −7587.2448, e2e 134 — 커밋 메시지와 같은 수.
- `python3 -m unittest discover -s tests/tools -p test_pimu_eval.py`: 5 OK(`test_obd_source_bins` 포함).
- 검토용 하네스 `ReviewExp.java`(작업 사본 밖 `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/review_exp/`, 커밋의 core 클래스를 그대로 링크): E1 LV RESET, E2 이중 프롬프트, E3 파서 퍼즈·적대 줄, E6 stop 문자 유지 클론의 stall 2회, E7 소스 전환 손실 시뮬레이션, E8 체인 판정.
- 손 변이 13개(`ObdTests` + Python stream 검사로 판정, 9절).
- 실행하지 않음: CMU C++ 디코더 빌드(작업 사본에 D-Bus 헤더 없음; 커밋 기록은 PASS라 함), 상호운용 리그, ARM, 전체 `make test`, 실제 어댑터·차량.

## 2. 발견 (심각도 순)

### F1. 어댑터 리셋 뒤 맨 `ATMA` 재시작: 설정 없는 모니터 최대 10 s — MEDIUM, CONFIRMED(재현)

- 위치: `phone/imu-sender/src/me/sixtyfive/mx5imu/core/ElmController.java:249-254`(`case PROMPT: restart("prompt")`), `:283-299`(`restart` → `startMonitor` → `command("ATMA")`), `:261-264`(TEXT 줄은 세기만 함), `:275-278`(no-traffic 10 s 뒤에야 재초기화).
- 시나리오: 모니터 중 어댑터가 리셋됩니다(ELM의 `LV RESET`: 시동 재시작·i-stop·염가 클론의 브라운아웃, 또는 클론 워치독). 어댑터는 `\r\rLV RESET\r\r>`를 내고 **에코 켬, ATL/ATS 기본, 헤더 끔(H0), CAF1, CRA 필터 없음**으로 돌아갑니다. 컨트롤러는 `LV RESET`을 TEXT로 세고, 뒤따르는 `>`에서 `restart("prompt")`로 **즉시 `ATMA`만** 보냅니다. 초기화(`ATZ…ATCRA 079`)는 "079 프레임이 하나도 없음"이 10 s(`noTrafficMs`) 이어져야 옵니다. 그 사이 BUFFER FULL 프롬프트가 오면 같은 맨 `ATMA`를 5번까지 더 보냅니다.
- 재현(E1, FAST 타이밍 noTraffic 800 ms): 리셋 뒤 쓴 명령 순서 `[ATMA, ATZ, ATE0, …, ATMA, …]`, **첫 명령 `ATMA` at +0 ms, `ATZ`는 +1358 ms**(기본 타이밍이면 약 +10 s). 이벤트에 `elm_text LV RESET`, `elm_text ATMA`(에코 켜짐의 증거)가 찍히고도 무시됐습니다. 헤더 없는 줄 400개는 MALFORMED로, 다른 ID 400개는 foreign으로 버려졌습니다(오귀속 없음).
- 왜 문제인가: (a) 설계 2절은 `ATSP0` 자동 탐색을 "진단 요청을 송신하므로 쓰지 않는다"고 했습니다. 정품 ELM은 `ATSP6`를 비휘발로 저장해 리셋 뒤에도 6이지만, **소유자 클론이 그렇게 하는지는 기록에 없습니다**(`OBD_OWNER_SETUP`은 `ATSP6` 응답만 확인). 저장하지 않는 클론이면 이 10 s 창의 `ATMA`는 자동 프로토콜에서 돌고, ELM은 모니터 전에 프로토콜을 찾으려 **요청을 송신**할 수 있습니다(`SEARCHING...` 줄이 그 흔적이며 파서가 이미 TEXT 예로 들고 있습니다). 앱 쪽 "AT만 씀" 불변식은 지켜지지만 "어댑터가 버스에 진단 프레임을 보내지 않는다"는 문장은 그 클론에서 성립하지 않습니다. 송신 자체는 Car Scanner가 늘 하는 종류라 **차량 위험은 낮지만**, 설계가 명시적으로 피하려 한 것입니다. (b) 프로토콜이 남아도 H0·CAF1·필터 없음으로 0x079를 못 받아 10 s 공백 + 재시작 폭풍이 생깁니다. (c) 헤더 없는 줄의 데이터가 `07 9x …`로 시작하면 079 프레임으로 오인될 수 있습니다(DLC 홀수 검사로 대부분 거절, 남은 확률 낮음).
- 제안: 모니터 중 TEXT 줄이 `LV RESET`·`ELM327`(배너)·`ATMA`(에코)이면 즉시 `monitor()`를 끝내 전체 초기화로 보내고, BUFFER FULL 직후가 아닌 PROMPT도 재시작 대신 재초기화로 처리하십시오. 비용은 ATZ 1회(수 초)입니다. 가짜 ELM에 "RESET" 스크립트 줄(설정 초기화 + `LV RESET` + 프롬프트)을 더하면 시험이 됩니다. 소유자 점검 때 `atz` 뒤 `atdp`로 클론이 프로토콜을 저장하는지 한 번 보면 (a)의 불확실성이 사라집니다.

### F2. 주차 점검 판정이 재시작 구멍을 보지 못함 — MEDIUM, CONFIRMED(단위 계산)

- 위치: `core/ObdLossMeter.java:243-249`(`breakChain`: 사슬 사이 시간을 `elapsed`에서 뺌), `:259-264`(`fps = intervals/elapsed`), `:301-307`(`verdict`), `MainActivity.runObdCheck`의 결과 줄은 `fps`·`lossPercent`·`frames`만 보여 줌.
- 재현(E8): 2 s 깨끗한 사슬 20개 사이에 1 s 구멍 20개(BUFFER FULL 또는 stall 재시작마다 `monitorStarted` → `breakChain`) → **`verdict=GOOD fps=50.0 loss_pct=0.0 stalls500=0 breaks=20`**. 벽시계 60 s 중 20 s(33 %)가 비었는데 "좋음 · 초당 50 · 손실 0 %"로 보입니다. `stalls500ms`는 사슬 안의 간격만 재므로 구멍을 보지 못합니다.
- 영향: `docs/OBD_YAW_CHECK_KO.md`의 표("좋음 → 그대로 두십시오")가 이 판정에 기댑니다. 느린 클론이 BUFFER FULL을 반복해도 좋음으로 나올 수 있습니다. 반면 CMU 쪽 `Σwindow` 덮개율은 구멍을 그대로 보이므로 주행 기록과 점검 결과가 어긋납니다.
- 제안: 점검 판정에 **벽시계 기준 덮개율**(`frames / (점검 시간 × 50)`)과 `chainBreaks`(또는 `ElmController.restarts`·`bufferFull`)를 넣고, 결과 줄에 "재시작 n회"를 표시하십시오. 선택기의 건강 기준(2 s에 80)은 이미 벽시계 기준이라 그쪽은 문제없습니다.

### F3. 없는 어댑터에 ≤30 s마다 페이징, 세션당 상한 없음 — MEDIUM, PLAUSIBLE

- 위치: `core/ObdLink.java:70-84, 114-118`(백오프 2 s → 30 s 상한, 끝없음), `SenderService.startObd:384-403`(CMU 연결마다 시작), `SppConnector.open`(보안 실패 → 비보안 한 번 더 = 연결 시도 2회).
- 시나리오: `docs/OBD_YAW_CHECK_KO.md`는 며칠 세워 둘 때 어댑터를 뽑으라고 권하고, 앱의 선택은 그대로 남습니다. 다음 주행 내내(CMU 세션 동안) 폰은 없는 BT 기기에 30 s마다 두 번(보안·비보안) 페이징합니다. 안드로이드 RFCOMM connect의 페이지 시도는 수 초 동안 라디오를 점유하며, 같은 폰이 동시에 CMU(HFP + 우리 RFCOMM 50 Hz)와 무선 AA 동글을 잡고 있습니다. 설계 11절이 세 연결 동시를 미검증이라 적었는데, "없는 기기 반복 페이징"은 그중 가장 흔한 상태가 됩니다.
- 1단계 차단기와의 관계(CONFIRMED, `ReconnectPolicy.java:110-124`): PING을 받은 세션은 어떻게 끝나도 실패로 세지 않고, OBD 링크는 CMU 세션 중에만 돌므로 OBD 페이징이 CMU 연결 실패(`connectFailed`, ACL UP)로 이어질 경로는 사실상 없습니다. 즉 차단기는 안전하지만 음성·AA 품질 영향은 남습니다.
- 제안: CMU 세션당 연속 연결 실패 상한(예: 5회 뒤 그 세션에서는 중지, 다음 세션에 다시) 또는 최대 백오프를 분 단위로 늘리기. `obd_connect_failed` 로그는 이미 있어 첫 주행 기록에서 확인할 수 있습니다.

### F4. 소스 전환이 회전을 잃거나 두 번 셈; BT 지연이 `obd_gap`으로 찍힘 — LOW-MEDIUM, CONFIRMED(시뮬레이션)

- 위치: `core/SourceSelector.java:46-66, 74-85`(OBD→PHONE 즉시, 보류 창은 버림), `core/ObdLink.java:131`(모호 공백이면 `max(1, lost)`).
- 메커니즘: OBD→PHONE 전환은 "가장 최근 OBD 프레임이 0.5 s보다 오래됨"에서 일어나는데, 그 0.5 s 동안 생산된 0x079 프레임은 BT 묶음으로 **뒤늦게 도착해 보류(폐기)** 되고, 그 구간은 폰 창도 보내지 않았으므로 회전이 사라집니다. PHONE→OBD 전환은 첫 OBD 창이 BT 지연(수십~수백 ms)만큼 과거의 생산 구간을 덮으므로 그만큼 **두 번** 셉니다.
- 재현(E7, 10 °/s 등속 회전 20 s, BT 지연 150 ms, 10 s 지점에서 700 ms BT 정지 1회): 전환 3회, 보낸 합 196.0° vs 참값 200.0°(**−4.0°**). 30 °/s 회전 중이면 같은 사건이 약 −12°입니다.
- 또한 `ObdLossMeter.add:219`는 수신 간격 > 160 ms이면 계수기 단계가 1이어도 `lastAmbiguous`로 두고 `ObdLink:131`이 `obd_gap`을 켭니다. 순수 BT 지연에도 "손실 있음" 비트가 찍힙니다(덮개율 Σwindow는 맞음).
- 영향: 기록 전용이라 제품에는 무관하지만, 2단계의 목적(0x079 영점·방위 평가)에서 **frame_reset 비트가 있는 1초 묶음과 그 앞뒤 묶음은 적분 평가에서 빼야** 합니다. 설계 8·11절은 혼합 묶음만 말하고 전환 자체의 손실·중복량은 적지 않았습니다. `tools/pimu_eval.py`는 아직 모든 묶음을 폰으로 취급합니다(설계 9절이 인정).
- 제안: 설계에 "전환당 최대 ≈0.5 s 손실 / ≈BT 지연만큼 중복"을 명시하고, 분석기가 `fr`>0인 행의 묶음을 표시하도록 하십시오. 코드 쪽 완화(전환 뒤 도착하는 늦은 OBD 창을 일정 시간 받아들이기)는 소스 혼합을 낳으므로 지금은 문서화가 맞습니다.

### F5. 정상 종료 때 `X`를 보내지 않아 어댑터가 ATMA 상태로 남음 — LOW, CONFIRMED

- 위치: `ElmController.stop():81-85`(`transport.close()`만), `ObdLink.stop()`.
- 버스에는 무해합니다(수동 모니터, ACK는 설계가 이미 수용). 다만 설계 7절의 "세션이 끝나면 어댑터를 놓아 Car Scanner가 쓸 수 있다"는 절반만 맞습니다: 다음 클라이언트의 첫 명령 첫 글자가 모니터를 멈추는 데 쓰이고 나머지는 `?`가 됩니다(Car Scanner는 보통 재시도하므로 실용상 문제 없음). 앱 프로세스가 죽는 경우는 어차피 같습니다.
- 제안(선택): 정상 `stop()`에서 스트림이 살아 있으면 best-effort로 `X`를 쓰고 200 ms 안에 닫기.

### F6. ATZ 뒤 프롬프트를 두 번 내는 클론은 영원히 초기화 실패 — LOW, CONFIRMED(재현)

- 위치: `ElmController.transact:149-163`(첫 PROMPT에서 반환), `initialise:198-207`(다음 명령의 응답에 `OK`가 없으면 예외).
- 재현(E2): `ELM327 v1.5\r\r>\r>` → `init ATE0: `로 실행 종료, 명령 `[XATZ, ATZ, ATE0]`. ObdLink가 백오프로 재연결하지만 매번 같습니다(상한 30 s, 무한). 클론 의존이라 낮음. 제안: 명령 전에 큐에 남은 PROMPT를 비우기.

### F7. 잘린 15자 줄이 6바이트 샘플로 수용됨 — LOW, CONFIRMED

- `079DF285A01FFD5`(E3) → CAN len=6 → `Can079.decode` OK(counter −1). `MIN_LEN=6`은 설계 의도이고 RFCOMM은 신뢰 전송이라 잘림은 ELM 출력 단계에서만 가능합니다(BUFFER FULL은 프레임 단위로 버린다고 알려짐). 기록에 `short`가 아닌 정상 샘플로 남을 뿐 값은 맞습니다.

### F8. 기타 LOW

- `ElmController.written`(`:63, 137-141`)은 제품 코드에서 명령마다 문자열을 무한히 쌓습니다(연결당, 폭풍 시 시간당 수백 KB). 시험용이면 상한을 두십시오.
- `MainActivity.runObdCheck`와 `SenderService.startObd`는 `Status.obdServiceRunning`/`obdCheckRunning`을 서로 다른 스레드에서 검사-후-설정하므로 동시에 둘 다 어댑터를 열 수 있습니다(둘 중 하나는 연결 실패 → 백오프). 영향 작음.
- `SenderService.stopObd`는 비동기 스레드(최대 3 s join)라 2 s 뒤 재연결이 오면 옛 링크와 새 링크가 잠시 겹칩니다(어댑터는 한 클라이언트만 받으므로 새 링크가 한 번 백오프). 영향 작음.

## 3. 결함을 찾지 못한 영역 (무엇을 어떻게 보았는지)

### 3.1 차량 안전: 쓰기 경로 (CONFIRMED 결함 없음)

- `write()`는 `command()`와 `interrupt()`에서만 호출됩니다(`ElmController.java:137-146, 176-184`). `command()`는 `SAFE_COMMAND = "AT[A-Z0-9 ]{1,16}"`에 **전체 일치**를 요구하고 고정 리터럴 + `String.format("ATCRA %03X", canId)`만 만듭니다. 맨 CR을 보내는 경로는 없습니다(모든 명령이 `AT`+1자 이상). 재시도 경로(`ATZ` 재시도, `?` 뒤 `ATMA` 재시도, 재시작, 재초기화) 모두 같은 두 함수를 거칩니다.
- 모니터가 도는 중에 명령을 보내는 경합(늦은 PROMPT 뒤 `restart`, F1의 맨 `ATMA`)에서 ELM이 첫 글자 `A`를 정지 문자로 먹어도 남는 꼬리(`TMA`, `TZ`, `TE0`, `TL0`, `TS0`, `TH1`, `TSP6`, `TCAF0`, `TCRA 079`)는 모두 `T`를 포함해 16진이 아니므로 `?`가 되며 OBD 요청으로 해석될 수 없습니다. `X`가 남는 클론도 `XAT…` → `?`. **앱이 보낸 바이트가 버스 요청이 되는 조합은 없습니다.**
- BUFFER FULL: 프롬프트가 따르면 재시작, 따르지 않으면 계속, 프레임이 멎으면 1 s stall → `X` → `ATMA`. 재시작 10 s에 6회면 재초기화, 프레임 없는 재초기화 3회면 연결 종료·백오프 — 어댑터를 붙잡고 무한히 명령하지 않습니다(`controllerNoTraffic` 시험이 덮음).
- `stop()`·`run()`의 `finally`·`ObdLink`의 `finally`·`SppConnector.abort`(대기 중 connect 닫기)로 모든 경로에서 소켓이 닫힙니다. `X`를 보내지 않는 점만 F5.

### 3.2 해독 정확성 (CONFIRMED 결함 없음; 참값은 미검증)

- 비트 배치 `((b3&3)<<10)|(b4<<2)|(b5>>6)`는 `YAW_DECODE_2026-10-10/README.md`의 후보식과 글자까지 같고, DBC Motorola start bit 25·length 12를 손으로 전개해도 같은 비트입니다(바이트 3 비트 1·0, 바이트 4 전체, 바이트 5 비트 7·6). DBC의 `31|16@0+`는 바이트 3·4라 하위 2비트를 버린 것이 맞고, 중심 N=512 ↔ raw 2048~2051로 명목 영점 2047과 1/4카운트 안에서 맞습니다.
- 부호·배율: `-(raw-2047)·0.000658615`는 `src/navigation/pipeline.cpp:28`의 `{2047.0, -0.000658615}`와 `ND_CAN_LOG_2026-10-03` 25·26행(좌회전에서 증가, 26.5카운트/°/s)과 일치합니다. 배율 1~3 %·DBC와 1.48배 어긋남은 설계가 공개한 대로 남습니다.
- 계수기 `b6&0x0F`, 단계 `(c−last)&15`, 15→0은 단계 1(시험 있음), 반복 계수기는 버리지 않음, 160 ms 넘는 공백은 모호 처리, 함의 생산율 검사로 "계수기가 1씩 안 오르는 차"에서 거짓 손실 없음(시험 있음). `0x000/0xFFF` sentinel은 세기만 하고 거르는 설계대로입니다.
- 반올림 이월: `carry = exact − rounded`, 3만 프레임 표류 ≤ 0.5 µrad(시험), `clampToInt`는 |dθ| ≤ 26,963 µrad라 닿지 않습니다. `frame_reset`: 모니터 (재)시작마다 `integrator.reset()`(이월 0 포함)과 `breakChain()`이 같은 잠금 안에서 함께 일어나고, 선택기 전환과 세션의 첫 전송·stale 재개가 각각 비트 3을 덧붙입니다. CMU `Aggregator::add`(`phone_imu.h:186-192`)는 비트 3이 있으면 seq 공백을 세지 않으므로 전환·재시작이 가짜 `gap`을 만들지 않습니다.

### 3.3 줄 파서 (CONFIRMED 결함 없음)

- E3: 무작위 바이트와 16진/제어문자 편향 바이트 8 MB → 이벤트 428,942개, overflow 20,374, **예외 없음, 미완성 줄 최대 96자**(상한 유지). 적대 줄 분류: `<RX ERROR`/`<DATA ERROR` 꼬리 → ERROR, `ATMA`/`XATMA`/`BUS BUSY`/`UNABLE TO CONNECT`/`ACT ALERT` → TEXT, `079 8 …`·`0798…`·앞뒤 공백·소문자 → CAN 8바이트, `07`·`…AA`(9바이트) → MALFORMED. `>`는 줄 중간에서도 프롬프트로 처리되고 NUL은 세고 버립니다. 헤더 없는 16자 줄은 앞 3자가 0x7FF를 넘으면 MALFORMED라 오귀속이 드뭅니다(F1 (c)).
- 1줄 96자 상한, 큐는 컨트롤러 스레드가 항상 소비(`transact`도 CAN을 버리며 소비)하므로 메모리가 자라지 않습니다. 리더 스레드의 예외 경로 없음(16진 검증 뒤에만 `parseInt`).

### 3.4 동시성·수명 (CONFIRMED 결함 없음, F3·F8 제외)

- `ObdLink.stop()`: `running=false` → `connector.abort()`(대기 중 소켓 닫기, `aborted`로 비보안 재시도 차단) → `ElmController.stop()`(전송 닫기 + 리더 인터럽트) → 스레드 인터럽트 + join 3 s. `connector.open()`이 반환된 직후 `current`가 아직 null인 창에서도 스레드 인터럽트 플래그가 `queue.poll`에서 `IOException`으로 바뀌어 `finally t.close()`로 닫힙니다.
- `Session.offer`는 비차단(`ArrayBlockingQueue.offer`)이라 OBD 스레드가 CMU 소켓에 묶이지 않습니다. 선택기 잠금 안에서 `admit`→`offer` 순서가 정해져 전환 직후 옛 소스가 섞이지 않습니다.
- 1단계 차단기: `ObdLink`는 `ReconnectPolicy`를 전혀 만지지 않고, PING을 받은 세션은 실패가 아니므로(`ReconnectPolicy.java:117`) OBD 쪽 장애가 차단기를 올릴 경로가 없습니다. ACL 방송은 CMU 주소로 거릅니다(`SenderService.onBluetoothEvent:224`).
- 권한·FGS: 기존 `BLUETOOTH_CONNECT`·`connectedDevice` 유형 그대로, `SecurityException`은 `IOException`으로 바꿔 백오프. 세 BT 연결 동시는 여전히 미검증(설계 11절).

### 3.5 플래그와 CMU 수신기 (CONFIRMED 결함 없음)

- `decode_data`(`phone_imu.h:64-70`)는 flags를 검사 없이 복사하고 `Aggregator::add`는 OR, 행은 `fl & 255`로 찍습니다. 비트 4·5가 그대로 남고 1단계 의미(비트 0~3)는 바뀌지 않습니다. `FLAG_MASK` 63은 인코더 쪽 거절 범위만 넓혔고 비트 6·7은 여전히 예외(시험 있음).
- 골든: `make_golden.py --check` 통과(결정적), OBD 4개는 비트 0~2 = 0·window 20000·bias 0로 6절 규격 문구와 맞습니다. JVM·Python이 40개를 같은 값으로 읽음을 확인했고 C++는 이번에 빌드하지 못했습니다(커밋 기록 PASS).
- 분석기 `phone_imu.obd` 절은 비트 4 묶음이 없으면 생기지 않아 기존 보고서가 그대로이며 시험 5건 통과. `tools/pimu_eval.py:109-116, 128-130`은 flags를 보지 않고 모든 묶음을 폰으로 셉니다(설계 9절이 TODO로 인정; F4와 함께 다음 작업).

## 4. 시험 품질

- 손 변이 13개(`ObdTests` + Python stream 검사): **KILLED 6** — `X` 미송신(단언이 빈 문자열에 걸림), `MAX_LINE` 제거, 재초기화 상한 제거(시험이 멈춤 = 사실상 검출), sentinel 제거, (커밋 기록의 14개도 재현된 것으로 봄). **SURVIVED 7**:
  - `STALE_NS` 0.5 s → 5 s: "OBD stale: back to PHONE" 시험(`ObdTests.selector`)은 마지막 프레임 뒤 0.6 s 시점에 **2 s 창의 프레임 수가 69 < 80**이 되어 통과하므로 신선도 기준 자체는 검증되지 않습니다. 프레임을 2 s 창에 충분히 남긴 채 0.6 s만 비우는 시험이 필요합니다.
  - 재시작 폭풍 검사 제거(`restartIdx > MAX_RESTARTS` → false): 폭풍 → 재초기화 경로에 시험이 없습니다.
  - 모호 공백의 `max(1, lost)` 제거, 계수기 반복+긴 공백의 `15`, `questionRetried` 1회 제한, `MIN_FRAMES_2S` 80→70, `transact`의 CAN 무시: 시험 없음(모두 동작에 큰 영향은 없음).
- 구성 벡터가 증명하는 것과 못 하는 것: `make_obd_vectors.py`의 `frame()`/`decode()`와 `Can079`는 같은 사람이 같은 문서에서 쓴 같은 식입니다. 벡터는 **두 구현의 일치·마스킹(정의되지 않은 바이트가 결과에 안 섞임)·상태 분기·반올림 합**을 증명하고, **비트 배치·부호·배율이 실제 0x079와 맞는지는 전혀 증명하지 못합니다**(레이아웃이 통째로 틀려도 전부 통과). `dbc_yawrate_corr_dps`도 DBC 식을 두 번 계산할 뿐 `cw_dps`와의 관계(부호 반대·비 ≈1.48)를 단언하지 않습니다. 참값의 유일한 근거는 공개 로그 1개(`YAW_DECODE`)이며 소유자 차량 확인은 주차 점검·첫 기록에 달려 있습니다. 벡터 파일 머리말과 설계 11절이 이 한계를 이미 적고 있어 **표현은 정직합니다**; 권장: 공개 로그의 실제 0x079 프레임 몇 개(바이트와 0x078 짝)를 벡터로 옮겨 "구성"이 아닌 "관측" 벡터를 최소 1개 두십시오(저장소에 원시 바이트를 두지 않는 규칙과 충돌하지 않는 범위에서 — 8바이트 프레임 몇 개는 OEM 바이너리가 아닙니다).
- 가짜 ELM은 BUFFER FULL·stall·stop 문자 유지·거절을 덮지만 **리셋(설정 초기화)·이중 프롬프트·자동 프로토콜**은 덮지 않습니다(F1·F6).

## 5. 권장 조치 (우선순위)

1. F1: PROMPT/`LV RESET`/배너/에코 → 전체 재초기화. 가짜 ELM에 RESET 스크립트 추가. 주차 점검에 `atz` → `atdp` 한 줄 추가해 클론의 프로토콜 저장 여부 확인.
2. F2: 점검 판정에 벽시계 덮개율과 재시작 수 반영, 결과 줄에 표시, `OBD_YAW_CHECK_KO.md` 표 갱신.
3. F3: 세션당 연결 실패 상한 또는 분 단위 최대 백오프.
4. F4: 설계에 전환 손실·중복량 명시, 분석기가 `fr`>0 행을 표시, `pimu_eval`의 OBD 분리(이미 TODO).
5. 시험: `STALE_NS` 전용 시험, 폭풍→재초기화 시험, 관측 벡터 1개 이상.
6. F5·F6·F8은 선택.

이 검토는 호스트 재현과 코드 추적만으로 이루어졌으며, 차량·어댑터 거동(ACK, 프로토콜 저장, 처리율, 3자 BT)에 대한 판단은 바꾸지 않습니다. 발행·설치·주행을 승인하지 않습니다.
