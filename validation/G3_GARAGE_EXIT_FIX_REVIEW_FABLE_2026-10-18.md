# G3 지하주차장 출구 방위 수정 독립 적대 검토 (Fable, 2026-10-18)

대상: `/home/sixtyfive/mazda-aa-dr`의 스테이징된 변경(`git diff --cached HEAD`, 22개 파일, 커밋 전), 기록 `validation/G3_GARAGE_EXIT_FIX_2026-10-17.md`. 소유자 저장소는 읽기만 했고, 실행은 `HEAD` 아카이브에 패치를 적용한 스크래치 사본(`(scratch)`)에서만 했습니다. 차량·폰·DHU·ARM/QEMU는 실행하지 않았습니다. 표시: **FACT**(코드·로그·실행에서 직접 확인), **INFERENCE**(해석·모델). 줄 번호는 패치 적용 뒤의 파일 기준입니다.

## 요약

- 전달 계약(바이트 40, 44..47만, BETA, 창 안, 실패 시 순정 그대로)을 깨는 새 경로는 **찾지 못했습니다**. 어댑터의 `encode_bearing_overlay`(`src/adapter/adapter.cpp:519-532`)는 바뀌지 않았고, 창을 여닫는 조건만 넓어졌습니다.
- 그러나 창을 여는 두 전제에 코드 사실과 어긋나는 부분이 있습니다. **(C1)** "후진 래치 미확인 = 시동 뒤 0x118 변화 없음 = 후진 아님"은 틀립니다. 래치는 세션·버스 승인 전의 0x118 메시지가 **배제**될 때(`exclude_reverse`)와 **모든 파이프라인 재설정**에서도 미확인이 됩니다. **(P1)** "플래그 4의 S 기록 = 저장 뒤 서 있었음"은 제품이 그 주행을 관측한 부팅에서만 성립하는데, 연쇄 토큰은 가드가 **거부**한 부팅에서만 바뀌므로 저널 실패·워커 미기동·센서 없음·트레이스 없는 부팅(이번 자료에도 #11→#13 사이에 로그 없는 이동이 있음)은 잡지 못합니다. 이 경우 출구에서 최대 180° 틀린 방위가 신뢰 4회·충돌 8회·첫 정책 M 진행방향·900/60 s 상한까지 나갑니다(순정의 150~180° 오류와 같은 크기, 단 순정이 맞았을 부팅에서도).
- 저장 쪽 새 노출: 교체 중(GPS 상실, ENGAGED) **이동 중에도** 쓰는 첫 저장 I/O입니다(2026-10-11 H1 "주차 쓰기만"이 더는 성립하지 않음). 터널 정체(G1 경로)에서 5 s마다 S·M 두 번의 temp+rename이 저널과 같은 relfs에서 돌며, BETA 출처는 저널 지연 1.5 s에 묶여 있습니다. 사전 등록 규칙은 사후 검출입니다.
- 불확실도 2 %/60°: 측정된 정차 영점 산포(sd 0.134 °/s)가 시간 항 전체(0.115 °/s)보다 크므로 60° 안이라고 보낸 방위가 실제로 40~60° 틀릴 수 있습니다(표본 1개, 기준 (A) 판정 불가). 그래도 순정 오류보다 작고 끝 조건이 있습니다. 집 나선 주차장은 모델상 출구 전에 60°를 넘어 창이 닫힐 가능성이 큽니다(안전 쪽, 효과 없음).
- 시험: 내가 만든 변이 10개 중 9개를 잡았고 **1개(M8: 회전 항 2 %를 스위치가 아니라 이월 기록에만 적용)가 살아남았습니다**. 2 %가 (a) 이 빌드가 쓰는 **모든 기록의 저장 불확실도**(`reference()`)와 (b) 플래그 없는 기록의 전파(한계 30° 그대로)에도 적용된다는 사실이 기록·AGENTS 문구에 없고 시험에도 고정되어 있지 않습니다.

판정: 차량 설치 전에 고쳐야 할 **CONFIRMED 결함은 전제·문구·시험 수준**(C1, C2, C3)이며, 전달 계약 위반은 없습니다. 제품 동작을 바꿀지는 P1(연쇄 검사)과 P3(교체 중 쓰기)에 대한 리드·소유자의 위험 수용에 달렸습니다. 아래에 각각 구체 시나리오와 완화안을 적었습니다.

## 확인한 것 (실행, FACT)

스크래치 사본, 1분 부하 0.5~1.5, 전체 `make test`는 다른 작업이 돌고 있어 중복하지 않았습니다.

| 항목 | 결과 |
|---|---|
| `build/test_g3` | 1174 검사 통과 |
| `build/test_worker_beta g3_garage / g3_park / g3_send` | 3개 모두 통과. g3_garage: 1번째 S `sync:0` SPEED_ENGAGED 중, 2번째 S `sync:1` ARMED에서 3002.5 ms(훅), 3번째 M `mark:1` 주행 중, 디스크 최종 M seq 3 |
| `tests/replay/test_replay_g3_garage.py` (build/replay_beta) | 4개 통과 |
| `tests/packaging/test_trial_menu.py` 메뉴 12, `test_trial_status.py` garage 표식 | 통과 |
| `tests/tools/test_analyze_g3.py` | 9개 통과 |
| 내 변이 10개(각각 단독, `test_g3` 재빌드) | M1 출발 거리 5 m → 1466줄 실패(잡힘). M2 정차 15 s → 1435줄(잡힘). M3 `chain_depart` 분기 삭제 → 1384줄(잡힘). M4 미확인 래치면 대기(`s.reverse!=0`) → 1539줄(잡힘). M5 ACTIVE에서 미확인 래치로 종료 → 1539줄(잡힘). M6 첫 바퀴 움직임 전 끊김도 셈 → 1571줄(잡힘). M7 교체 중 fsync 허용(`sync_ok=true`) → 1452줄(잡힘). **M8 `rotation_coef`를 `garage_carry_`로 → 1174 검사 통과(살아남음)**. M9 표시를 바퀴 움직임으로 취소 → 1509줄(잡힘). M10 회전만의 출발 무시 → 1480줄(잡힘) |

## CONFIRMED (코드 사실; 손해는 조건부)

### C1. "미확인 래치 = 전진" 전제가 코드와 다릅니다 — `src/runtime/g3_send.h:275, 188`, 기록 표 "송신 창", AGENTS 문구

- FACT: `G3Sample::reverse`는 `nav.reverse_latched()?latch_value():-1`이고 `latch_valid_`가 거짓이면 −1입니다. `latch_valid_`는 (1) 세션·버스 승인 **전**에 받은 REVERSE 메시지를 런타임이 배제할 때(`src/runtime/runtime.cpp:1202, 1214` → `Pipeline::exclude_reverse`, `src/navigation/pipeline.cpp:1101`), (2) **모든** 파이프라인 재설정(`pipeline.cpp:224 drop_latch`, BAD_INPUT·MISSING_SENSOR·source epoch, 입력 공백 `runtime.cpp:1079`)에서 거짓이 됩니다. 이전 기록이 이미 적어 둔 사실입니다(`REVERSE_GATE_ASSUMED_FORWARD_2026-10-11.md` 8~16행: "messages before the AA session/bus admission are excluded … every reset … leaves it unknown"; `G1_FIRST_TUNNELS` 131행: "6 at engine start"가 세션 전이라 배제됨).
- 따라서 "0x118 변화가 없었다 = 후진으로 넣지 않았다"는 (a) 시동 직후 R에 넣은 뒤(수동변속기, 또는 AA 연결 전 R 선택) 그 메시지가 세션 전이라 배제된 경우, (b) 창이 열린 뒤 **정차 중** 재설정(허용되는 `pbreaks`)으로 래치가 떨어진 뒤 차가 R인 채로 움직이는 경우에 거짓입니다.
- 시나리오(INFERENCE): 지상 주차칸, GPS 유효, 세션 시작과 함께 창이 열림(`rev:-1`). 운전자가 이미 R에 넣은 채 후진으로 빠져나감 → 0x118 변화 없음 → 바이트 44..47에 **차체 방위(전진)** 가 실림 → 실제 이동 방향과 180° 반대, 운전자가 D로 바꾸는 0x118 변화(값 0)까지(보통 5~15 s). 지하에서는 송신이 없어 무해. 순정은 그 구간에 GPS 진행방향(후진 이동 방향 = 맞음, 또는 잡음)을 보냅니다. beta.15 규칙은 열지 않았습니다. BETA 자체의 규칙 F는 같은 가정을 20 km/h 이상 3회 일치로 검증한 뒤에만 씁니다; 이 창은 검증 없이 씁니다.
- 손해 한도: 첫 0x118 변화까지의 후진 구간(초 단위). 소유자 습관(FIRST_CONNECTION 기록: 전진 출발 5/5, 후진 주차 4/4)이면 드뭅니다. 그러나 문구 "no 0x118 change since boot: the car was not shifted into reverse"는 사실이 아니므로 고쳐야 합니다.
- 완화안: (i) 문구를 코드 조건("latch not valid: no message since admission or since the last reset")으로 바꾸고 위험을 적는다. (ii) 또는 미확인이면 `first_move_reverse`가 −1이고 `s.motion.breaks`가 창 열림 뒤 0인 경우로 제한한다. (iii) 또는 beta.15처럼 미확인은 대기하되 시간 제한을 없앤다(지하 출구에서는 보통 그 전에 0x118 변화가 한 번 오는지 자료로 확인 필요: #14에서는 929 s까지 한 번도 없었으므로 (iii)는 효과를 없앱니다 — 그래서 저자가 바꾼 것으로 보입니다. 그렇다면 (i)+(ii)).

### C2. 회전 항 2 %가 스위치 전체에 걸립니다 — `src/runtime/g3_heading.h:1056`, `1045`, `1023` (변이 M8 생존)

- FACT: `rotation_coef()`는 `garage_`(스위치)를 보고 `garage_carry_`(플래그 4 기록을 이월했는지)를 보지 않습니다. 그래서 스위치가 켜진 모든 부팅에서 (a) `reference()`의 `head_unc_` 성장(1045행) = **저장되는 모든 기록의 불확실도**가 BETA의 10 % 대신 2 %로 자라고, (b) 플래그 4가 없는 기록(beta.16 기록, `chain_ok`·`position_ok` 경로)의 전파도 2 %인데 그 한계는 30°(`unc_max()`가 `garage_carry_` 기준)입니다.
- 기록의 표("회전 항 2 %(BETA 10 % 대신), 이 기록의 한계 60°")와 AGENTS 문구("the propagated uncertainty uses a 2 % rotation term")는 전파만 말합니다. 저장 불확실도가 작아지면 다음 시동의 `unc` 한계(60/30)를 더 많은 기록이 통과하고, `tolerance()`(신뢰 판정 폭)도 달라집니다. #14 집 기록의 ±69.7°(10 %)가 2 %에서는 60° 아래로 내려와 **이월 대상이 되는 효과**가 바로 이 경로에서 나옵니다. 의도한 것일 수 있으나 적혀 있지 않고, 변이 M8이 보여 주듯 시험이 어느 쪽도 고정하지 않습니다.
- 조치: 의도를 정하고(저장 불확실도도 2 %인지), 기록·AGENTS 문구에 적고, `reference()`의 저장 unc 값을 검사하는 단언을 하나 넣으십시오(지금 `garage_store`는 `d.flags`·`st`만 봅니다).

### C3. 시험·도구의 작은 결함

- `tests/replay/test_replay_g3_garage.py:236` `assertTrue(... or True)`: 항상 참(공허). 삭제하거나 의도한 단언으로 바꾸십시오.
- 같은 파일 `test_sends_differ_only_in_the_bearing_of_overlaid_fixes`: `--g3-send-out` 행은 바이트 40과 44..47만 기록하므로(`tools/replay_beta.cpp g3_send_record`) "나머지 바이트 동일"은 이 시험이 증명하지 않습니다. 기록 (B)의 "합성 시험에서 바꾼 송신은 바이트 40, 44..47만 다름"의 근거는 beta.15의 `test_adapter beta_bearing`과 `g3_send_identity.py`(워커 전체 페이로드 비교)입니다. 기록에 그렇게 적는 것이 정확합니다.
- `src/runtime/g3_heading.h:58-63` 머리말("the ONLY store write is the PARKED write … No record is written while moving or creeping")은 이제 거짓입니다(출발 표시는 이동 중에 씁니다). 2026-10-11 검토의 H1 불변식이 완화된 것이므로 주석과 기록에 명시해야 합니다.
- `docs/FIELD_PROCEDURE_BETA_KO.md` 새 단락 "출구에서 첫 GPS 위치(FIX)부터 … 바꿉니다": 기록 (C)대로 출구 첫 두 FIX 송신은 원본(세대 전환)입니다.
- `tools/analyze_logs.py g3_write_overlap`: `max_lat`를 garage 쓰기와 저장 규칙 쓰기가 공유하므로 "G3 write overlap check: … max lat_ms"에 garage 쓰기의 지연이 섞입니다. 규칙 판정(`lat_over_500`, `garage_lat_over_500`)은 분리되어 있어 판정에는 영향이 없습니다(표시만).
- `pending_wait_s_`(`g3_heading.h:1205`)는 garage에서도 15 s 기준이라 5 s 쓰기의 `wait_s`가 0으로 찍힙니다(저널 표시만).

## PLAUSIBLE (조건이 맞으면 실제 손해; 결과 크기 순)

### P1. 제품이 관측하지 않은 주행 뒤에도 S 기록이 살아남습니다 — `g3_heading.h:696`, `g3_chain_token` 408-437행, `runtime.cpp:1969`

- FACT: 연쇄 토큰은 `guard/last-decision`(가드가 **거부**한 가장 최근 부팅)입니다. 가드가 선택했지만 (a) 저널이 일찍 실패해 G3 틱이 돌지 않은 부팅(`runtime.cpp:1969`: `!j.failed`일 때만 틱), (b) 워커가 죽은 부팅, (c) 요 신호 없음으로 파이프라인이 계속 재설정돼 바퀴 이벤트가 G3에 오지 않는 부팅(기록 (b)에 적힌 메커니즘), (d) 트레이스가 전혀 없는 부팅에서는 토큰이 바뀌지 않고 표시도 쓰이지 않습니다. 기록 자신이 "#11 끝 → #13 시작 사이 로그 없는 이동"(d)을 FACT로 적고 있습니다.
- 그러면 다음 시동은 플래그 4·S·방위·연쇄 ok로 `CARRIED/chain_depart`가 되어 창을 엽니다. 첫 위치 검사(50 m + 저장 위치 예산 + 부팅 뒤 바퀴 거리)는 지하 상승 뒤에는 수백 m라 약하고, 같은 주차장으로 돌아왔으면 무력합니다(기록도 인정). `moving_at_boot`는 이 부팅의 관측만 봅니다.
- 손해 한도(FACT, `g3_send.h`): 정차 중 최대 900 s, 움직인 뒤 60 s 또는 게이트를 지난 충돌 8회(1 Hz면 약 8 s), 또는 첫 정책 M 진행방향(`G3Tracker::tick` course → REJECTED → `rejected`). 즉 순정의 출구 오류(150~180°, 40 s)와 같은 급이지만, 지상 출발처럼 순정이 맞았을 부팅에서도 생깁니다.
- 완화안(제품, 작음): 워커 시작 때 기록을 읽기 **전에** `logs/g3-last-start`(이전 제품 부팅 id)를 읽고 현재 boot_id를 써 두고, `chain_depart`는 `stored.boot_id == 이전 제품 부팅 id`일 때만 허용합니다("직전 제품 부팅이 쓴 S"). 제품이 기동했으나 G3가 돌지 않은 부팅(a)(b)(c)을 잡습니다. (d)는 어떤 규칙으로도 못 잡으므로 문구에 남깁니다. 분석기에는 "표시 없이 떠난 S"(S written 뒤 `g3_since left/unstored`의 m > 0.5인데 M written 없음) 계수를 추가하십시오.

### P2. 표시가 못 들어가서 낡은 S가 남는 경우 — `g3_heading.h:1213-1234`, `runtime.cpp:946-954`, `log_profile.h` 드레인

- FACT: 표시는 idle 규칙(`journal_lagging`, `no_writer`, `journal_draining`, `writer_busy` > 300 ms)과 5 s 비율 제한(S 쓰기와 `last_request_ns_` 공유)을 따릅니다. persistent 프로파일은 GPS 클래스 전환·BETA 상태 변화마다 RAW 창(~1,100행)을 150행/s로 드레인하므로(`log_profile.h` DRAIN_ROWS_PER_S) 사건마다 약 7~8 s 동안 저장 I/O가 막힙니다. 재생 도구는 idle 규칙을 모델링하지 않으므로(`tools/replay_beta.cpp g3_turn`: io_block 0, 쓰기 즉시 완료) 기록 (B)·(C)의 "켬" 근거는 표시가 항상 들어간다고 가정한 낙관적 결과입니다.
- 시나리오(INFERENCE): 주차장 입구 차단기에서 5 s 이상 정차 → S(차단기 방위) 저장 → 바로 GPS 상실(드레인) → 짧은 하강(< 20 s)·정체·지연으로 표시가 계속 막힘 → 마지막 정차 5 s 미만(이번 자료는 7.5 s였습니다) 또는 센서 중단 → 차단기 S가 남음 → 다음 시동에서 하강 순회전(집: −181°)만큼 틀린 방위. 확률은 낮지만(5 s마다 재시도) 결과는 최대입니다.
- 전원 차단(fsync 없음 뒤 수십 s 안)은 기록이 이미 적었습니다. 완화안: P1의 분석기 계수로 실제 빈도를 보고, 표시 요청은 `journal_draining`을 예외로 둘지(표시는 110 B이고 BULK 드레인과 경쟁해도 저널 지연 가드와 무관한지) 데이터로 정하십시오.

### P3. 교체 중·이동 중 저장 I/O가 G1 경로에 들어왔습니다 — `g3_heading.h:1148-1153`, `runtime.cpp:952, 1980`

- FACT: beta.16은 `beta_replacing`이면 요청도 막고 진행 중 쓰기도 취소했습니다(BETA 출처가 `journal_current`, 즉 저널 지연 < 1.5 s에 묶여 있기 때문: `g3_io_block_reason` 주석). 이제 garage가 켜지면 (1) 5 s 정차의 S 쓰기와 (2) 출발 표시 M이 GPS 상실·ENGAGED 중에도 temp 생성 + write + close + rename으로 돌고, 표시는 바퀴 움직임·stale로도 취소되지 않습니다. 터널 정체(G1의 주 시나리오)에서는 멈춤-출발마다 두 번, 5 s당 최대 1회입니다.
- 위험(INFERENCE): 저널 기록 스레드와 같은 relfs 장치의 rename/metadata 커밋이 writeback과 겹쳐 수백 ms 멈추면 기록 스레드 `write()`가 밀려 지연 가드(1.5 s)가 BETA 출처를 철회하고 **터널 교체가 그 자리에서 끊깁니다**. 이번 내보내기의 fsync 없는 쓰기 3.5~67.7 ms(기록)는 "보통은 괜찮다"의 근거이고, 사전 등록 규칙(lat_ms > 500, 지연 겹침)은 주행 뒤 검출입니다. 즉 첫 발견은 소유자가 터널에서 겪은 뒤입니다.
- 추가: 5 s 정차 + 2 m/1° 규칙으로 정체 구간의 쓰기 횟수가 beta.16(15 s)의 수 배가 됩니다(플래시 마모는 정량화되지 않음). 저널 행 예산의 최악 216 KB/h(720회 × 약 300 B) 산술은 맞습니다(`test_log_profile G3GarageRows`는 해당 주행에서 < 60 KB/h).
- 완화안: 표시와 S 쓰기를 터널 프로파일(BETA가 GPS_LOST/ENGAGED)에서는 허용하되 `writer_busy` 기준을 더 엄격히(예: 100 ms) 하거나, 교체 중에는 "주차장"에 해당하는 정차(속도 0 유지 ≥ 5 s **이고** 그 뒤 센서 중단 가능성이 있는 경우)만 쓰는 대신 이동 중 표시는 교체가 끝난 뒤로 미루는 방법이 있습니다. 어느 쪽이든 데이터로 정할 문제이며, 최소한 기록에 "G1 경로의 새 노출"로 적어야 합니다.

### P4. 2 %/60°는 측정된 영점 산포에 비해 낙관적입니다 — `g3_heading.h:1016-1023`, `pipeline.cpp:1047-1058, 1686`

- FACT: 시간 항 0.002 rad/s = 0.115 °/s(`beta_profile.h:179`). G3 적분은 고정 영점 `beta_.yaw_zero`(2048)를 씁니다(`pipeline.cpp:1686`), 학습 영점(`gyro_bias_`)은 쓰지 않습니다. 정차 영점 산포: 2040.7~2052.3(기록), `OBD_YAW_CHANNEL_DESIGN_2026-10-15.md` 440행 표: 범위 11.19카운트 = 0.422 °/s, **sd 3.56카운트 = 0.134 °/s**. 1카운트 ≈ 100 s에 3.8°.
- 산술(INFERENCE): 집 상승(이동 165 s, |회전| 1,190°) 예산 성장 ≈ 0.115×165 + 0.02×1,190 ≈ 19 + 24 = 43°. 영점 오차 1 sd만으로 22°, 2 sd면 44°(시간 항 19°의 두 배). 시동 직후 과도(−2.5 °/s 평균 10 s, M4 주석)는 정차 중이라 적분되지 않지만 그 안에 출발하면 수십 도가 예산 밖입니다(`unc_d`는 기록 전용). 이마트 표본은 unc 57°에서 오차 +14~19°(영점 ±1카운트에 ±6°)로 모순은 없으나 표본 1개입니다.
- 결론: "60° 안"이라고 보낸 방위가 40~60° 틀릴 수 있습니다. 손해는 순정(150~180°)보다 작고 첫 M 진행방향·신뢰·충돌에서 끝나므로 치명적이지는 않지만, 기준 (A) 30°는 판정 불가이고 23개 변이는 **논리**를 지킬 뿐 **숫자**를 지키지 않습니다. 완화안: 부팅 첫 정차의 `yst` 영점(이미 `yaw_stop` 행이 있음)으로 G3 적분 영점을 보정하거나, 그 영점이 2048에서 k카운트 이상 벗어나면 `chain_depart` 창을 열지 않는 규칙, 또는 garage 시간 항을 측정 산포(0.0025~0.003 rad/s)로 올리는 선택지가 있습니다.

### P5. 집 주차장(G2의 주 대상)은 효과가 없을 가능성이 큽니다

- 기록 "위험과 한계"가 50~60°라고 적었고, 내 산술(저장 unc 약 35° + 상승 43° ≈ 78°)은 더 큽니다. 60°를 넘으면 `unc_grew`로 출구 전에 닫히므로 안전 쪽이지만, 다음 주행의 기대를 요약 문장에 명시해야 합니다("집 출구는 이 빌드로도 바뀌지 않을 수 있음").

### P6. 창이 열린 뒤 정차 중 재설정으로 래치가 떨어지는 변형(C1의 (b))

- FACT: ACTIVE의 `yaw_break`는 정차 중(`parked`) 끊김을 허용하고(`g3_send.h:173-175`), 그 재설정은 래치를 떨어뜨립니다. 차가 R인 채 서 있다가(출구 차단기 앞 후진 보정) 움직이면 창은 전진 방위를 계속 보냅니다. 운전자가 D로 바꾸면 0x118 변화로 끝납니다. 드뭅니다.

### P7. 턴테이블·기계식 주차

- 바퀴가 돌지 않고 위치도 같으므로 표시도 위치 검사도 못 잡습니다. beta.15는 이 차에서 창이 열리지 않아 무관했으나 이제 유효한 위험입니다. 한도는 P1과 같습니다. 문구에 적으십시오(FIRST_CONNECTION 기록은 "turntable"을 이미 언급).

### P8. IDLE 요 끊김 감시가 효과를 자주 없앨 수 있음(안전 쪽)

- `g3_send.h:252-259`: 첫 바퀴 움직임 뒤 서 있지 않을 때의 `breaks` 증가 한 번이면 그 부팅의 창은 영원히 `none/yaw_break`입니다. `breaks`는 모든 재설정과 250 ms 넘는 요 구멍, 가정 시드 폐기(`pipeline.cpp:1123`)에서 늘어납니다. 지하 상승 1~3분 동안 한 번도 없을지는 자료가 없습니다. 다음 주행의 `none` 사유 분포를 보십시오.

### P9. 60 s fsync 재기록이 출구 차단기 정차와 겹치는 경우(기존 동작)

- 교체가 끝난 뒤(출구에서 GPS 복귀, 정차 ≥ 60 s) `sync_ok`가 참이 되어 fsync 쓰기가 요청됩니다. 그 순간은 GPS 복귀 사건의 RAW 드레인과 창의 publish 단계와 겹칩니다. 요청은 드레인 동안 막히고(idle 규칙) 교체가 다시 시작되면 취소되므로 새 결함은 아니지만, 이 변경으로 그 시점이 창과 겹칠 확률이 커졌습니다.

## 영역별 점검 결과 (찾지 못한 것 포함)

### (A) 전달 계약

- 송신 바이트 변경 경로는 `encode_bearing_overlay` 하나이며 바뀌지 않았습니다(FACT, `adapter.cpp:519-532`: 48바이트 복사 뒤 [40]=1, [44..47]만). 후보 게시(`G3Send::publish`)는 BETA ARMED·현재 generation의 FIX POSITION을 본 뒤에만, 게이트는 `end()`에서 동기적으로 닫힘, "진행 중 호출 최대 1회"는 beta.15 그대로입니다.
- 거부 순서: 첫 FIX의 위치·나이 검사(`g3.position`, `runtime.cpp:1872`)가 같은 워커 턴의 `g3.tick`/`g3_send.tick`(1977~1990행)보다 먼저 돌고, 첫 FIX 전에는 후보가 없으므로 거부된 방위가 송신될 틈은 없습니다(FACT).
- 끄기: `g3-garage-off`면 `garage_`가 거짓이라 beta.16 분기만 돕니다(FACT: `G3Tracker::start`, `G3Send::start`, `g3_io_block(..,garage=false)`, `rotation_coef`). 워커 시나리오 g3_park/g3_cancel/g3_move/g3_stop/g3_fault는 표식을 두고 돌며 `"garage"` 필드 없는 boot 행을 단언합니다(FACT, diff). 20개 부팅 재생의 바이트 동일성 주장은 그 구조상 타당합니다(직접 재생하지 않음).
- 구 빌드가 쓴 기록(플래그 1~3): `depart` 거짓 → beta.16 경로(CARRIED_UNVERIFIED/age_unknown, 창 없음). 구 빌드가 새 기록을 읽으면: 옛 `parse_u64(...,3,...)`가 플래그 4~7을 FORMAT으로 거부(FACT, diff 284행) → `no carry`. 손상·절단: CRC·한 줄·16토큰·크기 ≤ 256 검사 그대로, 임시 파일은 읽지 않음, rename 원자성 그대로. 다른 차의 기록: 저장 파일은 CMU 내부 `logs/`에만 있어 해당 없음. 견인·배터리 분리: 첫 FIX 위치 검사와 `age`(GPS utc 기준, 7일)만이 방어이며 기존과 같습니다.
- 동시 기록자: 저장 스레드 하나 + 1슬롯 우편함 + epoch(ABA) 그대로. 표시도 같은 게이트(open/fsync/rename)와 500 ms stale 규칙을 지킵니다(FACT).
- 틀린 이월 방위의 손해 한도 대 "아무것도 안 함": 정차 ≤ 900 s(그동안 순정은 0 또는 잡음 방위를 보냄), 움직인 뒤 ≤ 60 s·충돌 8회·첫 M 진행방향. 순정의 출구 오류 40 s(150~180°)와 같은 급. 차이는 **지상 출발처럼 순정이 맞았을 때** P1/P7의 틀린 이월이 몇 초~8 s 동안 순정을 덮는다는 점입니다.

### (B) 출발 표시

- 흔들림: 요는 바퀴가 움직일 때만 적분되고(`pipeline.cpp:1048`), 거리는 바퀴 속도에서만 옵니다. 승객·바람·핸드브레이크 요동으로는 M이 생기지 않습니다(FACT). 0.5 m 기어가기는 M을 만들지만 다음 ≥ 5 s 정차에서 `head_valid_`이면 `turned` 조건(`!(disk_flags_&HEADING)`)으로 S가 다시 쓰입니다(FACT, `g3_heading.h:1163-1164`, 시험 1470~1474). BETA 방위가 없을 때만 M/hv=0이 남습니다(안전 쪽).
- 표시 누락: P2. 추가로 `MISSING_SENSOR` 중에는 바퀴 이벤트가 drain되지 않아 `distance_m`가 늘지 않으므로 표시가 쓰이지 않습니다(그 상태로 차가 움직이면 P1 부류).
- 전원 차단 중 쓰기: temp(O_EXCL) → write → (fsync 생략) → close → rename. 데이터 전 rename 커밋으로 빈 파일·쓰레기가 남으면 EMPTY/TRUNCATED/CHECKSUM → 이월 없음(안전). 위험은 "이전 S 복귀"뿐이며 기록이 적었습니다.
- 60 s fsync 재기록과의 정합: 표시 뒤 `disk_synced_=false`, `written_=true`이므로 ≥ 60 s 정차에서 "새로 저장할 것 없음"이어도 fsync 쓰기가 한 번 더 나갑니다(hv=0 S 가능, #11 seq 32와 같은 부류). 파일 유효성은 유지됩니다.

### (C) 저장·시간

- "교체 중 fsync 없음": 요청 시 `sync_ok`(`g3_heading.h:1153, 1170`), 진행 중 sync 쓰기는 `g3_cancel_ignores_replacing`이 거짓이라 `beta_replacing`으로 취소(`runtime.cpp:1980`), 시험 1445~1453·1514~1526과 변이 M7이 잡음(FACT). 이미 시작한 fsync는 중단 불가(기존 잔여 위험, 변화 없음).
- 저장 스레드 취소 경합(G3_CANCEL_FLAKE): 표시는 바퀴·stale 취소에서 빠지므로 "꼬리 재시도" 유형이 하나 더 생기지만 모두 rename 게이트 전에 멈추고 임시 파일을 지웁니다. 새 경합은 찾지 못했습니다.
- 잠금·순서: 워커 단일 스레드에서 tick → cancel → pump → send tick 순서 그대로(FACT).
- 벽시계: 새 분기는 `rt_s`·`utc_s`를 순서 판정에 쓰지 않습니다. 첫 FIX의 `age_negative`·`age > 7 d` 거부는 그대로 적용됩니다(FACT).

### (D) 불확실도

- P4·P5. 레일 포화 항·시간 항은 그대로이며, `rail_excess_rad`는 계속 더해집니다(FACT). 후진 기동은 차체 적분에 영향이 없고(요는 차체 회전), 진행 방향만 래치 문제(C1)입니다. U턴·나선 1,250°는 2 %로 25°를 더합니다.

### (E) 시험·도구·포장

- 새 시험은 옛 동작에서 실패합니다(내 변이 M1~M7, M9, M10 모두 첫 CHECK에서 중단). M8만 생존(C2).
- `g3_send` 워커 시나리오들은 표식 없이(garage 켬) 돕니다. beta.15의 "미확인 래치 → 30 s 대기 → none" 경로는 이제 `test_g3 g3_send_rules`(`start(true)` = garage 꺼짐)만 덮습니다. 괜찮지만 알아 두십시오.
- 재생 시험: C3. 재생기에 idle 규칙이 없어 표시 누락(P2)은 재생으로 볼 수 없습니다.
- 포장: 메뉴 12는 메뉴 10 패턴 그대로(POSIX sh, `printf`, `IFS= read -r`, `mv -f`, Shift 문자 없음, 40열 검사는 시험에 있음). `install.sh`(메뉴 1)·`uninstall.sh`·메뉴 4가 표식을 지워 **재설치하면 조용히 ON으로 돌아갑니다** — 메뉴 10/11과 같은 의도된 패턴이나 현장 문서에 한 줄 있으면 좋겠습니다. 상태 줄 `g3_garage_marker=`와 verdict `GAR …`는 awk `field()`와 맞습니다. 순정 BusyBox 하네스는 돌리지 않았습니다(기록도 같음). ARM 목록·Makefile 목록에 `g3_garage` 추가됨(FACT).

### (F) AGENTS.md 예외 문구

대체로 정확하나 다음을 고쳐야 합니다.

1. "an unknown reverse latch counts as forward" → "a reverse latch that is not valid (no 0x118 drained since the session/bus admission or since the last pipeline reset; a message before admission is excluded) counts as forward" 로 바꾸고, C1의 손해(후진으로 빠져나가는 동안 180° 반대, 첫 0x118 변화까지)를 적으십시오. 현재 괄호("no 0x118 change since boot: the car was not shifted into reverse")는 사실이 아닙니다.
2. "the propagated uncertainty uses a 2 % rotation term" → 코드를 `garage_carry_`로 고치지 않는다면 "every record this build stores, and every carried heading while the marker is absent, grows its uncertainty with a 2 % rotation term (the non-departure limit stays 30 deg)"로.
3. "no fsync runs while BETA replaces" → "no fsync is requested while BETA replaces and an in-flight fsync write is cancelled before its fsync/rename gate; an fsync already executing cannot be interrupted (unchanged residual)".
4. 추가: "the departure mark (while moving) and the 5 s park record are the first store writes that run while BETA replaces and while the car moves; the 2026-10-11 H1 'parked write only' invariant no longer holds" — 그리고 사전 등록 규칙이 사후 검출임을.
5. 추가: "an S record also survives a boot in which the product did not observe the drive (journal failure, worker not started, no sensors, no trace); the chain token only changes on a guard decline; such a record opens the window with a heading wrong by that drive's rotation until the end conditions" (P1).
6. "state S (a departure from it would have replaced it by M)" → "(a departure that the product observed would have replaced it by M, if the mark landed: no fsync, idle rule, 5 s rate)".
7. 60° 한계가 **플래그 4 기록**에만, 2 %는 스위치 전체에 적용된다는 구분(C2와 같은 문제).

## 권고 (리드·소유자 결정 사항)

1. 설치 전 필수: C1 문구 수정(+ 가능하면 `first_move_reverse==-1 && breaks 변동 없음`으로 제한), C2의 의도 결정과 시험 고정, C3의 공허 단언 제거.
2. 강하게 권고: P1의 "직전 제품 부팅" 토큰(작은 파일 하나, 시작 시 1회 쓰기)과 분석기의 "표시 없이 떠난 S" 계수. 이것 없이는 "S = 서 있었음" 불변식이 이 차의 실제 운용(트레이스 없는 부팅이 있었음)에서 보장되지 않습니다.
3. 기록에 적을 것: P3(G1 경로의 새 I/O 노출과 사후 검출), P4(영점 산포 대 시간 항 숫자), P5(집 출구 기대치), P7(턴테이블).
4. 다음 주행 분석에 추가: `g3_send none` 사유 분포(P8), 표시 지연(`g3_store written st:M`의 `mono_ns` − 출발 시각), garage 쓰기 중 BETA 상태별 `lat_ms` 분포(P3).

## 하지 않은 것

- 차량·폰·DHU·ARM/QEMU 실행, 전체 `make test`(별도 진행 중), 비공개 주행 자료 재생(기록의 20개 부팅 동일성은 구조 검토만), 순정 BusyBox 하네스, 저장소 플래시 마모 정량화.
