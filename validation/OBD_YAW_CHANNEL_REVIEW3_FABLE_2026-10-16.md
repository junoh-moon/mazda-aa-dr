# OBD yaw 채널 2단계 — 3차 독립 적대 검토 (Fable, 2026-10-16)

대상: 커밋 `42a8058` (2차 검토 `validation/OBD_YAW_CHANNEL_REVIEW2_FABLE_2026-10-16.md`의 R1~R6 수정; 설계 노트 `validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md` 13절). 12파일, 새 `core/ObdArbiter.java` 포함.
방법: 저장소는 읽기만 했고, `git archive 42a8058`로 뽑은 사본 `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/rev3.hCxKWr/`에서 컴파일·시험·변이를 돌렸습니다. 상호운용 리그는 돌리지 않았습니다(다른 작업이 실행 중). 아래 줄 번호는 `42a8058`의 파일 기준입니다.
표기: **CONFIRMED** = 코드 추적으로 확정했거나 호스트에서 재현함, **PLAUSIBLE** = 추론. 심각도는 차량 안전 > 기록 해석 > 견고성(자료 수율) > 시험 품질 순입니다.

## 0. 요약

- **차량 안전 불변식 유지(CONFIRMED):** 어댑터에 쓰는 바이트는 여전히 `SAFE_COMMAND`를 통과한 AT 명령 + CR(`ElmController.java:197-203`)과 단일 `X`(`:264` stall/초기화 정지, `:128` `stop()`의 helper) 뿐입니다. 쓰기 호출은 이 세 곳이 `write()`(`:189-195`, `synchronized`)를 거치는 것이 전부이고 새 코드(`collect`, ATZ 맨 프롬프트 루프, 늦은 프롬프트 규칙, `ObdArbiter`, 느린 재시도)는 아무것도 쓰지 않습니다. `SenderService.java:515`의 `output()`은 CMU 쪽 RFCOMM 전송이지 어댑터가 아닙니다.
- **`src/`와 CMU 와이어 형식 변경 없음(CONFIRMED):** `git diff --stat 42a8058^ 42a8058 -- src/ …/core/Protocol.java …/core/Window.java …/core/ObdIntegrator.java tools/` 비어 있음. 커밋 메시지의 "analyzer"는 실제로는 이 커밋에 분석기 파일이 없습니다(12파일 모두 phone/·docs/·validation/). 안드로이드 연결 코드는 제가 `android-36` `android.jar`에 대해 `-Xlint:all,-deprecation -Werror`로 다시 컴파일해 통과했습니다(컴파일만).
- **가장 뜻있는 발견(F1, 호스트 모델 CONFIRMED / 차량 PLAUSIBLE, MEDIUM 자료 수율):** R1(b)의 "ATZ 맨 프롬프트" 규칙은 **어댑터가 정지 때 `STOPPED` 줄을 찍으면 무력화**됩니다. ELM327은 모니터를 글자로 끊으면 `STOPPED`를 찍고(`ElmLineParser.Kind.STOPPED`의 주석과 2차 검토 R1 시나리오도 그렇게 가정), 그러면 늦게 온 정지 답은 `[STOPPED, >]`라 `hasReplyText`가 참이 되어 ATZ의 답으로 받아들여지고, 2차 검토가 재현한 그대로 `ATDP`가 배너를, `ATE0`가 `ATDP`의 답을 받아 `init ATE0: ATDP|AUTO`로 실행이 끝납니다. 가짜 ELM이 끊길 때 `\r>`만 보내도록 되어 있어 새 시험이 이 경우를 보지 못했습니다. 가짜에 `STOPPED`를 넣으니 `controllerLateStopPromptAtInit`이 정확히 그 사유로 실패했고(4 FAIL), `hasReplyText`에서 `Kind.STOPPED` 줄을 건너뛰는 한 줄 수정으로 514/0으로 돌아왔습니다. 모니터 중의 늦은 정지 프롬프트 경로(R1 본체)는 `STOPPED`가 있어도 정상이었습니다.
- 나머지는 LOW: R3 수정이 남긴 두 개의 µs 창(F2), 생존 변이 3개가 드러낸 시험 약점(F3), 점검 스레드의 예외가 `checkRunning`을 영영 켜 두는 경로(F4), ATZ 재전송·차단 시간 상한의 문서 정밀도(F5).
- 시험 안정성: `ObdTests` 7회(기준 1 + 반복 5 + 수정 실험 1) 모두 514/0, 각 29~30 s, 1분 부하 1.3~2.6. 흔들림 없음.

## 1. 실행한 것 (DATA)

- 사본에서 `javac --release 11 -Xlint:all -Werror`(core + tests) 뒤 `ObdTests … docs/OBD_YAW_CHECK_KO.md`: 기준 **514 passed, 0 failed**, 29 s, 부하 2.3.
- 반복 5회(`flaky1~5`): 모두 514/0, 29~30 s, 시작 부하 1.9~2.6.
- 손 변이 4개(2절 F3, 4절): m1 KILLED(4 FAIL), m2 SURVIVED(동치), m3 SURVIVED, m4 SURVIVED.
- 실험 e1(가짜 ELM이 정지 때 `\rSTOPPED\r\r>`): **510/4** — 실패 4개 모두 `controllerLateStopPromptAtInit`(`init ATE0: ATDP|AUTO`, `elm_version STOPPED`, `elm_protocol_after_reset ATZ|ELM327 V1.5`, `atzBarePrompts 0`). 실험 e1fix(e1 + `hasReplyText`가 `Kind.STOPPED` 무시): 514/0.
- 안드로이드 연결 코드(`SenderService`, `MainActivity`, `Status`, `SppConnector`, `Prefs`, core): `$HOME/.local/share/android-sdk/platforms/android-36/android.jar`에 대해 `-Werror` 컴파일 통과.
- 실행하지 않음: `make test-phone-imu-interop`(지시대로), 전체 `make test`, ARM, APK 빌드, 실제 어댑터·폰·차량.

## 2. 발견 (심각도 순)

### F1. 정지 답에 `STOPPED` 줄이 있으면 ATZ 맨 프롬프트 규칙이 비켜 가 R1의 초기화 실패가 그대로 남음 — MEDIUM(자료 수율), 호스트 모델 CONFIRMED / 차량 PLAUSIBLE

- 위치: `ElmController.java:286-300`(초기화: `interrupt(false)` → `queue.clear()` → `transact("ATZ")` → 맨 프롬프트 루프), `:330-336`(`hasReplyText`: 에코가 아닌 **어떤** 텍스트 줄이든 참), `ElmLineParser.java:30-31`(`STOPPED` = "모니터나 요청이 우리 입력으로 끊겼다"), `ObdTests.java` `FakeElm.onByte`(모니터 중 글자를 받으면 `send("\r>")`만 — `STOPPED` 없음).
- 시나리오(2차 검토 R1(b)와 같음, 어댑터가 `STOPPED`를 찍는 점만 다름): 이전 클라이언트가 `ATMA`를 남겼거나 우리 자신의 재초기화(폭풍·무수신·늦은 프롬프트 > 2×stopMs) 때 어댑터가 모니터 중 → 초기화의 `X` → 어댑터는 즉시 `STOPPED\r\r>`를 내지만 BT 지연 > `stopMs`로 `interrupt(false)`가 빈손으로 끝남 → `queue.clear()` → `ATZ` 송신 → 뒤늦게 `[STOPPED, >]` 도착 → `hasReplyText`가 `STOPPED`를 "응답 텍스트"로 보아 **ATZ의 답으로 채택**(`version = "STOPPED"`) → `ATDP`가 ATZ의 배너를, `ATE0`가 `ATDP`의 답을 받아 `OK` 없음 → `IOException("init ATE0: ATDP|AUTO")` → 실행 종료 → `ObdLink` 백오프 재연결. 즉 2차 검토가 MEDIUM으로 적은 결과와 동일합니다.
- 재현(사본, FAST 타이밍 `stopMs` 300, 지연 400 ms): `FakeElm.onByte`의 정지 응답만 `"\rSTOPPED\r\r>"`로 바꾼 e1에서 `controllerLateStopPromptAtInit` 4개 검사 실패: `init completes and frames flow (0) [java.io.IOException: init ATE0: ATDP|AUTO] [elm_version STOPPED, elm_protocol_after_reset ATZ|ELM327 V1.5]`, `atzBarePrompts 0`. 같은 가짜로 `controllerLateStopPrompt`(모니터 중 stall 뒤 늦은 프롬프트)는 통과: 그 경로에서는 `STOPPED`가 `default:` 가지에서 TEXT로 세어지고(`:422-429`, `adapterResetLine` 불일치) 뒤따르는 프롬프트가 `lateStopPrompt`로 소비되므로 R1 본체 수정은 유효합니다.
- 차량 영향: (i) 어댑터가 실제로 `STOPPED`를 찍는지(ELM327 원칩은 찍음; 클론은 제각각, 소유자의 JF327은 미측정), (ii) 지연 > `stopMs`(기본 1 s)가 겹쳐야 합니다. 둘 다 미측정이므로 PLAUSIBLE이며, 범위는 2차 R1과 같이 자료 수율과 혼란스러운 기록(`obd_run_end why=init ATE0…`)입니다. 버스에는 무해합니다(쓴 것은 `X`, `ATZ`, `ATDP`, `ATE0`).
- 제안(한 줄 + 시험 모델): `hasReplyText`에서 `e.kind == ElmLineParser.Kind.STOPPED`인 줄을 건너뛰십시오(e1fix로 514/0 확인). 가짜 ELM의 정지 응답에 `STOPPED` 변형을 두고(`retainStop`처럼 플래그) 늦은 프롬프트 시험 둘을 두 변형으로 돌리십시오. 함께 보십시오: `transact("ATDP")`의 답을 검증하지 않으므로(`:305-306`) ATZ 답이 어긋나면 다음 명령에서야 드러나는 구조는 그대로입니다. 더 근본적으로는 각 `transact`가 "이 명령의 에코 또는 기대 토큰"을 확인할 때까지 늦은 답을 버리는 재동기화가 있어야 지연 > `cmdMs`도 견디지만, 그것은 이번 범위 밖입니다.

### F2. R3 수정이 남긴 두 개의 좁은 창 — LOW, CONFIRMED(코드; 시험으로 못 잡음)

- (a) `ElmController.java:201-202`: `command()`의 `if (stopped) throw` 와 `write(cmd + "\r")` 사이. `stop()`(`:123-133`)은 `stopped = true` 뒤 helper 스레드가 `X`를 씁니다. 컨트롤러 스레드가 `:201` 검사를 지난 직후 `stop()`이 오면 helper의 `X`가 먼저, `ATMA\r`가 뒤에 나가 어댑터가 모니터 중으로 남을 수 있습니다. 2차 R3의 창(모니터 루프 한 바퀴)을 수 µs로 줄였을 뿐 닫지는 못했습니다. 새 시험 `controllerNoAtmaAfterStop`은 `restart()` 안의 시계 호출에서 `stop()`을 강제하므로 `:201` 검사 **앞**에서 멈추는 경우만 봅니다.
- (b) `ElmController.java:350-352`: `command("ATMA")`가 쓴 뒤에 `monitorActive = true`가 됩니다. 그 사이에 `stop()`이 오면 `sendX = monitorActive && …`(`:124`)가 거짓이라 **`X`를 보내지 않고** 닫으므로 어댑터가 `ATMA` 상태로 남습니다(다음 클라이언트의 첫 글자가 먹힘; 우리 자신의 다음 초기화는 `interrupt(false)`로 수습, Car Scanner는 첫 명령에 `?`).
- 제안: (b)는 `monitorActive = true`를 `command("ATMA")` 앞으로 옮기고 예외 시 되돌리십시오. (a)는 `stopped` 검사를 `synchronized write()` 안으로 옮기고 helper의 `X`만 예외로 두면(`write(s, allowAfterStop)`) 같은 잠금 아래서 순서가 정해집니다. 어느 쪽도 버스 요청과 무관하며 바이트 섞임은 없습니다(`write()` synchronized).

### F3. 생존 변이 3개가 드러낸 시험 약점 — LOW(시험 품질), CONFIRMED

- m2 `lateStopPrompt`에서 `atmaSinceStopX &&` 제거 → 514/0 생존. **동치 변이**입니다: 마커가 켜진 채 `monitor()`가 프롬프트를 보는 때는 항상 `restart("stall")`의 `ATMA` 뒤(`:437-438` → `:480` → `:351`)이고, 폭풍으로 `restart`가 거짓이면 곧바로 `initialise()`의 `interrupt(false)`가 마커를 덮어씁니다. 방어적 조건으로 두어도 되지만 설계 13절의 "그 뒤에 ATMA를 보낸 상태에서"라는 문구만큼의 역할은 없습니다.
- m3 `ObdArbiter.endCheck()`에서 `deferred = null` 제거 → 생존. `arbiterDefersSessionStart`의 미룬 실행자가 스스로 `startServiceOrDefer`를 다시 불러(`ObdArbiter.java:30-32`) `deferred`를 지우기 때문입니다. 실제 코드의 `later`는 `main.post`만 하므로(`SenderService.java:403-409`) 변이판에서는 포스트된 실행자가 돌기 전까지 `deferred`가 남고, 그 사이 점검이 한 번 더 시작·종료되면 두 번 실행됩니다(제품에서는 `later`의 `obd != null`·`active != session` 가드가 막아 무해). 시험은 "한 번만 실행"을 사실상 검증하지 못합니다. 제안: 순수 계수 실행자로 미루고 `endCheck()`를 두 번 불러 1인지 확인하십시오.
- m4 `ObdLink.java:136` `gaveUp = false` 제거 → 생존. `linkSlowRetryAfterGiveUp`은 연결 성공 뒤 `gaveUp`을 확인하지 않습니다. 영향은 기록 문구뿐(`obd_connect_giveup` 대신 `obd_connect_slow_retry_failed`가 찍힘)이지만 공개 플래그 `gaveUp`의 뜻("느린 모드 **중**")이 시험되지 않습니다.
- m1 `LATE_STOP_FACTOR` 2→1(창 경계) → **KILLED**(4 FAIL: 84/100 프레임, `adapterResets 2`, `inits 3`). 부수 관찰: 창을 벗어난 늦은 프롬프트는 설계대로 `elm_unexpected_prompt reinit` → `elm_atz_bare_prompt try=1` → 재초기화 성공으로 회복했습니다(가짜는 `STOPPED`를 안 찍으므로 F1 조건 밖).

### F4. 점검 스레드가 예외로 끝나면 `checkRunning`이 영영 켜짐 → 모든 세션의 OBD 시작이 조용히 미뤄짐 — LOW, CONFIRMED(코드)

- `MainActivity.java:205-230`: `tryStartCheck()` 뒤의 스레드에 `try/finally`가 없고 `Status.OBD.endCheck()`는 마지막 줄 근처(`:228`)입니다. `link.verdict/summary/resultLine` 등에서 `RuntimeException`이 나면(가능성은 낮음) `checkRunning`이 프로세스 수명 동안 참으로 남고, 이후 CMU 연결마다 `startObd()`가 `obd_deferred check_running`만 남기고 끝납니다(`SenderService.java:411-417`). 수정 전에도 `obdCheckRunning`이 같은 식으로 남았지만, 그때는 `obd_skip`이었고 지금은 "점검이 끝나면 시작한다"는 약속이 조용히 깨지는 셈입니다. 제안: `finally { Status.OBD.endCheck(); }`.
- 같은 자리의 메모: `link.stop()`은 스레드를 최대 3 s만 기다리므로(`ObdLink.java:91`), 시간 초과면 점검 링크가 소켓을 쥔 채 `endCheck()`가 미룬 시작을 돌려 첫 연결이 "busy"로 실패하고 백오프로 넘어갑니다. 무해하고 자가 회복.

### F5. ATZ 재전송·차단 시간의 상한이 문서보다 큼 — LOW, CONFIRMED(코드)

- `ElmController.java:289-300`: `ATZ` 쓰기 횟수는 처음 1 + `?` 재시도 1 + 맨 프롬프트 루프 최대 2 = **최대 4회**(설계 13절 "합쳐서 최대 2번"은 루프만 말함). 차단 시간은 루프 한 바퀴가 `collect(atzMs)` 시간 초과 + `transact(ATZ, atzMs)`이므로 최악 `6 × atzMs` = 기본 30 s(이전 10 s). 이 최악은 배너 없이 프롬프트만 답하는 어댑터에서만 나며, 그때도 루프가 끝나면 `r`이 비어 있지 않아(`version = "ATZ"` 에코) 초기화는 계속됩니다. 죽은 어댑터는 `r == null`로 즉시 빠집니다. `stop()`은 `collect`가 100 ms 단위로 `stopped`를 보므로 즉시 반영됩니다. 문서 숫자만 맞추면 됩니다.

### F6. 기타 메모 (결함이라기보다 관찰)

- 점검(60 s)에서 5회 실패 뒤 상태 문자열이 `OBD 연결 안 됨 · 5분마다 다시 시도`로 보입니다(`ObdLink.java:124`). 점검에 5분은 없으므로 문구만 어색합니다.
- `echoVerifiedOff` 거짓 양성: `ATL0`…`ATCRA` 여섯 답에 에코가 하나도 없는데 `ATMA`만 에코하는 칩이면 R2 이전과 같은 무한 재초기화가 남습니다. 병적이며 PLAUSIBLE-낮음. `ATS0` 시간 초과는 `echoSeen = true`로 보수적 방향(리셋 감지 비활성)입니다 — 올바른 쪽.
- 2차 R3 보고의 `obd-stop-x` helper와 `interrupt()`의 `X`가 겹치면 어댑터 버퍼에 `X`가 남아 다음 클라이언트의 첫 명령이 `?`가 될 수 있는 점은 이전과 같습니다(우리 초기화는 재시도로 수습).

## 3. 결함을 찾지 못한 영역 (무엇을 어떻게 보았는지)

### 3.1 늦은 정지 프롬프트 마커 (CONFIRMED 결함 없음, F1 제외)
- 설정/해제: `lastStopXNs`는 `interrupt()`에서 `X`를 쓴 **뒤**(`:265`)에 찍고, (i) 정지 대기 안의 프롬프트(`:272`), (ii) 모니터의 늦은 프롬프트(`:400`, "X당 한 번"), (iii) ATZ 답 수락(`:301`)에서 지웁니다. CAN 줄은 지우지 않습니다(설계 근거 타당: 어댑터가 `X`를 읽기 전 보낸 프레임이 프롬프트보다 먼저 올 수 있음).
- 영원히 남는가: stall 뒤 프롬프트가 2×stopMs 뒤에 오거나 아예 안 오면 마커가 켜진 채 남지만, `rxNs - x <= 2×stopMs`(`:462-463`) 때문에 그 뒤의 어떤 프롬프트도 늦은 답으로 오인되지 않고, 다음 `interrupt()`가 덮어씁니다. 해롭지 않은 잔존입니다. `Long.MIN_VALUE` 보초는 첫 조건으로 걸러져 뺄셈 오버플로가 없습니다.
- 진짜 리셋을 삼키는가: `LV RESET`/`ELM327` 줄은 프롬프트보다 먼저 TEXT로 와서 `:425`에서 재초기화되므로 삼켜지지 않습니다. 배너 없는 "프롬프트만" 리셋이 stall `X` 뒤 2×stopMs 안에 겹치면 삼켜지지만, 그 뒤 프레임이 없으므로 `lastFrame == 0`·`monitorStart` 갱신(`:439-440`)으로 `noTrafficMs`(10 s) 뒤 무수신 재초기화로 넘어갑니다(설계 13절이 적은 그대로). 그때 `any`는 이미 참(stall은 프레임이 있었을 때만)이라 3회 무수신 종료와 섞이지 않습니다. 늦은 프롬프트는 `restart()`를 부르지 않으므로 폭풍 계수기와도 독립입니다.
- 리더 스탬프 경합: 프롬프트의 `rxNs`가 `X`보다 늦게 찍히는 경우는 그 프롬프트가 정지 대기 중 큐에 있어 (i)로 소비되므로, 모니터의 늦은 규칙에는 `ATMA` 뒤에 도착한 프롬프트만 옵니다.
- `?` 경로와의 겹침: 늦은 프롬프트 → `?` → 프롬프트 순서면 첫 프롬프트는 늦은 답, `?`는 기존 재시도(`:383-391`)가 자기 프롬프트를 소비합니다.

### 3.2 정지 대기 중 전달한 프레임 (CONFIRMED 결함 없음)
- `interrupt(true)`(`:273-277`)에서 전달한 프레임은 큐에서 빠지므로 중복 전달·이중 계수가 없습니다. `listener.frame` → `ObdLink.onFrame` → 손실계·적분기(옛 사슬 안)로 가고, 그 **뒤** `restart` → `monitorStarted` → `breakChain`·`integrator.reset()`이 오므로 다음 창에 `FRAME_RESET`이 붙는 순서가 맞습니다. 대기 종료 뒤 지연으로 도착하는 옛 프레임은 새 사슬에 들어가 계수기 간격만큼 손실로 세어질 수 있습니다(재시작 구멍의 손실이므로 실질적으로 맞는 값; 설계의 "사슬 경계에선 손실을 추정하지 않음"과 약간 어긋나는 정도).

### 3.3 echoVerifiedOff (CONFIRMED 결함 없음, F6 메모 제외)
- `ATE0` 자신의 에코는 제외(`:314`), 이후 여섯 명령 중 하나라도 에코되면 미확인 → 보수적. 일부만 에코하는 클론도 "하나라도"로 미확인. `echoes`는 공백 제거·`X` 접두 처리. 에코를 못 끄는 칩의 조용한 리셋은 `ATMA` 에코로 못 보고 무수신 감시(10 s)가 받습니다 — 설계 13절과 일치. `echoVerifiedOff`는 초기화마다 다시 계산(`:309`, `:322`).

### 3.4 ObdArbiter·연결 (CONFIRMED 결함 없음, F4 제외)
- 교착: 잠금은 `Status.OBD` 모니터 하나뿐. `endCheck()`는 실행자를 잠금 밖에서 돌리고(`ObdArbiter.java:36-44`), 실행자는 `main.post`만 합니다. `startObd`의 바깥 `synchronized (Status.OBD)`와 안쪽 `startServiceOrDefer`는 같은 모니터(재진입). 잠금 안에 차단 IO 없음.
- 잃어버린 시작/끊긴 뒤 시작/이중 시작: 포스트된 실행자가 `link == null || session == null || active != session || obd != null`을 다시 봅니다(`SenderService.java:405`). 세션 종료는 `stopObd`의 `cancelDeferred`(`:426-428`)와 `active != session`이 이중으로 막고, 새 세션이 이미 링크를 쥐면 `obd != null`이 막습니다. 미룬 자리는 하나뿐이고 새것이 덮습니다. 점검이 미룬 시작과 다음 점검이 겹치면 다시 미뤄지는 사슬도 맞게 돕니다.
- 스레드 누수: 미룬 시도마다 시작하지 않은 `ObdLink`/`SppConnector`가 하나 버려질 뿐(스레드 없음). UI 스레드: 미룬 `startObd`는 메인에서 돌지만 `getRemoteDevice`·객체 생성·스레드 시작뿐이고 연결은 링크 스레드에서 합니다.
- 시계: `ObdLink`의 느린 재시도는 주입 시계(`SenderService.CLOCK = elapsedRealtimeNanos`; 시험은 `AtomicLong`, `Clock.SYSTEM = nanoTime`)로 마감을 잡고 `Thread.sleep`은 500 ms 조각으로만 쓰며 매번 시계를 다시 봅니다(`ObdLink.java:189-196`). 가짜 ELM의 지연은 `System.nanoTime`으로 따로 가지만 컨트롤러 시계와 섞이지 않습니다. Doze/절전은 범위 밖입니다: 깊은 잠에서 깨면 `elapsedRealtime`이 진행해 있어 바로 재시도하지만, 포그라운드 서비스의 `Thread.sleep`이 얼마나 미뤄지는지는 측정하지 않았습니다.
- 상태 플래그: A 세션 종료 → `obd = null` → 정지 스레드가 `obd == null`일 때만 `serviceStopped()`(`:435-439`). 그 사이 점검은 `serviceRunning`이 아직 참이라 거절(보수적), 새 세션 B의 시작은 허용되고 `start(obdStopping)`가 전임을 기다립니다. 모두 2차 3.3절과 같습니다.

### 3.5 R4 판정 분리 (CONFIRMED 동일)
- 옛 식 `span >= 30 s && cov >= 0.90 && loss <= 5 && stalls == 0 && restarts <= 1`과 새 `notGoodReasons`(`ObdLossMeter.java:202-210`: `cov < 0.90`, `restarts > 1`, `!(loss <= 5)`, `stalls != 0`, `span < 30 s`)는 조건별 정확한 부정이며, `loss`가 NaN인 가상의 경우도 두 식 모두 GOOD이 아닙니다. MARGINAL/BAD 경계(`cov >= 0.60`)는 그대로. `verdictOf`는 **자르지 않은** 덮개율을 받고(`MainActivity.java:221,225`) 100 % 자르기는 `resultLine`·진행 줄 표시에만 적용됩니다(`:213-214`, `:226`). 기록 `wall_coverage`도 자르지 않은 값.
- 문서 대조 시험: `verdict-cases` 표 9줄을 순서·숫자·결과 단어 접두로 대조하고 각 줄을 `verdictOf`에 다시 넣습니다. 의도적으로 깨지기 쉽습니다(순서 바뀌면 실패, 표식 주석 필요). 빠진 것: 상단 뜻 표(좋음/보통/나쁨 행 문구)는 시험되지 않습니다 — 제가 읽어 코드와 일치함을 확인했습니다("좋음이 아니면서 60 % 이상", "60 % 미만(재시작 횟수와 관계없음)"). 결과 줄 예시 두 개는 문자열 그대로 대조되므로 `(기준 100)` 불일치는 재발하지 않습니다.

### 3.6 느린 재시도 (CONFIRMED 결함 없음, m4 제외)
- 5회 실패 → `gaveUp` → 5분 대기 → 1회 시도 → 실패면 다시 5분(`ObdLink.java:119-128`). 성공하면 `failuresInRow = 0`, `gaveUp = false`(`:135-136`). `stop()`은 `running=false` + 인터럽트로 500 ms 안에 깨웁니다. 포기 전 백오프 2·4·8·16 s(`minBackoff 2000`, 최대 30000)는 변경 없음.

## 4. 손 변이·실험 요약

| 이름 | 바꾼 것 | 결과 |
|---|---|---|
| m1 | `LATE_STOP_FACTOR` 2 → 1 (창 경계) | KILLED (4 FAIL, 39 s) |
| m2 | `lateStopPrompt`의 `atmaSinceStopX &&` 제거 | SURVIVED — 동치 변이(3.1절) |
| m3 | `ObdArbiter.endCheck()`의 `deferred = null` 제거 | SURVIVED — 시험 약점(F3) |
| m4 | `ObdLink` 연결 성공 시 `gaveUp = false` 제거 | SURVIVED — 기록 문구만 영향(F3) |
| e1 | 가짜 ELM: 정지 응답 `\r>` → `\rSTOPPED\r\r>` | 510/4 — `controllerLateStopPromptAtInit`만 실패(F1) |
| e1fix | e1 + `hasReplyText`가 `Kind.STOPPED` 줄 무시 | 514/0 |

반복 5회 + 기준 1회 + e1fix: 7/7 514/0, 29~30 s, 1분 부하 1.3~2.6. 실패한 변이들은 시간 초과를 기다리느라 35~39 s.

## 5. 권장 조치 (우선순위)

1. F1: `hasReplyText`에서 `Kind.STOPPED` 무시(한 줄) + 가짜 ELM에 `STOPPED` 변형을 두고 늦은 프롬프트 시험 둘을 두 변형으로 실행. 설계 13절 "남은 위험"에 "정지 답의 `STOPPED` 줄" 항목 추가.
2. F4: 점검 스레드의 `endCheck()`를 `finally`로.
3. F2: `monitorActive = true`를 `ATMA` 쓰기 앞으로; `stopped` 검사를 `write()` 잠금 안으로(helper `X`만 예외).
4. F3: 아비터 시험을 순수 계수 실행자 + `endCheck()` 2회로; 느린 재시도 시험에 성공 뒤 `gaveUp == false` 단언.
5. F5: 설계 문서의 ATZ 상한을 "루프 2회, `?` 재시도 포함 최대 4회, 최악 6×atzMs"로.

## 6. 한계

- 모든 재현은 호스트 JVM의 가짜 ELM327입니다. 소유자 어댑터가 `STOPPED`를 찍는지, `ATE0`를 지키는지, 폰의 RFCOMM 지연 분포는 측정되지 않았습니다. F1의 차량 발생 조건은 2차 R1과 같이 지연 > `stopMs`(1 s)입니다.
- 상호운용 리그, 전체 `make test`, ARM, APK 빌드, 실제 기기·차량은 돌리지 않았습니다. 안드로이드 연결 코드는 컴파일만 했습니다.
- 저장소에는 아무것도 쓰지 않았습니다. 이 기록과 변이 사본은 `/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/rev3.hCxKWr/`(`var/`, `log/`)에 있습니다.
