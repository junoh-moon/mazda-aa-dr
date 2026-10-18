# G3 지하주차장 출구 방위 수정 2차 독립 적대 검토 (Fable, 2026-10-18)

대상: `c75053c`(HEAD, "fix(g3): garage-exit carry second round", 15개 파일). 1차는 `c1f00f4`, 1차 검토는 `validation/G3_GARAGE_EXIT_FIX_REVIEW_FABLE_2026-10-18.md`, 기록은 `validation/G3_GARAGE_EXIT_FIX_2026-10-17.md`("독립 검토 반영" 절과 AGENTS.md 문안). 소유자 저장소는 읽기만 했고, 빌드·실행은 `git clone` 뒤 HEAD를 체크아웃한 스크래치 사본(`(scratch)`)에서만 했습니다. 차량·폰·DHU·ARM/QEMU는 실행하지 않았습니다. 표시: **FACT**(코드·로그·실행에서 직접 확인), **INFERENCE**(해석·모델). 줄 번호는 HEAD 파일 기준입니다.

## 요약

- 전달 계약(바이트 40, 44..47만, BETA, 창 안, 실패 시 순정 그대로)을 깨는 새 경로는 **찾지 못했습니다**. 새 계수 `G3Motion::reverse_unsure`는 파이프라인 안에서 **증가만** 하고(`pipeline.cpp:221, 327, 1107, 1112, 1150`), 읽는 곳은 `g3_send.h`의 창 규칙(206, 301)과 진단 행(319, 337)뿐입니다(FACT, `grep`). 1차 검토의 C1·C2·C3는 코드에서 고쳐진 것을 확인했습니다.
- **(C1, CONFIRMED)** 교체 중 S 요청 30 s 간격(P3 수정)이 1차 설계의 안전 장치 하나를 없앴습니다. 1차에서는 출발 표시(M)가 못 들어가도 **최종 주차의 S가 앞의 S를 덮어** 이월 방위를 바로잡았는데, 이제 교체 중에는 앞의 S 요청에서 30 s 안의 최종 주차 S가 `replacing_rate`로 막히고, 표시는 정차 5 s가 지나면 더 시도하지 않으므로(`g3_heading.h:1261`), 표시가 이동 중(과 정차 첫 5 s)에 막혔으면 **앞 정차(예: 차단기)의 S가 플래그 4·S·방위 있음으로 남아** 다음 시동에 `chain_depart`로 이월됩니다. 기록·주석의 "막힌 최종 주차는 디스크에 표시를 남긴다"(`g3_heading.h:1292`, 기록 P3 절)는 표시가 들어갔을 때만 참입니다. 스크래치 시험으로 재현했고 같은 입력에서 30 s 규칙을 빼면(1차 동작) 최종 S가 써집니다(아래 "실행").
- **(C2, CONFIRMED)** 시간 항 0.0023 rad/s의 사전 등록 산술(3.40 카운트 × 0.000658615 = 0.00224 → 0.0023)은 맞지만, 비공개 trace에서 같은 규칙(정차 ≥ 5 s, 4095 표지 제외, 2048 기준)으로 다시 세면 **93개가 아니라 111개**이고 RMS는 3.29~3.31(0.0022로 올림)입니다. 어떤 trace·행을 썼는지 적혀 있지 않아 재현되지 않습니다. 선택된 0.0023은 제 어떤 변형보다 **보수적**이라 안전 쪽이며, 결함이 아니라 기록 문제입니다.
- **(C3, CONFIRMED, 시험)** 제가 만든 변이 중 두 개가 살아남았습니다: M3(규칙 F 오염 재기록을 30 s 간격의 예외에서 뺌)과 **M4(파이프라인의 규칙 F 결정이 `reverse_unsure`를 읽음: test_g3 1,314, navigation 3,977, test_model_session_input, 합성 재생 모두 통과)**. "어떤 파이프라인 결정도 계수를 읽지 않는다"는 성질은 코드 읽기로만 보장되고 시험·재생이 고정하지 않습니다. 또 `tools/replay_beta.cpp`는 `writer_lag_ns`를 채우지 않아(679~683행) 재생은 표시의 100 ms 규칙을 모델링하지 않습니다.
- PLAUSIBLE: 세지 않는 정보 손실 경로 둘(거부가 선행하지 않은 원천 epoch 변경, 워커가 채널을 받기 전), 같은 부팅 안의 서비스 재시작에서의 토큰 교체(효과 손실·주행 중 쓰기), 60 s fsync 재기록 뒤 GPS 상실의 C1 변형, 토큰이 못 잡는 "워커가 swap 전에 끝난" 부팅. 문구 보완 목록은 (6)에 있습니다.
- g3_cancel·g3_garage 흔들림: 부하 ≤ 3에서 각 15회 반복(아래 "실행"). 새 코드가 두 시나리오에 더한 것은 시작 시 토큰 파일 쓰기 하나와 파이프라인 계수(증가만)뿐이고 두 시나리오는 표식 부팅이므로(`test_worker_beta.cpp:262`), 새 스레드 상호작용은 없습니다(FACT). 실패한 단언(`!lagging && max_summary_gap<1.6 s`, 1095·1104·1135·1159행)은 저널 기록 스레드의 CPU 몫에 대한 시간 단언입니다.

판정: 차량 설치 전에 고치거나 적어야 할 것은 C1(30 s 규칙과 표시의 상호작용: 코드 또는 최소한 문구·분석기 계수)과 C2·C3(기록·시험)입니다. 전달 계약 위반은 없습니다.

## 실행 (스크래치 사본, FACT)

1분 부하 0.9~2.3. 빌드는 `nice 10`. 전체 `make test`와 ARM 스위트는 저자가 돌렸으므로 중복하지 않았습니다.

| 항목 | 결과 |
|---|---|
| `build/test_g3` | 1314 검사 통과 |
| `build/test_model_session_input` | 통과(배제 4사례 `reverse_unsure` 1/0/1/0) |
| `tests/tools/test_analyze_g3.py` | 10개 통과 |
| `tests/replay/test_replay_g3_garage.py` (on/off/gap) | 5개 통과 |
| 탐침 `review2_stale_s` (test_g3 하네스 복사본, 제품 시험 아님) | 교체 중: 1 s 출발, 2~6 s 정차 → S1(6 s 요청). 7~17 s 이동, 표시는 7~17 s `journal_draining`, 18~22 s `writer_lag_ns` 150 ms로 막힘, 18 s부터 최종 정차(22 m 더 간 자리), 30 s에 시동 끔. **HEAD: 30 s에 디스크 = S1(세 번째 쓰기 없음), 37 s에야 S2.** 표시가 막히지 않으면 18 s에 M, 36 s에 S2. **같은 입력에서 `replacing_rate`의 `return`만 제거(1차 동작): 23 s에 S2가 써져 디스크 = 최종 주차 S** |
| 탐침 `review2_short_reposition` | S1 6 s, 7~9 s 4.5 m 기어감(5 s 간격 안이라 표시 불가), 10 s 정차: 11 s에 M(정차 첫 5 s 안에 표시 시도), 36 s에 S2. 안전 쪽(디스크 M) |
| 변이(각각 단독 적용, 별도 바이너리) | M1 20 m 문턱을 "움직였으면"으로: test_g3 1648행 **잡힘**. M2 직전 부팅 비교를 앞 8자만: test_g3 통과(1314), 재생 시험 **잡힘**(합성 부팅 id의 앞 8자가 같아서). M3 규칙 F 오염 재기록도 30 s 간격 적용: test_g3 1314 **생존**. M4 규칙 F 가정 조건에 `g3_.reverse_unsure` 추가(파이프라인이 계수를 읽음): test_g3·test_navigation(3977)·test_model_session_input·재생 **모두 통과(생존)**. M5 계수 증가 전부 제거: test_g3 1771행 **잡힘** |
| 요 영점 재계산 | 비공개 trace 18개 파일(10-10 trip6·beta7, 10-11 trip7~10, 10-17 내보내기; 같은 행은 한 번만), `yaw_stop` 고유 행 141, `dur_ms ≥ 5000`이고 4095 표지(평균 1535 카운트 1행) 제외: **n = 111, 평균 −1.31, sd 3.03, RMS 3.29 카운트 = 0.00217 rad/s**. `pre` 있는 행만: n = 90, RMS 3.17(0.00209). `first/last` 있는 행만: n = 109, RMS 3.31. 어느 것도 93·3.40이 아닙니다 |
| `test_worker_beta g3_cancel` × 15, `g3_garage` × 15 (부하 ≤ 3 대기 뒤 순차, 각 약 79 s) | (아래 "흔들림 반복" 절) |

## CONFIRMED (코드 사실; 손해는 조건부)

### C1. 교체 중 30 s S 간격이 "최종 주차의 S가 앞 S를 덮는" 안전 장치를 없앱니다 — `g3_heading.h:1261, 1292-1296, 1325, 1339-1342`

- FACT: 출발 표시 `depart()`는 `store()`에서 `!parked`일 때만 불립니다(1261행). `parked`는 정확히 0인 정차 5 s(garage)이므로 표시는 **이동 중과 정차 첫 5 s 안**에만 시도됩니다. 표시는 (a) 직전 요청에서 5 s(`G3_WRITE_MIN_NS`, 1339행, S 요청과 공유), (b) idle 규칙(`io_block`: 드레인·지연 가드·writer_busy 300 ms·저장 스레드 없음), (c) 교체 중 기록 스레드 지연 ≤ 100 ms(1342행)를 모두 통과해야 합니다. 정차 5 s 뒤의 S 요청은 교체 중이면 직전 **S 요청**(`last_s_request_ns_`, 1325행; 취소·실패한 요청도 포함)에서 30 s가 지나야 합니다(1292~1296행).
- 따라서 교체 중 "S1 → 표시 못 함 → 30 s 안의 최종 주차 → 시동 끔"이면 디스크에 S1이 남습니다. 1차(`c1f00f4`)에서는 최종 주차의 S2가 5 s 간격만 지나면 써져 S1을 덮었습니다(탐침으로 확인). 기록의 "마지막 주차의 S는 … 기다리고, 그 사이 시동을 끄면 디스크에는 표시(M)가 남아 이월하지 않습니다"와 주석 "A blocked final park leaves the mark on disk"는 **표시가 들어갔을 때만** 참입니다.
- 조건의 현실성(INFERENCE): 표시를 막는 것은 GPS 등급 전환마다의 RAW 창 드레인(1차 검토 P2: 사건당 약 7~8 s, 지하 진입이 바로 그 순간), 저널 지연 가드, 그리고 새 100 ms 규칙입니다(기록 스레드는 가장 오래된 미flush 행이 250 ms가 되면 flush하므로(`runtime.cpp:87`, `journal_ring.h:180-186`, `runtime.cpp:855-868`) `oldest_ns`는 평소 0~250 ms 사이를 오가고, 지연 ≤ 100 ms는 flush 주기마다 약 100 ms 창입니다; 10 Hz 틱이면 수 초 안에 들어가는 것이 보통이나 보장은 아닙니다). 이동 전체와 정차 첫 5 s가 모두 막혀야 하므로 자주는 아니지만, 결과는 최대(차단기 방위와 최종 주차 방위의 차이만큼, 하강 중이면 임의 각)이고 다음 시동의 불확실도는 S1의 작은 값이라 60° 한계가 막지 못합니다. 홈 하강(#14: GPS 상실 908.8 s → 최종 주차 965.2 s, 56 s)에서 중간 정차가 945 s 근처에 있었다면 바로 이 창입니다.
- 변형(P3 아래): 교체 중이 아닐 때의 60 s fsync 재기록(`sync_ok`, 1264행; 차단기 앞 60 s 대기)도 `last_s_request_ns_`를 갱신하므로, 그 직후 GPS 상실 → 30 s 안의 첫 지하 정차 S가 막힙니다.
- 완화안: (i) `replacing_rate`를 "직전 S 요청 뒤 표시(M)가 디스크에 들어갔을 때만" 적용(즉 `disk_=='S' && disk_flags_&HEADING`이면 30 s 간격 없이 S 허용 — 이 경우 새 S는 앞 S를 바로잡는 안전 쓰기이고 교체 중 쓰기 횟수는 "표시가 없을 때 1회 더"로 묶임), 또는 (ii) 표시를 `parked` 뒤에도 계속 시도(정차 중에는 교체 중 지연 조건을 두고), 또는 (iii) 최소한 기록·AGENTS 문안의 해당 문장을 "표시가 들어간 뒤라면"으로 고치고 분석기 `S records left without a departure mark`에 "교체 중 `replacing_rate`로 막힌 뒤 세션이 끝난 S"를 따로 세십시오.

### C2. 시간 항 0.0023의 사전 등록 결과가 재현되지 않습니다 — 기록 "P4 사전 등록 규칙과 결과"

- FACT: 카운트 → rad/s 변환(`pipeline.cpp:28` 0.000658615, `drive_record_log.h:91`)과 RMS 산술(sd 3.16, 평균 −1.28 → RMS 3.40; × 0.000658615 = 0.00224; 0.0001 단위 올림 0.0023)은 맞습니다. G3 적분 영점은 `beta_.yaw_zero` = 2048(`beta_profile.h:202`; `pipeline.cpp:1059, 1066`)이므로 "2048 기준"도 맞습니다.
- FACT: 제가 같은 규칙으로 센 결과는 위 표와 같이 n = 111(또는 90·109), RMS 3.17~3.31이고 어떤 선택도 93·3.40을 주지 않습니다. 기록은 어느 내보내기·어느 추가 조건(예: `pre` 유무, `sd` 상한, 부팅 선별)을 썼는지 적지 않았습니다.
- 영향: 0.0023은 제 모든 변형(0.0021~0.0022)보다 큰 값이라 **보수적**입니다(안전 쪽). 그러나 "사전 등록"의 요점은 재현 가능성이므로 선택 기준과 행 수의 근거를 기록에 보충하십시오. 사전 등록 규칙의 문구대로면 제 자료에서는 0.0022가 나왔을 것이고, 이마트 출구 모델의 "창이 닫힘" 결론(59.7~60.4°)은 0.0022에서도 거의 같습니다(INFERENCE: 시간 항 차이 0.0001 rad/s × 약 190 s ≈ 1.1°).

### C3. 시험이 고정하지 않는 두 성질과 재생기의 누락 — `tests/runtime/test_g3.cpp:1535-1567, 1764-1835`, `tools/replay_beta.cpp:679-683`

- M4 생존(FACT): `pipeline.cpp:1417`의 규칙 F 가정 조건에 `g3_.reverse_unsure||`를 넣어도 test_g3(1,314)·test_navigation(3,977)·test_model_session_input·합성 재생이 모두 통과합니다. 즉 "관측 전용"은 코드 읽기(제가 확인: 읽는 곳은 `g3_send.h`뿐)로만 보장됩니다. 저자의 비공개 재생 동일성(표식 on = beta.16, 110/110)은 어떤 부팅에서 `reverse_unsure > 0`이면서 규칙 F가 걸렸을 때만 이런 변이를 잡습니다. navigation 시험에 "배제된 R(값 1) 뒤에도 규칙 F가 그대로 가정한다"(또는 더 직접적으로 "`reverse_unsure`를 바꿔도 `status()`·BETA 추정이 같다") 한 사례를 넣으십시오.
- M3 생존(FACT): 30 s 간격에서 규칙 F 오염 재기록(`stale`, 1273행)을 빼는 예외는 시험이 없습니다. 예외가 없어지면 오염된 방위가 교체 중 최대 30 s 더 디스크에 남습니다(작은 안전 구멍). `garage_store`에 사례 하나면 됩니다.
- 재생기(FACT): `tools/replay_beta.cpp`는 `G3Sample::writer_lag_ns`를 채우지 않으므로(`g3_sample`의 memset 0) 재생의 표시는 100 ms 규칙에 걸리지 않습니다. 1차 검토 P2(재생이 idle 규칙을 모델링하지 않음)와 같은 종류이며 기록 (B)·(C)와 비공개 재생의 "표시가 들어감"은 그만큼 낙관적입니다.
- M2(앞 8자 비교)는 test_g3를 통과하고 재생 시험에서만 잡혔는데, 그것도 합성 부팅 id 세 개의 앞 8자가 모두 `00000000`이어서입니다. 결함은 아니지만 `garage_codec_restore`의 직전 부팅 사례는 앞 8자가 다른 id만 씁니다(약한 고정).

### C4. 문구 (기록·AGENTS 문안·주석)

1. 기록 P3 절·`g3_heading.h:1292` "막힌 최종 주차는 표시를 남긴다" → "표시가 이미 들어갔다면"(C1).
2. AGENTS 문안의 잔여 "a car in reverse since before the product started" → 제품 시작이 아니라 **워커가 motion 채널에서 첫 메시지를 받기 전**(가드 대기 포함)입니다. 그 전의 0x118 변화는 배제도 계수도 되지 않습니다(`runtime.cpp:1199-1214`는 받은 메시지만 다룹니다).
3. AGENTS 문안에 없음: 표시는 이동 중과 정차 첫 5 s에만 시도된다(1261행); 30 s 간격은 취소·실패한 S 요청과 fsync 재기록 요청에서도 센다(1325행); 규칙 F 오염 재기록은 예외; 같은 부팅의 두 번째 워커 시작은 직전 부팅 기록의 이월을 거부한다(P2).
4. 기록 "P1 토큰" 절 "제품이 시작했지만 G3가 돌지 않았거나 … 잡는 경우"에 "워커가 swap(`runtime.cpp:1706`)에 이르기 전에 끝난 부팅(`stop_requested` 1542행, locale 실패 1546행, 워커 스레드 미생성)은 못 잡음"을 더하십시오(P4).
5. `docs/FIELD_PROCEDURE_BETA_KO.md` 새 문단은 코드와 맞습니다(20 m, 잃은 정보, 직전 제품 시동, 첫 두 FIX 원본, 메뉴 1·4). 다만 "후진이면 기다리거나 끝냅니다"에 C1의 조건은 없습니다(현장 문서라 생략 가능).

## PLAUSIBLE (조건이 맞으면 실제 손해 또는 효과 손실)

### P1. 세지 않는 0x118 정보 손실 경로 — `src/runtime/motion_gap.h:48-56`, `pipeline.cpp:321-325`, `runtime.cpp:1076`

- (a) 원천 epoch 변경(탭·jciVBS 재시작): `MotionGapTracker::accept`는 **거부가 선행해 `open`일 때만** epoch 변경을 TOO_LARGE로 봅니다(52행). 거부 없이 epoch만 바뀌면 `exclude_reverse(INPUT_GAP)`가 불리지 않고, 파이프라인 쪽 321행은 **유효한** 래치일 때만 세며(`drop_latch`), 무효 래치면 아무것도 세지 않습니다. 재시작 사이에 잃은 R 변화는 계수 0으로 남아 20 m 규칙이 엽니다. 드뭅니다(주행 중 탭 재시작).
- (b) `motion_gap_accept`는 `enabled`(모델 켜짐)일 때만 셉니다(1076행). BETA 빌드에서는 모델이 시작부터 켜져 있어 승인 전 메시지도 1203행 경로로 센다고 봅니다(FACT: 1199~1203행); 모델이 꺼진 동안의 공백은 G3도 없으므로 실질 영향 없음. 적어 두는 것뿐입니다.
- (c) 워커가 채널에 붙기 전(C4-2).

### P2. 같은 부팅 안의 서비스 재시작과 토큰 — `runtime.cpp:1540, 1706`, `g3_heading.h:785-790`

- FACT: 토큰은 `run_worker_association`마다 바뀝니다. 같은 부팅에서 워커가 다시 시작되면(주석 1540행 "same-boot service restarts") 직전 = 자기 부팅이 되어 직전 부팅의 S 기록은 `not_prev_boot`가 됩니다(기록도 적음). 출발 전(지하에서 서 있는 동안 AA 재연결 등으로 호스트 프로세스가 재시작되는 경우)에는 1차에서는 이월됐을 기록이 이제 거부됩니다(효과 손실, 안전 쪽). 그 토큰 쓰기(temp+rename, fsync 없음)는 재시작이 주행 중이면 주행 중에 일어납니다. 새 워커는 BETA가 아직 기동 전이라 "교체 중"은 아닙니다. 워커 시작을 막지는 않습니다(동기 파일 I/O 한 번, 저널 스레드 생성 뒤).
- 2026-10-11 H1("주차 쓰기만")의 또 하나의 예외이므로 문구에 넣으십시오.

### P3. 60 s fsync 재기록 → GPS 상실 → 30 s 간격 (C1의 변형) — `g3_heading.h:1264, 1281, 1325`

- FACT: 교체 중이 아닌 긴 정차(≥ 60 s)에서 이 부팅의 fsync 없는 기록이 있으면 fsync 재기록이 요청되고(1281행) `last_s_request_ns_`가 갱신됩니다. 그 뒤 30 s 안에 GPS 상실(교체 시작)과 정차 5 s가 오면 S가 `replacing_rate`로 막힙니다. 차단기 앞 60 s 대기 → 진입 → 바로 주차의 순서입니다. 표시가 들어갔으면 디스크는 M(안전, 효과 없음), 안 들어갔으면 C1.

### P4. 토큰이 못 잡는 "제품은 시작했으나 워커가 swap 전에 끝난" 부팅 — `runtime.cpp:1542, 1546, 1706`

- FACT: swap 전에 돌아가는 경로는 `stop_requested`(소유자의 명시적 정지; 주행 없음)와 locale 실패, 그리고 워커 스레드가 아예 생성되지 않는 로더 경로입니다. 가드가 선택했는데 이 중 하나면 토큰은 그대로이고 그 부팅의 주행은 다음 부팅에 보이지 않습니다. 기록은 "제품이 시작하지 않은 시동"만 적었습니다. 확률은 낮습니다.

### P5. 20 m 규칙의 남는 경우 (기록이 적은 것 + 하나)

- 제품(워커) 시작 전부터 R이고 20 m 넘게 후진(기록), 수동변속기 중립으로 20 m 넘게 뒤로 밀림(요는 차체 회전이라 영향 없고 바퀴 속도는 부호가 없음). 둘 다 드뭅니다. 한도는 아래 (2)의 정량.

### P6. 1차 검토의 P5·P7·P8 (변화 없음)

- 집 나선 주차장 효과 없음(기록이 적음), 턴테이블·기계식 주차(기록이 적음), IDLE 요 끊김 감시로 창이 자주 `none/yaw_break`일 가능성(다음 주행의 `none`/`wait` 사유 분포를 볼 것). 2 %는 여전히 표본 1개입니다.

## 영역별 점검 결과 (찾지 못한 것 포함)

### (1) 파이프라인 계수 `reverse_unsure`

- 읽는 곳(FACT, `grep -rn reverse_unsure src/ tools/`): `g3_send.h:206`(ACTIVE 종료), 301(대기 사유), 319·337(행). `tools/analyze_logs.py`는 `rlost` 필드를 형식 검사만 합니다. 파이프라인 결정(`reset_state`, `enqueue_raw`, 규칙 F, BETA 게이트, 앵커)은 읽지 않습니다. `G3Motion`은 `g3_motion()`로만 노출됩니다(`pipeline.h:381`). 동작·위치 출력은 구조상 동일하며, 변이 M5(증가 전부 제거)도 test_g3의 계수 단언에서만 잡혔습니다. M4가 살아남은 것은 이 성질을 시험이 고정하지 않는다는 뜻이지 깨졌다는 뜻이 아닙니다(C3).
- 네 손실 경로(FACT): 유효 래치 지움(모든 사유, `drop_latch` 1150행; 원천 epoch 변경 321~322행도 여기로), 거부된 REVERSE(327행), 재설정이 버린 대기열 REVERSE(221행; 유효 래치면 1150행과 겹쳐 2 증가 — 무해), 배제된 비전진 값(1112행; 런타임 1202·1214행과 재생기 774행), 입력 공백(1107행; `motion_gap_accept` 1079행). 세지 않는 것: P1(a)(b)(c), `init_model`의 `clear_latch()`(103행; 워커 시작 한 번뿐, 1661행), 규칙 F 가정 해제(325행 `clear_latch`, 가정은 `latch_valid_` 거짓일 때만 있으므로 잃을 유효 래치 없음), `pbreaks`(래치와 무관). 부팅 수명 계수: `g3_`는 생성자에서만 memset(57행)이고 `reset_state`는 `breaks`만 올립니다(233행).

### (2) 창 규칙 (`g3_send.h:206-208, 300-305`)

| 시나리오 | 코드 경로 | 결과 |
|---|---|---|
| 시동 묶음(값 0) 승인 전 배제 | 1112행: 전진 값은 세지 않음 | 영향 없음(설계) |
| 승인 전 R(값 1) 배제 | 1112행 계수 1 | `wait/latch_unsure`; 뒤에 유효 전진(D로 변속 메시지)이 오면 열림 |
| 세션 전 R, 메시지 자체는 워커 연결 전 | 받은 적 없음 | 계수 0 → 20 m 뒤 열림, 180° 반대 (기록의 잔여, C4-2) |
| 정차 중 재설정(창 열림 뒤) | 유효 래치 → 1150행 계수 → 206행 `latch/unknown` 종료; 무효 래치·대기열 없음 → 계수 그대로 → 창 유지(잃은 정보 없음) | 1차 P6 고쳐짐 |
| 지하에서 기어감 | 거리 누적(승인 뒤 바퀴 이벤트만) | 20 m까지 `latch_unknown` |
| 소유자가 R에 두고 떠난 차(자동: 불가능, P→R은 메시지; 수동: R 스위치가 켜진 채 시동 → 묶음 값이 1이면 계수) | 묶음이 승인 전이고 값 1이면 1112행 | 대기(안전). 묶음이 워커 연결 전이면 잔여 |
| 수동 중립 | 0x118은 R 스위치뿐 | 전진 방위, 뒤로 밀리면 반대(P5) |
| 유효 전진 래치 + 계수 > 0 | 300행 첫 조건 `s.reverse==1`만 아님 | 열림(`rlost` > 0 기록), 뒤에 래치가 떨어지면 종료 |
| 규칙 F 가정 전진 | `reverse_latched()`는 `latch_valid_`만(`pipeline.h:350`) → −1 | 가정은 창을 열지 못함(20 m 규칙만) |

- 한도의 정량(FACT, 상수 `g3_send.h:78-100`): 서 있는 동안 ≤ 900 s(순정도 0 또는 잡음 방위), 출발 뒤 GPS가 돌아오면 게이트(≥ 3.6 km/h, 바퀴 ≥ 1 m/s, |GPS/바퀴 − 1| < 10 %) 통과 FIX 8회 연속 충돌(1 Hz면 약 8 s) 또는 정책 M의 첫 진행방향(`G3Tracker::tick` course → REJECTED) 또는 출발 60 s; 180° 반대는 신뢰 4회로 끝날 수 없습니다(허용 폭 = max(unc, 10°) ≤ 60°). 출구의 첫 두 FIX 송신은 원본(세대 전환). 즉 틀린 방위가 **움직이면서** 나가는 시간은 수 초~8 s 급이고, 그 전의 느린 FIX(게이트 미통과)는 더 길 수 있습니다.
- 20 m 규칙의 일관성(INFERENCE): 자동변속기에서 "승인 뒤 바퀴 20 m 동안 0x118 변화가 없고 잃은 것도 없음"은 "그동안 R에 들어가지 않았음"과 동치이며 P→D는 메시지를 만들지 않으므로 맞습니다. 거리는 승인 뒤 바퀴 이벤트만 누적하므로 사실상 "승인 뒤 20 m"입니다(문구 "since boot"보다 보수적). 남는 구멍은 P1·P5뿐입니다. 일관됩니다.

### (3) 직전 제품 부팅 토큰 (`g3_heading.h:496-534`, `runtime.cpp:1697-1707`)

- 원자성(FACT): 읽기 → O_EXCL 임시(앞서 남은 임시는 unlink) → write → close → rename. fsync 없음. 읽기와 rename 사이 전원 차단: 디스크는 옛 id(A), 이번 부팅은 메모리의 prev = A로 진행; 다음 부팅은 A를 읽어 이번 부팅(B)의 기록을 `not_prev_boot`로 거부(효과 손실, 안전). 손상·짧음·긴 파일(≤ 64 B 검사)·버전 2·추가 줄·심볼릭 링크: `unread` → 거부 → 다시 씀(시험 `last_start_token` 1836~1895행). "unknown" boot_id: 쓰지 않음, `prev_ok`도 거짓.
- 두 부팅 연속(#12류): A(S) → B(짧음, 기록 없음) → C: prev = B ≠ A → 거부(기록이 적음, 효과의 대가).
- 워커 미기동: swap 없음(P4). 같은 부팅 재시작: P2.
- 위치·권한: `<root>/logs/g3-last-start`(0600), 저널과 같은 relfs 디렉터리. `logs/`를 열거해 모르는 파일을 거부하는 코드는 없습니다(FACT: `opendir/readdir` 없음). 메뉴 3 내보내기에 50 B 파일 하나가 더 들어갑니다.
- 표식과 무관하게 씀(FACT, 1697~1706행: `beta_requested`면 `beta_core`·`g3_on`·`g3_garage` 이전에 호출). 비BETA 모드·`beta_requested` 거짓이면 없음(기록대로). 저널 행·송신·저장 기록은 바뀌지 않음(FACT: `journal_start`의 `prev`는 `garage_`일 때만, 805~812행; `wait` 행도 `garage_`만). 표식 부팅에서 `prev_boot_`는 복사만 되고 읽히지 않습니다(`depart`가 `garage_`를 요구, 769·785행).
- 표식의 의미(FACT): 759행에서 플래그 4 기록(S든 M이든)을 `G3_LOAD_FORMAT`으로 바꿔 `have_stored_` 거짓, `next_seq_` 1, 나이·연쇄 판정 없음 — beta.16의 `parse_u64(...,3,...)` 거부와 같은 결과입니다. 7일 나이 경로는 그 뒤라 닿지 않습니다. 표식 부팅이 다음에 쓰는 기록은 플래그 4 없이 seq 1부터입니다(beta.16과 같음).
- 주행 중·교체 중 실행: 워커 시작 한 번뿐(P2의 재시작 제외). 교체 중은 아님.

### (4) 교체 중 저장 I/O 경계

- 단위(FACT): `oldest_ns`는 ns(`runtime.cpp:1985`), 상수 100,000,000 ns = 100 ms, 30,000,000,000 ns = 30 s. `writer_busy`(300 ms, `G3_WRITER_IDLE_LAG_NS`, 89행)와 같은 `oldest_ns`(큐의 가장 오래된 prompt 행, 꺼냈지만 안 쓴 행, 쓰였지만 flush 안 된 행, BULK 쓰기 시작 시각 중 최솟값)입니다.
- 호출 경로(FACT): S 요청 — `store()` 한 곳(1292행), 60 s fsync 재기록도 같은 경로(교체 중에는 `sync_ok` 거짓이라 요청 자체 없음), 규칙 F 오염 재기록은 `!stale`로 예외. 표시 — `depart()` 한 곳(1342행). 지연을 모를 때(`!w`): `journal_writer_lag`는 0을 돌려주지만 io_block의 `no_writer`가 먼저 막습니다(`runtime.cpp:946-954`, `g3_heading.h:542-550`).
- 표시 기아(C1): 100 ms 규칙 단독으로는 기아가 어렵지만(위), 드레인·지연 가드·5 s 간격과 합쳐 "이동 전체 + 정차 첫 5 s"가 막히는 경우는 있을 수 있고, 그때 1차와 달리 최종 S가 구하지 못합니다. 1차 검토 P2가 말한 "낡은 S"의 조건이 "최종 정차 < 5 s"에서 "최종 정차가 직전 S 요청 30 s 안"으로 넓어졌습니다.
- 교체 중 취소 규칙(FACT, 1362~1368행): fsync 없는 쓰기는 `beta_replacing`으로 취소되지 않음, 표시는 바퀴·stale로 취소되지 않음 — 1차와 같음.

### (5) 2 %·0.0023 범위

- FACT: 저장 불확실도 `reference()`는 `garage_`(스위치) 기준(1145행), 전파 `propagate()`는 `garage_carry_`(플래그 4 이월) 기준(1120행), `unc_max()`는 `garage_carry_`(743행), 복원 한계는 `depart`(769행). garage 켠 부팅이 쓰는 모든 기록은 플래그 4(1301행; 표시도 `G3_FLAG_DEPART`)이므로 "2 %로 자란 기록 = 플래그 4"가 성립하고, 플래그 4 없는 이월 기록은 10 %·0.002·30°입니다. 표식 부팅은 플래그 4를 FORMAT으로 거부하므로 "30° 규칙이 2 % 기록에 적용되는 빌드 없음"도 성립합니다. `not_prev_boot`로 거부된 기록도 `garage_carry_` 참이지만 UNVERIFIED라 창이 없습니다.
- 표식이 있을 때 beta.16과의 바이트 동일성(INFERENCE, 구조): `garage_` 거짓이면 `time_coef/rotation_coef`가 BETA 값, `G3_PARKED_NS` 15 s, 표시 없음, 교체 중 차단(`g3_io_block` `!garage`), `prev` 필드 없음, 플래그 4 → FORMAT. 저자의 110/110은 이 구조와 맞습니다. 표식 없이 beta.16과 다른 것은 설계된 차이뿐입니다.
- 산술(FACT): C2. 60° 대 30°의 적용은 시험 `garage_codec_restore`가 고정합니다(29.9/30.1, 62.02).

### (6) AGENTS.md 문안

대체로 코드와 맞습니다(표식·메뉴 1·4, 플래그 4, 60/30, 2 %·0.0023, 토큰의 시점·표식 무관, 후진 규칙과 잔여, 교체 중 S 30 s·표시 100 ms, H1 완화, fsync 잔여, 사후 검출, 효과 미증명). 고칠·더할 것:

1. "a car in reverse since before the product started" → "since before the worker received its first motion message".
2. "the departure mark is written while the car moves" → "while the car moves or within the first 5 s of a stop; once a stop is 5 s old only the parked S record is requested".
3. "at most one S request per 30 s while BETA replaces" 뒤에: "counted from the previous S request (also a cancelled or failed one and the 60 s fsync rewrite), the rule-F taint rewrite exempt; a final park within 30 s of a previous S request while BETA replaces is not stored before the 30 s pass, so if the departure mark of the previous S did not land (idle rule, 5 s spacing, 100 ms rule) the previous S survives the drive" (C1).
4. 낡은 S 목록에 "a second worker start in the same boot refuses the previous boot's record (not_prev_boot)" 와 "a boot whose worker did not reach the token swap" 추가(P2·P4).
5. "G3Motion::reverse_unsure zero: … no motion input gap that clears the latch" 뒤에 "a source-epoch change without a preceding rejection and messages before the worker attached are not counted"(P1).
6. 0.0023의 근거 문장에 trace·행 선택 기준(C2).

### (7) 시험

- 공허 단언: 1차 C3의 `or True`는 삭제됨(FACT). 새 단언 중 `strlen(boot)!=36 ||`(test_worker_beta 654·1141·1154행)는 boot_id를 못 읽는 호스트에서 토큰 검사를 건너뛰는 방어이며 이 호스트에서는 36자입니다. 그 밖의 공허 단언은 찾지 못했습니다.
- 변이 결과는 위 표. 잡힌 것: M1, M2(재생에서), M5. 생존: M3, M4(C3).
- 흔들림 반복: 아래 절.

## 흔들림 반복 (g3_cancel · g3_garage 각 15회, 부하 ≤ 3)

- 방법: 각 실행 전에 1분 부하 ≤ 3.0을 기다린 뒤 순차 실행(`nice` 없음), 실행당 약 79~81 s. 같은 시간에 제 변이 빌드(g++ 4개, 각각 단일 스레드)가 다른 코어에서 돌았습니다.
- `g3_cancel`: **14/15 통과, 1 실패(4번째 실행, 종료 134)**. 실패 단언은 저자가 본 것과 같은 `test_worker_beta.cpp:1095` `!lagging && max_summary_gap<1600000000ULL`이고, 행은 `cancelled_at_rename=1 why=beta_replacing latency_ms=3004 begun_before_loss=1 short_writes=1 max_summary_gap_ms=1022 lagging=1` — 통과한 5번째 실행(`latency_ms=3003 … max_summary_gap_ms=1020 lagging=0`)과 **기능 결과는 같고 `beta_journal_lag lagging` 행 하나만** 다릅니다. 그 실행은 시작 시 부하 ≤ 3이었으나 끝날 때 1분 부하 4.62였고(실행 중 M4 변이의 빌드 4개가 09:59~10:00에 겹침, 로그 시각으로 확인), 제 부하 조건을 지키지 못한 실행입니다(FACT). 나머지 14회의 종료 시 부하는 0.4~2.2였습니다.
- `g3_garage`: **15/15 통과**(종료 시 부하 0.6~1.8).
- 판정(INFERENCE): 실패는 기록 스레드가 1.5 s 넘게 CPU를 못 받아 저널 지연 가드가 내려간 것으로, 같은 입력에서 저장 스레드·취소·rename 결과는 변하지 않았습니다. 새 코드의 경합을 가리키는 흔적은 없습니다. 다만 이 두 시나리오의 1.5 s/1.6 s 단언은 부하 4 이상의 호스트에서 재현 가능한 거짓 양성이므로, 그 단언을 `ARM_TIMING_RETRIES`처럼 "제한된 재시도 + 기록"으로 두거나 부하를 로그에 남기는 것이 좋겠습니다.

새 코드가 두 시나리오에 더한 것(FACT): 워커 시작의 토큰 쓰기(저널 스레드 생성 뒤, 저장 스레드 시작 전, 동기 파일 I/O 한 번)와 파이프라인 계수 증가. 두 시나리오는 표식 부팅(`test_worker_beta.cpp:262`)이라 30 s 간격·100 ms 규칙·표시·`prev` 행은 돌지 않습니다. 실패했던 단언은 저널 지연 가드(1.5 s)와 `beta_summary` 간격(1.6 s)의 시간 단언으로, 기록 스레드가 CPU를 못 받으면 코드와 무관하게 실패합니다. 새 경합은 찾지 못했습니다.

## 권고 (리드·소유자 결정 사항)

1. 설치 전: C1의 코드 또는 문구 결정(완화안 (i)가 가장 작습니다: 디스크가 S·방위 있음이면 교체 중 S 간격을 적용하지 않음 — 그 S는 앞 S를 바로잡는 쓰기), 분석기에 "교체 중 `replacing_rate`로 막힌 뒤 세션이 끝난 S" 계수.
2. 기록: C2의 선택 기준, C4·(6)의 문구.
3. 시험: M3·M4 사례, 재생기 `writer_lag_ns`(최소한 상수로 주입 가능하게).
4. 다음 주행 분석에 추가: `episode_block "replacing_rate"`의 `g3_since unstored` 행 수와 그 뒤 M 유무.

## 하지 않은 것

- 차량·폰·DHU·ARM/QEMU 실행, 전체 `make test`, 비공개 20개 부팅 재생(구조 검토만; 요 영점 행만 비공개 trace에서 직접 셈), 순정 BusyBox 하네스, 플래시 마모 정량화.
