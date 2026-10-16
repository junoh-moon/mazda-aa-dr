# OBD yaw 채널 2단계 수정(커밋 9be20d5) 2차 독립 적대 검토 — Fable (2026-10-16)

대상: `9be20d5 fix(phone-imu): OBD yaw stage 2 review fixes …`(14파일). 읽은 것: AGENTS.md, 1차 검토 `validation/OBD_YAW_CHANNEL_REVIEW_FABLE_2026-10-16.md`, 설계 `validation/OBD_YAW_CHANNEL_DESIGN_2026-10-15.md`(12절 포함), `docs/OBD_YAW_CHECK_KO.md`, 수정된 소스 전체(`ElmController`, `ObdLink`, `ObdLossMeter`, `SenderService`, `MainActivity`, `Status`, `SppConnector`, `ElmLineParser`, `SourceSelector`), `ObdTests` 전체, `tools/analyze_logs.py`의 `phone_imu_report`, `src/collector/phone_imu.h`의 `Aggregator`, 벡터 스크립트와 JSON. **차량·폰·어댑터는 쓰지 않았습니다.** 저장소는 읽기 전용으로 두고 `git archive 9be20d5`로 만든 작업 사본(`/home/sixtyfive/.claude/jobs/0a3abdf4/tmp/rev2.l6zrrQ/repo`)에서 실행했습니다. 상호운용 리그는 돌리지 않았고(지시대로), 무거운 실행은 1분 부하 ≤ 2.1에서 했습니다.

표기: **CONFIRMED** = 코드 추적으로 확정했거나 호스트에서 재현함, **PLAUSIBLE** = 추론. 심각도는 차량 안전 > 기록의 해석 오류 > 견고성(자료 수율) > 시험 품질 순입니다.

## 0. 요약

- **차량 안전 불변식은 유지됩니다(CONFIRMED):** 어댑터에 쓰는 바이트는 여전히 `SAFE_COMMAND`를 통과한 AT 명령 + CR, 그리고 단일 `X`뿐입니다. 새 경로(`ATDP`, 정지 때 helper 스레드의 `X`, 프롬프트 비우기, 전체 재초기화, `ATMA` 에코 감지) 모두 `command()`/`write()` 두 함수만 거칩니다. helper 스레드의 `X`는 `write()`가 `synchronized`라 다른 명령과 바이트 단위로 섞이지 않고, 닫힌 소켓에 쓰면 `IOException`으로 삼켜지며, 새 링크의 소켓은 다른 객체라 그쪽으로 갈 수 없습니다(3.1절).
- `src/`와 CMU 와이어 형식은 바뀌지 않았습니다(CONFIRMED: `git diff 9be20d5~1 9be20d5 -- src/` 비어 있음; `Protocol.java`·골든 파일은 커밋 14파일에 없음).
- 1차 검토의 F1~F8은 모두 코드에 반영되어 있고 각 수정에 시험이 붙어 있으며, 제가 만든 손 변이 9개가 **모두** 잡혔습니다(4절). `ObdTests` 5회 연속 443/0(15 s씩, 부하 ≤ 1.1).
- 관측 벡터 5개는 **실제 공개 로그의 바이트가 맞습니다**(CONFIRMED: `/tmp/user3.eTG2tn/raw_can.csv` sha256 일치, k번째 프레임·시각·최대/최소값·계수기 짝 모두 독립 재계산으로 일치). 문서가 "증명하지 않는 것"을 명시해 과장은 없습니다(3.6절).
- 새로 찾은 가장 뜻있는 문제는 **F1 수정이 만든 견고성 저하**입니다(R1, 축소 타이밍으로 호스트 재현): 어댑터→폰 지연이 `stopMs`를 넘으면 stall 뒤 우리 `X`의 프롬프트가 늦게 도착해 "예상 밖 프롬프트"로 분류되고, 전체 재초기화로 가며, 같은 지연 아래서 그 재초기화는 응답이 한 칸씩 밀려(`elm_version` 빈 문자열, `ATDP` 답 = 배너) `init ATE0` 실패로 실행이 끝나 재연결합니다. 이전 코드는 같은 조건에서 맨 `ATMA`로 버둥대긴 했지만 연결은 유지했습니다. 버스에는 무해(AT와 `X`뿐)하고 기본 타이밍(`stopMs` 1 s)에서는 1 s 넘는 BT 지연이 있어야 하므로 차량에서는 PLAUSIBLE입니다.
- 그 밖에는 LOW: 에코를 끄지 못하는 복제칩에서 F1 감지가 무한 재초기화가 되는 경우(R2), 정지 `X`와 뒤따르는 `ATMA` 사이의 좁은 경합(R3, 바이트 섞임 없음), 문서 표와 코드 판정의 표현 차이(R4), 세션당 포기 규칙의 부작용(R5), 점검 중 CMU 재연결 시 그 세션의 OBD 생략(R6).

## 1. 실행한 것 (DATA)

- 작업 사본에서 `javac --release 11 -Xlint:all -Werror`로 core + tests 컴파일, `ObdTests` 1 + 5회: **매번 443 passed, 0 failed**, 벽시계 14.95~15.03 s, 1분 부하 0.65~1.14. `make_obd_vectors.py --check` 통과(17 + 관측 5), Python 스트림 검사 42프레임 −7587 대 −7587.2448.
- `python3 -m unittest discover -s tests/tools -p test_pimu_eval.py`: 6 OK(`test_obd_reset_bins_excluded` 포함).
- 공개 ND 로그 `/tmp/user3.eTG2tn/raw_can.csv`(읽기만): sha256 `18983a366b46d8bea62ea426652798ca24cfef23f932e3751442485938aab54f` 일치. `make_obd_vectors.py observed` 통과("observed pairs ok 5 of 14411"). 독립 재계산(3.6절).
- 손 변이 9개(`ObdTests`로 판정, 4절): 모두 KILLED.
- 검토 하네스 `ReviewExp2.java`(작업 사본 밖 `…/rev2.l6zrrQ/exp/`): FakeElm 앞에 어댑터→폰 바이트를 균일하게 지연시키는 파이프를 끼워 stall 2회를 돌림. 새 core(9be20d5)와 옛 core(6b9c17d)를 같은 하네스로 비교(R1).
- 실행하지 않음: 상호운용 리그, ARM, 전체 `make test`, C++ CMU 빌드, 안드로이드 컴파일, 실제 어댑터·차량, 높은 부하에서의 `ObdTests`.

## 2. 발견 (심각도 순)

### R1. 지연된 프롬프트가 "어댑터 리셋"으로 분류되어 전체 재초기화 → 같은 지연에서 재초기화도 깨짐 — MEDIUM(자료 수율), CONFIRMED(축소 타이밍 호스트 재현) / 차량에서는 PLAUSIBLE

- 위치: `phone/imu-sender/src/me/sixtyfive/mx5imu/core/ElmController.java:229-236`(`interrupt()`는 `stopMs` 동안만 프롬프트를 기다림), `:342-348`(stall → `interrupt()` → `restart("stall")` → `ATMA`), `:310-318`(`afterBufferFull`이 아니면 모든 PROMPT = 리셋 → `return any`), `:186-208`(`transact`의 비우기는 **이미 큐에 있는** 이벤트만 버림; 명령을 쓴 뒤 도착하는 늦은 프롬프트는 그 명령의 답으로 잡힘), `:245-253`(ATZ 응답에 `ELM327`이 있는지 확인하지 않음).
- 시나리오: 모니터 중 프레임이 1 s 끊김(stall; BT 혼잡이 흔한 원인). `X` 송신 → 어댑터는 즉시 `STOPPED`·`>`를 내지만 그 바이트가 혼잡 때문에 1 s(`stopMs`) 넘게 늦게 도착. `interrupt()`가 프롬프트 없이 끝나고 `restart("stall")`이 `ATMA`를 보냄(어댑터는 다시 모니터). 뒤늦게 도착한 `>`는 `afterBufferFull=false`라 "리셋"으로 분류 → `adapterResets++`, 모니터 종료 → `initialise()`: `X` → (늦은) 프롬프트 … → `transact("ATZ")`가 **다른 늦은 프롬프트를 ATZ의 답으로** 받고(`elm_version ` 빈 문자열), `ATDP`가 배너를 받고, `ATE0`가 `ATDP`의 답을 받아 `OK`가 없으므로 `init ATE0: …`로 **실행 종료** → `ObdLink`가 2 s 백오프 뒤 재연결(프레임이 있었으므로 최소 백오프).
- 재현(하네스, FAST 타이밍 `stopMs` 300 ms): 지연 50 ms·200 ms → `starts=[init, stall, stall]`, 100프레임, `adapterResets=0`(정상). **지연 400 ms → `events=[…, elm_stall, elm_unexpected_prompt reinit, elm_version , elm_protocol_after_reset ELM327 V1.5]`, `commands=[…, ATMA, ATMA, ATZ, ATDP, ATE0]`로 끝남, 51프레임, `inits=2`, `adapterResets=1`.** 같은 하네스에서 옛 core(6b9c17d)는 `starts=[init, stall, prompt, prompt, question, prompt, prompt, init]`, 100프레임: 맨 `ATMA` 재시작 폭풍(그 `A`가 정지 문자로 먹혀 `TMA`→`?`)을 거쳐 폭풍 재초기화로 수습했습니다. 즉 옛 코드도 이 조건에서 깨끗하진 않았지만 **연결을 유지한 채 복구**했고, 새 코드는 **실행을 끝내고 재연결**합니다.
- 차량 영향: 기본 타이밍이면 어댑터→폰 지연 > 1 s가 필요합니다. 폰이 CMU(HFP + 50 Hz RFCOMM)와 무선 AA 동글을 동시에 잡은 상태의 RFCOMM 지연은 미측정이라 PLAUSIBLE로 둡니다. 버스에는 무해합니다(쓴 것은 `X`, `ATMA`, `ATZ`, `ATDP`, `ATE0`뿐). 기록 전용 채널의 수율과 `obd_run_end why=init ATE0…` 같은 혼란스러운 기록이 문제입니다. 1차 검토 F6의 "두 번째 `>`가 다음 명령을 쓴 뒤 도착하면 여전히 잡힌다"(설계 12절 '남은 위험')와 같은 뿌리이며, F1 수정이 그 경로에 들어가는 빈도를 높였습니다.
- 제안(작음): (a) 우리가 `X`를 쓴 시각을 기억하고, 그 뒤 `2×stopMs` 안에 오는 PROMPT는 "우리 정지의 답"으로 보아 `restart`(맨 `ATMA`; 설정은 온전함)로 처리하고, 그 밖의 PROMPT만 리셋으로 처리하십시오. (b) `transact("ATZ")`의 답에 `ELM327`(또는 어떤 TEXT 줄)이 없고 프롬프트만 있으면 한 번 더 기다리거나 ATZ를 다시 보내십시오(이미 `?`에는 재시도함). (c) 하네스처럼 FakeElm 앞에 지연 파이프를 끼운 시험 1개(지연 > `stopMs`)를 두면 (a)(b)가 검증됩니다.

### R2. 에코가 꺼지지 않는 복제칩에서 `ATMA` 에코 = 리셋 → 모니터가 영영 시작되지 않음 — LOW-MEDIUM, PLAUSIBLE(코드 추적; 재현 안 함)

- 위치: `ElmController.java:254-263`(`ATE0`는 `OK`가 있으면 통과; 에코가 실제로 꺼졌는지 보지 않음), `:333-337`과 `:361-364`(TEXT 줄이 `ATMA`로 끝나면 리셋).
- 시나리오: 모든 명령에 `OK`를 답하지만 `ATE0`를 구현하지 않은(에코 유지) 염가 복제칩. 초기화는 통과하고, `ATMA`의 에코 줄이 첫 이벤트로 와서 즉시 `return false` → `idleReinits` 1, 2, 3 → `IOException("no_traffic after 3 inits")` → 백오프(프레임 0이라 두 배씩 30 s까지) → 재연결 → 같은 일 반복. **이전 코드는 이런 칩에서 정상 동작했습니다**(에코 줄은 TEXT로 세기만 함). 소유자의 JF327이 `ATE0`를 지키는지는 기록에 없습니다(`OBD_OWNER_SETUP`은 `ATSP6` 응답만).
- 제안: 초기화 중 에코가 꺼졌음을 확인한 경우(예: `ATL0` 이후 응답에 명령 에코 TEXT 줄이 없음)에만 `ATMA` 에코를 리셋 신호로 쓰고, 아니면 `elm_echo_persists`를 기록하고 TEXT로만 세십시오. 또는 리셋 판정에 두 가지 신호(에코 + 프레임 없음 등)를 요구하십시오.

### R3. 정지 `X`와 컨트롤러의 다음 `ATMA` 사이의 경합: 어댑터가 ATMA 상태로 남을 수 있음 — LOW, CONFIRMED(코드)

- 위치: `ElmController.java:102-116`(`stop()`: `stopped=true` 뒤 helper 스레드가 `X`), `:280-283`(`poll` 직후의 `if (stopped) break`), `:367-381`/`:267-272`(`restart`→`startMonitor`→`command("ATMA")`에는 `stopped` 검사 없음), `:176-179`(`command()`도 검사 없음).
- 시나리오: 컨트롤러 스레드가 BUFFER FULL 프롬프트를 `poll`로 받고 `:283`의 검사를 지난 직후 `stop()`이 호출되면, helper의 `X`가 먼저 나가고 그 뒤에 `ATMA\r`가 나가 어댑터는 모니터 중으로 남습니다(F5 이전 상태; 다음 앱의 첫 글자가 먹힘). 창은 수 µs~스케줄링 지연 한 번이라 드뭅니다. **바이트 섞임은 없습니다**: `write()`가 `synchronized`(`:168`)라 `X`와 `ATMA\r`는 각각 통째로 나갑니다. 변이 M7(`:283`의 검사 제거)은 시험이 잡았지만(4절) 이 좁은 창 자체는 시험으로 못 잡습니다.
- 제안: `command()` 첫 줄에 `if (stopped) throw new IOException("stopped");`.

### R4. 문서 표·결과 줄과 코드 판정의 표현 차이 — LOW, CONFIRMED

- `docs/OBD_YAW_CHECK_KO.md:26-28` 대 `ObdLossMeter.java:175-182`: `GOOD` 조건은 정확히 일치합니다(덮개율 ≥ 90 %, 재시작 ≤ 1, 사슬 안 손실 ≤ 5 %, 첫 프레임부터 ≥ 30 s, 500 ms 넘는 끊김 0). **`MARGINAL` 행은 부정확합니다**: 문서는 "덮개율 60~90 % 또는 재시작 2회 이상"이라 하지만 코드는 "GOOD이 아니고 덮개율 ≥ 60 %"입니다. 따라서 (a) 덮개율 ≥ 90 %인데 손실 > 5 %, 끊김 > 500 ms, 또는 측정 < 30 s이면 코드는 MARGINAL인데 문서 행에 그 사유가 없고, (b) 재시작 ≥ 2이면서 덮개율 < 60 %이면 코드는 BAD인데 문서의 MARGINAL 행 문구("재시작 2회 이상")와 어긋납니다. 소유자가 표로 판정을 해석하므로 행 문구를 "좋음이 아니면서 덮개율 60 % 이상(사유: 60~90 %, 재시작 2회 이상, 손실 5 % 초과, 0.5 s 넘는 끊김, 30초 미만 측정)"처럼 코드와 같게 고치십시오.
- 결과 줄: 코드(`MainActivity.java` `runObdCheck`, 결과 포맷 줄 `"점검 결과: %s · 덮개율 %.0f%%(기준 100) · …"`)에는 `(기준 100)`이 있고 문서 예시(`:22`)에는 없습니다. 사소합니다.
- 덮개율은 100 %를 넘을 수 있습니다(`frames`에 첫 프레임이 포함되어 N초에 50N+1개; 진행 중 줄은 첫 프레임 직후 수천 %). 판정에는 영향 없고(≥ 90 % 비교) 표시만의 문제이니 `min(1, …)`로 자르는 편이 소유자 혼란을 줄입니다.
- 수식 자체는 확인했습니다: 0으로 나눔 없음(`wallSeconds ≤ 0 → 0`, `frames == 0 → NONE`), 30 s 미만은 GOOD 불가(시험 있음), E8 입력은 MARGINAL(덮개율 0.667, 재시작 20; 시험 있음), 재시작은 `chainBreaks`(프레임이 있던 사슬이 끊길 때만 증가; 첫 모니터 시작은 세지 않음; 전체 재초기화도 셈 — 문서의 "재시작"과 같은 뜻).

### R5. 세션당 연결 포기 규칙의 부작용 — LOW, PLAUSIBLE

- 위치: `ObdLink.java:110-125`(연속 실패 5회 → `return`; `failuresInRow`는 연결 성공에서만 0), `SenderService.java:385-408`(CMU `connected()`마다 새 링크), `docs/OBD_YAW_CHECK_KO.md:32`(뽑아 둔 경우만 설명).
- 시나리오: 연결 **시작 직후**가 연결 실패가 가장 잦은 때입니다(폰 BT가 CMU HFP/RFCOMM과 무선 AA 동글을 동시에 세우는 중, Car Scanner 자동 연결이 어댑터를 잡고 있음, 어댑터가 아직 부팅 중). 2·4·8·16 s 뒤까지 5번 실패하면 **그 주행 내내**(세션이 몇 시간이어도) OBD를 다시 시도하지 않으며, Car Scanner를 닫거나 어댑터가 복구되어도 그렇습니다. 기록에는 `obd_connect_giveup` 한 줄만 남습니다. 1차 검토 F3의 제안 범위 안이고 안전 문제는 아니지만, 2단계의 목적(주행 기록 수집)을 한 번의 나쁜 시작이 통째로 지울 수 있습니다.
- 또한 `maxConnectFailures`는 **연결 실패만** 셉니다. 연결은 되지만 초기화가 매번 실패하는 어댑터(`init ATZ: no prompt` 등)는 이전처럼 30 s 백오프로 무한 재시도합니다(변경 없음, 문서와 일치).
- 주차 점검에서 포기하면 상태 문자열이 `OBD 연결 포기 · 다음 CMU 연결 때 다시 시도`로 표시됩니다(`ObdLink.java:117`; 점검은 CMU와 무관) — 문구만의 문제.
- 제안: 포기 뒤에도 느린 재시도(예: 5~10분마다 1번)를 두거나, 1차 검토가 제안한 "분 단위 최대 백오프" 쪽이 소유자 기록에는 더 안전합니다. 문서에 "Car Scanner가 잡고 있어도 5번 뒤 멈춘다"를 적으십시오.

### R6. 점검 중 CMU가 (재)연결되면 그 세션은 OBD 없이 감 — LOW, CONFIRMED(코드)

- 위치: `SenderService.java:401-404`(`obdCheckRunning`이면 `obd_skip check_running` 뒤 **그 세션에서는 다시 시도하지 않음**), `Status.java:23-33`.
- 문서 절차(송신 끄기 → 점검 → 송신 켜기)를 따르면 일어나지 않습니다. 송신을 켠 채 점검을 눌렀고 60 s 안에 CMU가 끊겼다 붙으면(BT 흔들림) 다음 CMU 재연결까지 OBD가 없습니다. 원자성 자체는 맞습니다(두 쪽이 같은 `OBD_LOCK` 안에서 검사-후-설정; 잠금 안에 차단 IO 없음).

### R7. 기타 LOW (결함이라기보다 메모)

- 전체 재초기화 루프에는 상한이 없습니다(`run()`: 프레임이 있던 모니터는 `idleReinits`를 0으로 되돌림). 이전에도 같았고(폭풍 → 재초기화) 이제는 한 바퀴마다 `ATZ`(약 1 s) + 9명령이 추가될 뿐입니다. 기록 줄은 바퀴당 4줄쯤이며 `RollingLog`가 1 MiB 상한이라 무한히 자라지 않습니다.
- `transact`의 비우기 뒤 **`ATZ` 응답의 `version`에 에코가 섞입니다**(`elm_version ATZ|ELM327 V1.5`; 리셋 직후는 에코 켬이라 정상). 기록 해석에만 참고.
- 분석기: `reset_m`에는 어느 소스든 비트 3 묶음이 들어가므로 자이로 쪽 stale 재개나 세션 첫 창의 `frame_reset`도 이웃 OBD 묶음을 제외합니다(보수적, 설계 문구 "어느 소스든"과 일치). ±1000 ms 이웃 판정은 한 CMU 링크 세션 안의 1 s 격자(`Aggregator::advance`가 `t0_ += rows*10 s`)에서 정확하고, 세션 경계(새 `t0_`)를 넘는 이웃은 격자가 달라 짝이 안 맞을 수 있는데, 세션 첫 OBD 묶음은 스스로 비트 3을 가져 제외되므로 실질 영향은 없습니다. OBD 묶음이 없으면 `obd` 키 자체가 생기지 않아 기존 보고서는 그대로입니다(코드 `if obd["bins"]:` 안).

## 3. 결함을 찾지 못한 영역 (무엇을 어떻게 보았는지)

### 3.1 차량 안전: 쓰기 경로 (CONFIRMED 결함 없음)

- 쓰기 호출은 세 곳뿐입니다: `command()`(`:176-179`, `SAFE_COMMAND = AT[A-Z0-9 ]{1,16}` 전체 일치 + `\r`), `interrupt()`(`:230`, `"X"`), `stop()`의 helper(`:107`, `"X"`). 새 `ATDP`(`:251`)와 재초기화의 모든 명령은 `transact`→`command`를 거칩니다. 맨 CR, 16진만의 줄, OBD 요청을 만들 수 있는 조합은 없습니다. `onlySafeWrites`가 모든 컨트롤러 시험에서 이를 단언합니다.
- **helper 스레드의 `X`와 다른 명령의 섞임:** `write()`가 `synchronized`(`:168-174`)라 `X`(1바이트)와 `ATMA\r`는 각각 원자적으로 나갑니다. 순서는 보장되지 않지만(R3) 어느 순서든 어댑터에 가는 것은 `X` 뒤 `ATMA\r` 또는 그 반대이며, 어느 쪽도 버스 요청이 아닙니다. 어댑터가 `X`를 명령 버퍼에 남기는 칩이면 다음 명령은 `XATZ`→`?`(이미 재시도 처리).
- **닫힌 소켓·새 링크의 소켓에 쓰기:** helper는 자기 `ElmController`의 `out`(옛 `BluetoothSocket`의 스트림)만 잡습니다. `stop()`은 200 ms 뒤 `transport.close()`를 무조건 부르고, 그 뒤 helper의 `write`는 `IOException`(삼킴)으로 끝납니다. 새 링크는 새 `SppConnector`→새 `BluetoothSocket`→새 스트림 객체라 옛 helper가 그 스트림에 쓸 길이 없습니다. 커널 fd 번호 재사용 경합(fd 정수를 읽은 뒤 syscall 전에 선점 + 닫힘 + 새 연결이 같은 번호를 받음)은 이론상 µs 창이며 새 RFCOMM 연결은 페이징에 초 단위가 걸리고 `PREDECESSOR_WAIT_MS` 4 s가 앞에 있어 실질적으로 불가능합니다; 설령 일어나도 들어가는 바이트는 `X` 하나라 어떤 위치에 끼어도 `AT…`에 `T`가 남아 `?`가 될 뿐 요청이 되지 않습니다.
- **잠금 보유 중 차단 IO:** helper가 멈춘 BT 쓰기에서 컨트롤러 모니터 잠금을 쥔 채 서 있어도, 컨트롤러 스레드는 `poll`에서 `stopped`로 빠져나와 `write`를 더 부르지 않고, `stop()`의 `close()`가 멈춘 쓰기를 풀어 줍니다(안드로이드 `BluetoothSocket.close`). `written()`(`synchronized`)은 제품 코드에서 부르지 않습니다. 교착 없음. 시험 `controllerStopSendsX`의 "쓰기가 영원히 막히는 전송" 케이스도 통과.
- **비우기(drain)가 정당한 응답을 먹는가:** `transact`는 명령을 **쓰기 전에** 큐에 있는 것만 버립니다(`:187-194`). 인과상 그 명령의 응답은 그 뒤에만 올 수 있으므로 먹히지 않습니다. 리더의 `<closed>` 표식은 보존됩니다. 비우기가 보호하지 못하는 경우(명령을 쓴 **뒤** 도착하는 늦은 프롬프트)가 R1입니다.
- `ATMA` 거부(`?`)는 모니터당 1회 재시도 뒤 재초기화, 3회 프레임 없음이면 종료(시험 `controllerAtmaAlwaysRefused`). `ATZ` 차단 시간: `transact("ATZ", 5 s)`가 100 ms 단위로 `stopped`를 보므로 `stop()`은 즉시 반영됩니다.

### 3.2 F1 규칙의 거짓 양성 (CONFIRMED: 정상 운용에서는 없음)

- `adapterResetLine`은 `default:` 가지(TEXT·OK·STOPPED)에만 적용됩니다. CAN 프레임 줄은 16진만이라 `L`,`V`,`M`,`T`,`R`,`S`를 포함할 수 없어 `LV RESET`/`ELM327`/`ATMA`와 절대 일치하지 않습니다(프레임 유래 줄의 거짓 양성 불가). 모니터 중 나올 수 있는 ELM 문구(`SEARCHING...`, `BUS BUSY`, `CAN ERROR`, `STOPPED`, `NO DATA`, `ACT ALERT`, `LP ALERT`, `<RX ERROR`)는 모두 불일치입니다. 남는 거짓 양성은 R2(에코를 끄지 못하는 칩)뿐입니다.
- "진짜 BUFFER FULL 뒤의 프롬프트"는 `afterBufferFull`로 재시작 경로에 남고, CAN 프레임이 사이에 끼면 플래그가 지워져 재초기화로 가지만(`:287`), 실제 ELM은 `BUFFER FULL` 뒤에 프레임을 내지 않으므로 과잉 반응일 뿐 위험은 없습니다. 폭풍 계수기(`restart()`만 증가, 6회/10 s → 재초기화, `restartIdx` 0으로)와 F1 재초기화는 서로 독립이며, F1 경로는 폭풍 계수기를 건드리지 않습니다. 3회 프레임 없음 종료(`MAX_REINITS`)는 F1 재초기화에도 그대로 적용됩니다(프레임이 있었으면 0으로 되돌림: R7).
- 변이 M7의 부수 관찰: `stop()`이 보낸 `X`의 프롬프트를 "예상 밖 프롬프트"로 오인하지 않는 것은 오직 `:283`의 `if (stopped) break` 덕분입니다. 이 검사를 빼면 BUFFER FULL 폭풍 시험까지 실패했으므로(`storm: BUFFER FULL prompts are not adapter resets`), 이 줄을 리팩터링에서 지우지 마십시오.

### 3.3 F3·F8: 포기 계수, 전임 링크 대기, 잠금 (CONFIRMED 결함 없음; 부작용은 R5·R6)

- `failuresInRow`는 `loop()`의 지역 변수라 링크(=CMU 세션)마다 0에서 시작하고, 연결 성공에서 0으로 돌아갑니다(시험 `flaky adapter`). 포기 뒤 `running`은 true로 남지만 링크는 세션마다 새로 만들어지고 `stop()`은 정상 동작합니다.
- **4 s 대기는 새 링크의 자기 스레드(`obd-link`)에서만** 일어납니다(`ObdLink.java:95-103`). `SenderService.startObd()`(CMU 링크 스레드의 `connected()`에서 호출)는 `o.start(obdStopping)`로 스레드를 만들 뿐이고, `stopObd()`도 `obd-stop` 스레드를 띄울 뿐입니다. UI 스레드·CMU 세션 스레드는 막히지 않습니다. `ObdLink.stop()`은 `X` 200 ms + join 3 s ≤ 3.3 s라 4 s 안에 끝나며, 넘으면 `obd_predecessor_still_running`을 남기고 진행합니다(그때 겹치면 새 연결이 한 번 실패 → 5회 중 1회 소모).
- `Status.OBD_LOCK`은 플래그 검사-후-설정만 감싸고(`SenderService.java:401-405`, `:417-422`, `Status.java:23-33`), 잠금 안에 소켓·조인·로그 외 차단 호출이 없어 교착이 없습니다. `stopObd()`가 `obd=null`로 만든 뒤 `obdServiceRunning`을 늦게 내리는 창에서는 점검이 "사용 중"으로 거절됩니다(안전한 방향). 새 세션의 `startObd()`가 그 창에 들어와도 `obd = o`를 잠금 안에서 먼저 두므로 `obd-stop` 스레드가 플래그를 내리지 않습니다.
- `MainActivity.runObdCheck`의 링크(`maxBackoff` 4 s, 포기 5회)는 60 s 점검 안에서 포기할 수 있고 판정은 `NONE`("0x079 수신 없음")으로 떨어집니다.

### 3.4 F2 수식 (CONFIRMED 결함 없음; 표현은 R4)

- `wallCoverage(endNs) = frames / (wallSeconds × 50)`, `wallSeconds = max(0, endNs − firstEverNs)/1e9`, `≤ 0 → 0`. 분모 0 없음. `endNs`는 `link.stop()` **전에** 잡습니다(점검 끝). 아주 짧은 구간(첫 프레임 직후)은 덮개율이 1을 크게 넘어 `MARGINAL`이 되는데 30 s 조건 때문에 GOOD은 불가능합니다. 경계 90/60 %는 코드·설계·문서가 같습니다.

### 3.5 분석기 (CONFIRMED 결함 없음)

- `reset_m`·`excluded`·`reset_bins`·`eval_bins` 계산을 손으로 따라갔고 시험의 8묶음 사례(51000·52000·56000)와 일치합니다. 묶음 경계: `m + 1000*i`는 한 CMU 세션 안에서 1 s 격자(`Aggregator::advance`, `phone_imu.h`), 빈 행을 건너뛰어도 격자 유지. `eval_excluded_m`은 200개로 자르지만 `eval_excluded_bins`는 전체 수. OBD 묶음이 없으면 키가 생기지 않습니다(R7 메모 참조).

### 3.6 관측 벡터 (CONFIRMED: 실제 값이며 과장 없음)

- 파일 `/tmp/user3.eTG2tn/raw_can.csv`(1차 조사 때 위치, 읽기만): sha256 일치, 0x078·0x079 각 14,411프레임.
- k = 100·13499·9752·8233·6264의 0x078/0x079 바이트가 JSON과 **바이트 단위로 일치**하고, 로거 시각도 일치(`make_obd_vectors.py observed` 통과 + 독립 CSV 파싱). 독립 재계산: `raw079` 최대 2927은 정확히 k=13499, 최소 1737은 k=9752(주장대로 "최대·최소"). 다섯 쌍의 0x079−0x078 시각 차는 4.9~5.0 µs. 계수기(0x078 b6 상위 nibble = 0x079 b6 하위 nibble)는 다섯 쌍은 물론 **14,411쌍 전부** 일치. k=100 부근(k 50~150)의 raw079는 2045~2050으로 정지(영점 근처)와 부합합니다.
- 순번 짝짓기 자체의 한계: 전체 로그에서 k번째끼리의 시각 차 중앙값 5.0 µs, p99 6.3 ms, 최대 128.6 ms(로거 배치/공백). 선택된 5쌍은 모두 5 µs라 문제없지만, "k번째끼리 = 같은 순간"은 일반론으로는 성립하지 않으므로 벡터 설명의 "k-th with k-th"라는 표현이 정확하고 그 이상을 말하지 않는 것이 맞습니다.
- 문서(`make_obd_vectors.py` 머리말, 설계 12절, `ObdTests.observedVectors` 주석)는 "격자 일치·격자 차이 0~3·계수기 짝만 보여 주고 배율·부호·다른 바이트·소유자 차량은 증명하지 않는다"고 적었습니다. 정확합니다. 덧붙일 점 하나: 격자 상수 `2047.00190`·`1.52751423` 자체가 같은 로그에서 맞춘 값이라(YAW_DECODE README가 인정) "격자 일치"는 같은 자료 안의 자기 일관성이지 독립 검증이 아닙니다. 벡터 JSON의 `cw_dps`는 Java·Python 두 구현의 일치 확인입니다.

### 3.7 시험 (4절과 함께)

- 5회 연속 443/0, 각 15.0 s, 부하 ≤ 1.14. 타이밍에 민감한 시험(`controllerStopSendsX`의 `ms < 500`, `linkWaitsForPredecessor`, `linkConnectGiveUp`의 `≥ 105 ms`, `controllerAtmaAlwaysRefused`의 15 s join)은 **높은 부하에서는 돌리지 않았습니다**(지시대로). 커밋 기록이 리드 재실행을 요구하므로 그때 부하를 함께 적으십시오.

## 4. 손 변이 (모두 KILLED)

| 변이 | 잡은 시험 |
|---|---|
| M1 PROMPT를 항상 재시작으로(F1 이전 동작) | `PROMPT: the first command after the reset is ATZ…` 외 6건 |
| M2 `ATMA` 에코 감지 제거 | `silent reset: …` 2건 |
| M3 `transact` 비우기 제거 | `double prompt after ATZ…`(`init ATE0: ATDP|AUTO`) 2건 |
| M4 덮개율을 사슬 경과 시간으로(옛 맹점) | `E8: wall coverage 1.01…` 1건 |
| M6 정지 `X` 미송신 | `normal stop: last write is the stop character` 2건 |
| M7 `poll` 직후 `stopped` 검사 제거 | `RESET/BANNER/PROMPT: adapter reset counted: 2`, `storm: … not adapter resets` 4건 |
| M8 `STALE_NS` 0.5 → 5 s(1차 검토의 생존 변이) | `newest 0.6 s old with >= 80 frames in 2 s: back to PHONE` 1건 |
| M9 폭풍 검사 제거 | `storm: …` 3건 |
| M10 연결 포기 제거 | `absent adapter: …` 5건(83회 시도) |

시험이 **못 잡는 것:** R1(지연된 프롬프트; FakeElm은 지연이 없음), R2(에코 유지 칩; FakeElm은 `ATE0`를 지킴), R3의 µs 창. 세 경우 모두 가짜 ELM에 "지연 파이프"와 "ATE0 무시" 옵션을 더하면 시험이 됩니다.

## 5. 권장 조치 (우선순위)

1. R1: 우리 `X` 뒤 `2×stopMs` 안의 PROMPT는 재시작으로, 그 밖만 리셋으로. `ATZ` 응답에 배너가 없으면 재시도. 지연 파이프 시험 추가.
2. R2: `ATMA` 에코를 리셋 신호로 쓰는 조건에 "에코가 꺼진 것을 확인함"을 추가(아니면 기록만).
3. R4: 문서 MARGINAL 행과 결과 줄 예시를 코드와 같게; 덮개율 표시 100 % 상한.
4. R3: `command()`에 `stopped` 검사 한 줄.
5. R5·R6: 포기 뒤 느린 재시도 또는 분 단위 백오프; 문서에 Car Scanner 점유 사례와 점검 중 재연결 사례를 추가.

## 6. 한계

호스트 JVM 재현과 코드 추적뿐입니다. R1의 차량 발생 여부는 폰의 RFCOMM 지연 분포(미측정)에 달려 있고, R2는 소유자 어댑터의 `ATE0` 준수 여부(미기록)에 달려 있습니다. 상호운용 리그·ARM·전체 `make test`·안드로이드 빌드·실제 어댑터·차량은 실행하지 않았습니다. 발행·설치·주행을 승인하지 않으며, 이 검토가 차량 검증을 대신하지 않습니다.
