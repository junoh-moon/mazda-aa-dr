# G3 지하주차장 출구 방위 수정 (garage carry, 2026-10-17)

상태: 원인 분석, 구현, 호스트 시험, 정확한 ARM 빌드·시험, 비공개 주행 기록의 오프라인 재생과 디지스트 수준 재구성만 했습니다. 차량·폰·DHU에서는 실행하지 않았습니다. 이 기록은 릴리즈 승인이나 차량 방문 요청이 아닙니다. 좌표는 적지 않았습니다. 표시: **FACT**(로그·코드에서 직접 확인), **INFERENCE**(해석·모델). 2026-10-18 독립 검토(`G3_GARAGE_EXIT_FIX_REVIEW_FABLE_2026-10-18.md`)의 지적을 고쳤습니다(아래 "독립 검토 반영" 절이 위의 표·문구보다 우선합니다). 2026-10-18 2차 검토(`G3_GARAGE_EXIT_FIX_REVIEW2_FABLE_2026-10-18.md`)의 지적은 "세 번째 수정" 절에서 고쳤고, 그 절이 다시 앞의 절들보다 우선합니다.

## 문제 (소유자)

지하주차장에서 지상으로 올라온 뒤 네이버 지도의 진행 방향(화살표·경로 방향)이 실제 차 방향과 맞지 않아 안내를 쓸 수 없습니다.

## 자료와 방법

- 자료: 2026-10-17 USB 내보내기 `trace.2/1/0.jsonl`(14개 부팅, 그중 앞 9개는 이전 주행, #10·#11은 이전 beta 주행, #12는 짧은 주차 부팅, #13·#14가 이번 주행), `g3-heading.v1`. 비공개 폴더 밖으로 복사하지 않았습니다(스크래치 분석만).
- persistent 프로파일은 원시 motion 행을 트리거 창(앞 60 s, 뒤 30 s)에만 남깁니다. 그래서 `replay_beta`로 이 부팅들을 제품 그대로 재생하면 창 밖에서 바퀴 신호가 끊겨(`wheel_stale`) G3 창이 닫힙니다. 실제 자료의 수치는 (1) 10초 `log_digest`의 1초 요 합(`y1`, 움직일 때)과 (2) 원시 창 안의 yaw 이벤트를 제품과 같은 방식(이동 중, 창 평균 × 창 길이, 영점 2048)으로 적분한 **혼합 재구성**으로 얻었습니다(INFERENCE 수준의 모델이며 제품 재생이 아닙니다).
- 참값: GPS 진행방향(15 km/h 이상, 안정 구간). GPS도 참값이 아니며 기준입니다.

## 부팅별 G3 사슬 (FACT, 차량 행)

| 부팅 | 시작 시 저장 기록 | 복원 상태 | 창(g3_send) | 저장(written) | 출구에서 본 것 |
|---|---|---|---|---|---|
| #10 5a9f9b2b (이전 주행, 구 빌드) | 없음 | UNKNOWN missing | (구 빌드, 송신 없음) | 11회, hv=0 8회 | 해당 없음 |
| #11 41945267 (구 빌드) | #10의 hv=0 | UNKNOWN no_heading | (송신 없음) | 20회, hv=0 1회(마지막 seq 32) | 해당 없음 |
| #12 07ea46fa (짧은 주차 부팅, AA 없음) | #11 seq 32(hv=0) | UNKNOWN no_heading | - | 없음 | 차 정지(0.2 km/h 이하 흔들림만) |
| #13 3121d513 (집 지하 나선 출구로 시작, 이마트 지하에서 끝) | #11 seq 32(hv=0) | UNKNOWN no_heading | `none/no_heading` | 6회(hv=0 3회), 마지막 hv=1 seq 38 at 780.8 s(163.6°, ±7.9°) | 집 출구: 아래 참조 |
| #14 da8288d5 (이마트 지하 시작, 집 지하에서 끝) | #13 seq 38 | **CARRIED_UNVERIFIED age_unknown**(chain ok, 벽시계 1970) → 127 s `unc_grew`(30.3°, 37 m) | `none/age_unknown` | 10회(hv=0 1회), 마지막 seq 48 hv=1 269.7° ±69.7° | 이마트 출구: 아래 참조 |

- #13 집 출구(FACT, 원시 send 행): 나선을 오르는 동안(144~280 s, 순회전 약 −1,000°) 순정은 LDS 저장값(위치 고정, 방위 322°)을 보냈고, 280~355 s 출구에서 정지한 채 GPS가 돌아온 뒤(328.9 s) 순정 방위는 333~354°(정지 GPS 잡음)였습니다. 이후 확실한 GPS 진행방향(388 s, 60°)에서 요 적분을 거꾸로 돌린 출구 정지 방위는 약 152°입니다. **출구에서 약 40초 동안 순정 방위가 150~180° 틀렸고**, 이어 1~7 km/h로 움직인 360~379 s에는 157, 222, 286, 248, 242, 138, 54°처럼 흩어졌습니다(INFERENCE: 소유자가 말한 증상과 일치). 이 부팅에는 이월할 방위가 없었습니다.
- #14 이마트 출구(FACT): 첫 GPS 위치는 184.9 s(mode 2, 정확도 198 m) ~189.9 s, 190.9 s부터 mode 1 5.6 m였고 그 진행방향(224~228°, 15~20 km/h)은 혼합 재구성과 대체로 맞습니다. 이 출구에서 순정 방위는 첫 유효(30 m 미만) 위치부터 쓸 만했습니다. 차량의 G3 `course` 행: 저장 방위 163.6°는 GPS 317°와 −153.4°(err_c), 적분한 차체 방위 271.2°는 −45.8°(err_p) 어긋났습니다.
- 사전 등록 규칙(G3_RECORD_ONLY v3b)의 쓰기 겹침 검사는 이번 내보내기에서 48회 쓰기, 최대 184.8 ms, 겹침 0으로 **발동하지 않았습니다**(FACT, 분석기).

## 근본 원인

### (a) 나이 검사는 이 차에서 통과할 수 없습니다 (FACT)

- 저장 파일 마지막 기록(#14, mono 1,082 s): `utc_s`=1792234543(GPS 시각), `rt_s`=**1082**. CMU의 CLOCK_REALTIME은 GPS를 받은 뒤에도 부팅 후 초(1970)입니다. 지하뿐 아니라 **어떤 시동에서도** 작업자 시작 때 나이(`age_src rt`)를 알 수 없습니다.
- beta.15/16의 송신 창은 `CARRIED`(reason `chain_ok`)에서만 열리고, `chain_ok`는 벽시계 나이가 필요합니다. 첫 위치의 GPS 나이·위치 검사로 올라간 `position_ok`는 검토 결정으로 창을 열지 않습니다. 따라서 **beta.15의 첫 연결 방위 송신은 이 차에서 한 번도 열릴 수 없는 구조**입니다(17개 비공개 부팅 재생에서 `--g3-rt` 없이 CARRIED 시작 0개였던 것과 같은 원인).

### (b) 150° 차이: 저장 기록이 낡았습니다 (FACT + INFERENCE)

- FACT: #13의 마지막 저장은 780.8 s(163.6°)입니다. 그 뒤 차는 약 1.8 km, 회전 절댓값 약 1,250°(순 +81°)를 더 달려 1,091.3 s에 이마트 지하에 정지했고, 7.5 s 뒤(1,098.8 s) 요 신호가 끊겼습니다(시동 끔, `sensor_silence`). 이 마지막 정차는 (1) 15 s 미만이었고 (2) BETA가 ENGAGED(GPS 상실 교체 중)라 저장이 막혀(`beta_replacing`) 기록되지 않았습니다. 요 신호가 끊긴 뒤에는 파이프라인이 `MISSING_SENSOR`로 계속 재설정되어 바퀴 이벤트가 G3에 오지 않으므로(`wheels_fresh` 거짓) 그 뒤에도 쓸 수 없습니다. 출발 표시(이동 기록)가 없어서 다음 부팅은 780.8 s 기록을 "주차 방위"로 읽었습니다.
- INFERENCE(혼합 재구성, 영점 2048): 하강 직전 GPS 진행방향(995.7 s, 47°)에서 하강(순 −181°, |회전| 799°, 이동 96 s)을 적분한 주차 방위는 약 225.5°로, 저장값 163.6°보다 약 62° 다릅니다. 이 주차 방위에서 #14의 상승(이동 81 s, |회전| 622°)을 적분하면 출구 첫 위치들(186.9~189.9 s)에서 GPS와 **+14~+19°**(영점 2047: +8~+12°, 2049: +21~+26°)입니다. 1초 디지스트만 쓴 추정은 +1~+6°였으므로 이 구간의 재구성 불확실성은 약 ±10°입니다. 즉 150° 중 대부분은 낡은 기록 때문이고, 적분 자체의 오차는 수십 도 이하입니다.
- 불확실도 모델: 규칙 5의 회전 항(|회전|의 10 %)은 지하 나선에서 37 m 만에 30°를 넘겨(`unc_grew`, 127 s) 창을 닫았습니다. 위 재구성에서 |회전| 1,400° 이상 뒤 남은 오차는 약 15~20°였습니다(표본 1개).
- 출발 시 후진·포화: #14 출구까지 후진 신호(0x118)는 한 번도 오지 않았습니다(첫 이벤트 929 s). 요 레일 포화는 이 구간에 없었습니다(FACT, 디지스트 최댓값). 정차 중 영점은 정차마다 2040.7~2052.3 카운트로 크게 달랐습니다(FACT, `yst/ystn`) — 이동 중 적분은 고정 2048을 씁니다.

### (c) 정차 기록의 hv=0 (FACT)

이번 내보내기의 쓰기 47회 중 13회가 hv=0입니다.

| 부류 | 횟수 | 원인 |
|---|---|---|
| 부팅의 첫 BETA 시드 전 정차 | 6 (#10 2, #13 3, #14 1) | 마지막 기록 뒤 2 m 넘게 움직였으므로 새 기록이 필요하지만, 이 부팅에서 아직 GPS 진행방향으로 시드된 방위가 없어 방위 없는 S를 씁니다(낡은 방위를 지우는 것은 맞음). |
| 구 빌드(#10, #11)의 재설정·연결 끊김 뒤 | 7 (#10 6, #11 1) | 행에 `ref_s`가 없는 beta.11 이전 빌드입니다. 요 BAD_INPUT 재설정(#10 2,026 s)이나 AA 연결 끊김(cadence fence, #10 2,520 s, #11 3,915 s) 뒤 BETA가 시드를 잃으면 그 순간의 BETA 방위만 저장했습니다. #11 seq 32는 같은 정차의 60 s fsync 재기록이 15 s 기록(37.0°)을 hv=0으로 덮었습니다. 현재 코드(beta.11 "reference carry")는 BETA가 시드를 잃어도 참조 방위를 이어 가며 사슬 끊김에서만 무효로 합니다. 이번 주행(#13, #14)에는 이 부류가 없습니다. |

정차 저장 조건(바퀴 넷 모두 정확히 0, 15 s, 신선)은 hv와 무관합니다. hv를 정하는 것은 이 부팅의 시드 여부와 요 사슬입니다.

## 바뀐 동작 (garage carry, BETA, 기본 켜짐, 끄기: 메뉴 12 = `g3-garage-off`)

| 부분 | 규칙 | 상수 |
|---|---|---|
| 출발 표시 | 디스크의 S 기록(방위 있음) 뒤 차가 0.5 m 넘게 움직이거나 2° 넘게 돌면 방위 없는 `M` 기록으로 바꿉니다(fsync 없음, 5 s 비율 제한·유휴 규칙, 바퀴 움직임·신호 끊김으로는 취소하지 않음). S 기록 = "저장 뒤 서 있었음" | `G3_GARAGE_DEPART_M` 0.5 m, `G3_GARAGE_DEPART_DEG` 2° |
| 정차 기록 | 정확히 0인 정차 5 s(15 s 대신). BETA가 교체 중(GPS_LOST/ENGAGED/NO_FIX/SPEED_ENGAGED)이어도 fsync 없이 씁니다. fsync 쓰기(60 s)는 교체 중이 아닐 때만, 교체 중 fsync 쓰기는 계속 취소 | `G3_GARAGE_PARKED_NS` 5 s |
| 기록 표지 | 새 기록은 flags 4(`G3_FLAG_DEPART`). 구 빌드는 flags > 3을 형식 오류로 거부(안전 쪽) | - |
| 복원 | flags 4, S, 방위 있음, 연쇄 ok, 다른 부팅이고 (2026-10-18 수정, P1) 기록을 쓴 부팅이 **직전 제품 부팅**(`logs/g3-last-start`)이면 벽시계 없이 `CARRIED`(`chain_depart`); 아니면 `CARRIED_UNVERIFIED`(`not_prev_boot`, 창 없음). 나이를 알면 7일 초과는 거부. 첫 위치의 위치·GPS 나이 검사, `moving_at_boot`는 그대로 | `G3_LAST_START_NAME` |
| 불확실도 | (2026-10-18 수정, C2·P4) garage 사슬, 즉 garage 켠 부팅이 저장하는 모든 기록의 불확실도와 이월한 플래그 4 기록의 전파: 회전 항 2 %, 시간 항 0.0023 rad/s(BETA 10 %, 0.002 rad/s), 레일 여유 그대로, 플래그 4 기록의 한계 60°. 플래그 4가 없는 이월 기록은 BETA 항과 30° 그대로. 표식이 있으면 플래그 4 기록은 beta.16처럼 형식 오류로 거부 | `G3_GARAGE_ROTATION_PER_RAD` 0.02, `G3_GARAGE_YAW_ERROR_RAD_S` 0.0023, `G3_GARAGE_UNC_MAX_DEG` 60° |
| 송신 창 | `chain_depart`에서도 열림. 부팅 뒤 이미 움직였어도 열림(출구에서야 첫 FIX가 오므로), 단 첫 바퀴 움직임 뒤 서 있지 않을 때 요 사슬이 끊기면 열지 않음(`none/yaw_break`). 후진 래치(2026-10-18 수정, C1): 유효한 전진이면 열림; 유효하지 않으면(시동 뒤 0x118 변화 없음, 연결 전 메시지 배제, 모든 재설정·래치 지움) 잃어버린 후진 정보가 없고(`reverse_unsure` 0) 시동 뒤 20 m 이상 움직였을 때만 전진으로 봄; 후진이거나 정보를 잃었으면 기다리고, 열린 창은 후진 또는 정보를 잃은 무효 래치로 끝. 그 밖의 끝 조건(신뢰 4회, 충돌 8회, 정책 M, 900 s/60 s, 세션, 이상)은 beta.15 그대로 | `G3_SEND_UNKNOWN_LATCH_MIN_M` 20 m |
| 행 | 쓰기 결과 행에 `"garage":1`(표시는 `"mark":1`), `shadow_boot` g3에 `"garage":1`, `g3_send start`에 `"garage":1,"moved":0/1`. 분석기: garage의 fsync 없는 쓰기는 GPS 상실과 겹치는 것이 설계이므로 저장 규칙에서 빼고 별도 규칙(아래)으로 셉니다 | - |

근거: 지하 주차의 마지막 정차는 짧고(7.5 s) 교체 중이며 그 뒤 시동이 꺼집니다. fsync 없는 110바이트 rename 쓰기는 저장 스레드에서 수 ms(이번 내보내기 sync 0 쓰기 3.5~67.7 ms)이고, 위험이 컸던 것은 fsync였습니다(v3b). 5 s는 이번 마지막 정차(7.5 s)를 잡는 값입니다. 2 %는 이마트 사슬의 잔여 오차(약 15~20°)를 |회전| 1,400°에 대해 덮고, 순정 출구 방위의 150~180° 오류보다 작은 60° 안에서만 보내기 위한 값입니다(표본 1개라 근거가 약합니다).

**새 사전 등록 규칙(분석기에 구현, 단위 시험):** 다음 주행에서 `"garage":1,"sync":0` 쓰기 중 하나라도 `lat_ms` > 500이거나 그 창 [start_ns, end_ns + 2 s]에 `beta_journal_lag` "lagging"이 있으면 다음 빌드는 garage carry를 기본 끔(메뉴 12 선택)으로 합니다. 분석기 출력 `G3 garage writes without fsync: n, lat_ms > 500: k, journal lag overlaps: m`.

## 미리 정한 수용 기준 (구현·재생 전에 작성, 2026-10-17 22:30 KST)

탐색적 재구성(위 (b)의 이마트 사슬 수치 일부)은 기준 작성 전에 보았습니다. 기준은 그 뒤, 제품 변경과 재생 전에 정했습니다.

- (A) 실제 자료: 새 규칙에서 `CARRIED/chain_depart`로 시작하는 지하 출구(NO_FIX 이동 30 s 이상 뒤 첫 FIX)마다, [첫 FIX, +10 s]의 바꾼 송신이 모두 기준 진행방향(첫 NNG 신뢰 4회 순정 진행방향을 요 적분으로 역전파)과 30° 안. 통과: 그런 출구의 2/3 이상 통과이고 최악 60° 이하. 3개 미만이면 **판정 불가**.
- (B) 끔이면 비공개 전 부팅 재생이 master와 바이트 동일, 켬이면 바뀐 송신은 바이트 40, 44..47만 다름, 교체 중 fsync 요청 없음, 출발 표시는 방위 없음.
- (C) 합성 지하 사슬(제품 코드): 출구 첫 FIX 무렵 창이 열려 있고 바꾼 방위가 합성 참값과 5° 안, 끔이면 overlay 없음.
- (D) 추가 행이 정차당 1개 이하로 묶이고 persistent 예산 600 KB/h 미만.
- (E) 새 논리의 손 변이 8개 이상이 각각 시험 하나 이상을 실패시킴.

## 결과

- **(A) 판정 불가.** 해당 출구는 이마트 1개뿐입니다(#13 집 출구는 이월할 기록이 없었고, 그 전 이동이 로그에 없어 참값 사슬이 없습니다). 이마트 출구의 모델 값(INFERENCE, 혼합 재구성): #13 마지막 정차는 1,096.3 s(BETA ENGAGED)에 fsync 없이 저장되고 불확실도 34.8°(참조 7.9° 가정), #14에서 `chain_depart`로 복원, 출구에서 바꾼 송신 186.9/187.9/188.9/189.9 s의 오차 **+14.3, +19.1, +19.3, +16.1°**(영점 2048; 2047은 +7.6~+12.5, 2049는 +21.0~+26.0), 그때 불확실도 56.6~57.3°(한계 60°까지 약 3°), 189.9 s에 신뢰 4회로 끝. 30° 기준 안이지만 **이 네 송신은 모두 정확도 198 m의 mode 2 위치라 네이버가 30 m 기준으로 무시했을 것**입니다(INFERENCE). 이 출구에서는 190.9 s부터 순정 진행방향이 이미 쓸 만했습니다.
- **(B) 통과.** 20개 부팅(재생 가능 14, 위치 행 없음 6)에서 `--g3-garage off` 대 master 110개 파일(report, csv, journal, g3 행, sends, stdout/stderr; 저장 스레드 시각 필드 정규화) 모두 동일. 켬: g3 행과 send 기록의 주석(phase/g3 상태, 사유 코드)만 다르고 바꾼 송신 0개(persistent 재생은 원시 창 밖에서 `wheel_stale`로 창이 닫힘 — 근거가 아님). 바꾼 송신이 바이트 40, 44..47만 다르다는 것의 근거는 beta.15의 `test_adapter beta_bearing`(어댑터 부호화)과 `tests/runtime/g3_send_identity.py`(워커 전체 페이로드 비교)입니다. 재생 시험의 `--g3-send-out` 기록은 바이트 40과 44..47만 담으므로 그 밖의 바이트가 같다는 것을 보여 주지 않습니다(2026-10-18 정정). 교체 중 fsync 금지와 표시의 방위 없음은 단위 시험.
- **(C) 통과.** 합성 사슬: A는 137 s 이후 ENGAGED 중 fsync 없이 S(flags 7) 저장, B는 `chain_depart`로 복원, 창은 정차 중 열려 지하 상승(−600°, 45° 회전) 동안 유지, 출구 첫 두 FIX 송신은 원본(BETA가 SPEED_ENGAGED를 떠나는 세대 전환, beta.15와 같음), 세 번째부터 바꿈, 오차 0.05°, 2회 뒤 `trusted`. 끔: 저장 없음(교체 중), 복원 없음, overlay 없음.
- **(D) 통과.** log_rate persistent 1시간: 쓰기 25회(표시 13회), 추가 6.4 KB/h. 최악 경계: 쓰기는 5 s당 1회라 시간당 720행 × 약 300 B ≈ 216 KB/h(정차·출발이 5 s마다 반복될 때).
- **(E) 통과.** 23개 변이 모두 잡힘(아래). 처음 runtime.cpp에 넣은 "표시도 바퀴 움직임으로 취소" 변이는 살아남았습니다(쓰기가 너무 빨라 틱이 비행 중을 보지 못함). 취소 규칙을 `g3_cancel_reason()` 헬퍼로 옮겨 직접 시험한 뒤 잡혔습니다.

변이(각각 단독 적용, 모두 잡힘): 회전만 한 출발 무시, 출발 거리 5 m, 교체 중 fsync 허용, 연쇄 없이 chain_depart, 출발 표지 미기록, 회전 항 10 % 유지, 한계 30° 유지, 후진 중 창 열림, 후진 미확인이면 창 종료, 대기 중 요 사슬 끊김 무시, 정차 15 s, 표시 비율 제한 무시, 표시 유휴 규칙 무시, 표시를 S로 기록, 스위치 꺼도 표시, 표시를 바퀴 움직임으로 취소, 표시를 신호 끊김으로 취소, 교체가 fsync 쓰기를 취소하지 않음, runtime이 garage 유휴 규칙 미전달, runtime이 교체 상태 미설정, 스위치 무시(항상 켬, g3_park가 잡음), 분석기 garage 쓰기를 저장 규칙에 포함, 분석기 GPS 상실을 garage 규칙에 포함.

## 시험 (2026-10-17, 스크래치 사본)

- 호스트 `make test`(D-Bus 헤더·라이브러리를 스크래치 경로로 지정): 첫 실행에서 test-tools까지 모든 대상이 통과하고 test-tools에서 1건 실패(`test_analyze_diag` 레거시 보고서 바이트 동일성: 분석기 JSON에 garage 필드가 항상 붙음). garage 쓰기가 있을 때만 필드를 붙이도록 고친 뒤 `test-tools` 293건 OK(건너뜀 5), 이어서 `test-integration`, `test-replay-beta` 29건 OK(건너뜀 2) 통과. 첫 실행의 unittest 합계 1,084건(건너뜀 48: 비공개 순정 루트가 필요한 시험), C++ 시험 모두 통과(`test_g3` 1,174 검사, navigation 3,977, core 1,872 등).
- `test_worker_beta` 25개 시나리오 모두 통과(새 `g3_garage`: 1번째 기록 SPEED_ENGAGED 중 sync 0, 2번째 60 s fsync, 3번째 출발 표시가 주행 중 취소 없이 기록; 기존 `g3_park/g3_cancel/g3_move/g3_stop/g3_fault`는 `g3-garage-off` 표식으로 beta.16 규칙 회귀 시험으로 유지).
- 정확한 ARM: 고정 GCC 4.9.1로 `tools/build_arm.py` 검증 빌드 통과, `tests/run_arm_all.sh`(qemu-arm) PASS 229, FAIL 0, SKIP 2(비공개 LDS·AA 순정 필요), `ARM_TIMING_RETRIES=0`, `ARM_TIMING_FAILED=0`. `worker-beta-test g3_garage`를 시간 계열 목록에 추가.
- 비공개 재생: 20개 부팅, `--g3-garage off` 대 master 110/110 동일(최종 바이너리로 다시 확인).
- 포장: `test_trial_menu`, `test_trial_status`, `test_purge` 154건 OK(메뉴 12 추가 시험 포함). 순정 BusyBox 하네스(`cmu_emulation.py`)는 릴리즈 묶음이 필요해 실행하지 않았습니다.
- 실행하지 않음: 차량, 폰, DHU, AA 설치 탐침(비공개 순정 루트), 독립 검토.

## 위험과 한계 (첫 판; 아래 "독립 검토 반영"의 "남은 위험"이 우선)

- 실차·폰 미검증. 네이버가 정지·저속에서 차 방위를 어떻게 쓰는지는 여전히 모릅니다(DHU 10 미완). 실제 자료의 근거는 이마트 사슬 1개의 모델입니다.
- 집 나선 주차장은 회전이 커서(상승 |회전| 약 1,190°, 이동 약 165 s) 모델상 출구에서 불확실도가 50~60°에 이를 수 있습니다. 60°를 넘으면 창은 출구 전에 닫히고 순정으로 돌아갑니다(안전 쪽, 효과 없음).
- 출발 표시와 정차 기록은 fsync 없이 씁니다. 쓰기 직후 30 s 남짓 안에 전원이 끊기면 이전 내용이 남을 수 있습니다. 최악은 출발 표시가 사라져 오래된 S가 되살아나는 경우입니다(표시는 출발 직후 쓰고 지하 주행은 보통 수십 초 이상이라 그 사이 기록되었을 가능성이 큼, INFERENCE). 이번 #13은 정지 뒤 128 s 이상 CMU가 기록을 계속했습니다(FACT).
- 제품이 돌지 않은 이동(이번 #11 끝 → #13 시작 사이처럼 로그 없는 이동)은 연쇄 토큰이 바뀌지 않으면 잡지 못합니다. 첫 위치의 위치 검사(50 m + 위치 예산 + 부팅 뒤 바퀴 거리)는 지하에서 수백 m를 달린 뒤라 약합니다. 그런 경우 틀린 이월 방위가 출구에서 신뢰·충돌·한계까지 나갈 수 있습니다.
- 마지막 정차가 5 s 미만이면 저장되지 않고 디스크에는 출발 표시(M)가 남아 다음 시동은 이월하지 않습니다(안전 쪽).
- 출구 첫 두 FIX 송신은 원본입니다(세대 전환).
- 정차 영점이 정차마다 ±6 카운트 흔들립니다. 이동 중 적분은 고정 2048이며 1 카운트가 100 s에 약 3.8°입니다.
- 회전 항 2 %와 한계 60°는 표본 1개로 정했습니다.

## 다음 주행에서 보여야 할 것 (첫 판; 아래 "독립 검토 반영"의 목록이 우선)

1. 지하 주차 끝: `g3_store written "st":"S" ... "garage":1, "sync":0`가 마지막 정차 5 s 뒤에 있고 그 시점 BETA가 ENGAGED여도 됨. 출발마다 `"st":"M","mark":1`.
2. 다음 시동: `g3_store loaded reason chain_depart`, `g3_send start "garage":1`, 지하를 오르는 동안 `count` 행의 `unc`(60 미만인지), 출구에서 `choice 6` 송신과 `end`(trusted/m_course)와 `g3_first course`의 `err_p`.
3. 분석기의 `G3 garage writes without fsync` 줄: lat_ms > 500 0, journal lag 겹침 0(아니면 다음 빌드에서 garage 기본 끔).
4. 소유자 관찰: 집 출구 차단기 앞에서 정지했을 때 화살표 방향, 출발 뒤 몇 초 안에 도로와 맞는지.

## 독립 검토 반영 (2026-10-18)

검토: `validation/G3_GARAGE_EXIT_FIX_REVIEW_FABLE_2026-10-18.md`(C1~C3 CONFIRMED, P1~P9 PLAUSIBLE). 이 절이 위의 표·문구보다 우선합니다. 모든 변경은 `g3-garage-off` 표식 뒤에 있고, 예외는 P1의 토큰 파일 하나뿐입니다(아래). 차량·폰·DHU에서는 실행하지 않았습니다. 고친 것이 안전 쪽으로만 움직이는지가 우선이며, 그 대가로 효과가 줄어든 곳은 그렇게 적었습니다.

### 지적별 처리

| 지적 | 처리 | 바뀐 동작 |
|---|---|---|
| C1 미확인 래치 = 전진 | **고침** | 아래 "후진 래치 규칙". 문구도 코드 조건으로 고침(주석, 이 기록, 현장 문서, AGENTS 문안) |
| C2 2 %의 범위 | **고침(의도 결정)** | garage 사슬(저장 불확실도와 플래그 4 이월 전파)만 2 %; 플래그 4가 없는 이월 기록은 10 %·30°; 표식이 있으면 플래그 4 기록을 형식 오류로 거부(beta.16과 같음). 아래 "C2 결정" |
| C3 시험·도구 | **고침** | `or True` 단언 삭제, 바이트 동일성 근거 문구 정정, `g3_heading.h` 머리말의 "주차 쓰기만" 불변식 정정, 분석기의 저장 규칙 `max lat_ms`에서 garage 쓰기 분리(garage 줄에 따로 표시), garage의 `wait_s`를 5 s 기준으로 |
| P1 관측하지 않은 주행 뒤의 S | **고침(부분)** | 직전 제품 부팅 토큰(`logs/g3-last-start`). 분석기 "S records left without a departure mark" 계수. 남는 것은 아래 "남은 위험" |
| P2 표시가 idle 규칙에 막힘 | **문서화(변경 없음)** | 실제 자료에서 빈도 0/1, 아래 "P2 정량" |
| P3 교체 중 저장 I/O | **고침(경계)** | 교체 중 S 요청 간격 30 s, 교체 중 출발 표시는 기록 스레드 지연 100 ms 이하일 때만, 분석기의 BETA 상태별 garage 쓰기 줄. 출발 표시 자체는 간격을 두지 않음(아래 "P3") |
| P4 영점 산포 대 시간 항 | **고침(사전 등록 규칙)** | garage 사슬 시간 항 0.0023 rad/s. 그 결과 이마트 출구는 출구에서 창이 닫힘(아래 "P4") |
| P5 집 나선 주차장 | **문서화** | 효과 없을 가능성이 큼(아래) |
| P6 열린 창의 정차 중 재설정 | **고침(C1과 함께)** | 정보를 잃은 무효 래치로 창 종료 |
| P7 턴테이블·기계식 주차, 관측 안 된 주행 | **문서화** | 남은 위험과 AGENTS 문안 |
| P8 IDLE 요 끊김 감시 | **변경 없음** | 다음 주행에서 `none`/`wait` 사유 분포를 봄 |
| P9 60 s fsync 재기록과 출구 정차 | **변경 없음** | 기존 동작, 교체 중 fsync 요청 없음 그대로 |

### 후진 래치 규칙 (C1)

코드 사실: `G3Sample::reverse`가 −1(유효하지 않음)인 경우는 "시동 뒤 0x118 변화 없음"만이 아니라 세션·버스 승인 전 메시지의 배제(`exclude_reverse`), 모든 파이프라인 재설정(`drop_latch`), 원천 epoch 변경, 거부된 REVERSE, 입력 공백입니다. 그래서 "유효하지 않음 = 후진 아님"은 틀렸습니다.

규칙(가장 작은 안전한 규칙으로 고름): 파이프라인이 G3 관측값 `G3Motion::reverse_unsure`(부팅 수명 계수, 어떤 파이프라인 결정도 읽지 않음)를 셉니다. 늘어나는 경우: 유효한 래치가 지워짐(사유 무관), REVERSE 메시지가 `enqueue_raw`에서 거부되거나 재설정으로 대기열에서 버려짐, 배제된 REVERSE 메시지의 값이 전진 값이 아님(`Pipeline::exclude_reverse_event`, 런타임·재생기의 두 배제 지점), 래치를 지우는 움직임 입력 공백. garage 창은
- 유효한 래치가 전진(0)이면 엽니다(beta.15와 같음).
- 래치가 유효하지 않고 `reverse_unsure`가 0이면, 승인 뒤 첫 바퀴 움직임부터 센 바퀴 거리(`G3Motion::distance_m`) 20 m 이상일 때만 전진으로 보고 엽니다(그 전에는 `wait/latch_unknown`).
- 래치가 후진(1)이거나, 유효하지 않으면서 `reverse_unsure`가 0보다 크면 열지 않고 기다립니다(`wait/reverse`, `wait/latch_unsure`; 나중에 유효한 전진 래치가 오면 엽니다). 열린 창은 같은 조건에서 `end latch/reverse` 또는 `latch/unknown`으로 끝납니다(P6 포함).

근거: 0x118은 변화 때만 오므로 R로 넣거나 R에서 빼는 순간마다 메시지가 하나 생깁니다. 제품이 그 메시지를 받았다면 래치가 유효해지거나(그대로 씀), 잃었다면 계수가 늘어납니다(열지 않음). 배제된 전진 값(시동 순간의 0x118 묶음: 2026-10-17 내보내기에서 시동 묶음은 모두 값 0이었고 절반가량의 부팅에서 세션 승인 전이라 배제됨, FACT)은 "그때 R이 아니었음"이므로 막지 않습니다. 정보가 전혀 없는 경우(제품이 돌기 전에 R로 넣은 경우)는 남으므로, 그런 경우의 주된 위험인 "주차칸에서 바로 후진으로 빠져나감"을 20 m 문턱으로 막습니다(후진으로 빠져나가는 거리는 몇 m, 지하 출구는 수백 m: #14는 첫 FIX까지 원시 바퀴 자료가 덮는 구간만으로 약 227 m). 바퀴 속도는 부호가 없어 방향을 주지 않습니다(FACT: #14의 929.5~942.5 s 후진 래치 동안 바퀴 카운트 10118~10206 = +1.2~2.1 km/h). 그 밖의 선택지를 버린 이유: "유효하지 않으면 무조건 대기"(beta.15)는 이 차의 지하 출구에서 창을 없앱니다(#14는 929 s까지 0x118 메시지가 하나도 없었음, FACT). "배제된 메시지가 하나라도 있으면 대기"는 시동 묶음이 승인 전에 온 부팅(절반가량)에서 효과를 없앱니다. 틀린 180° 방위 몇 초의 비용(순정이 맞았을 지상 출발에서도 생길 수 있음)이 효과보다 크다고 보고, 정보를 잃은 경우는 전부 막았습니다.

남는 경우: 워커가 첫 움직임 메시지를 받기 전부터 R이었고(그 0x118 변화가 그 전) 그대로 20 m 넘게 후진하면, 첫 0x118 변화나 끝 조건까지 방위가 180° 반대입니다. 수동변속기를 R에 두고 주차한 뒤 R로 시동하고 20 m 넘게 후진하는 경우입니다(드묾).

### C2 결정

리드가 선호한 안(저장 불확실도는 beta.16의 10 %, 2 %는 이월 전파만)은 택하지 않았습니다. 이유(INFERENCE, 모델): 하강 구간의 회전이 저장 불확실도에 10 %로 들어가면 이마트 기록은 7.9 + 11.0 + 0.10 × 799 ≈ 99°로 60° 한계를 넘어 복원 자체가 거부됩니다. 하강 중 |회전|이 약 400°를 넘는 모든 지하주차장에서 기능이 사라지므로, 남는 것은 위험(새 I/O, C1, P1)뿐입니다. 대신 2 %가 쓰이는 범위를 garage 사슬로 고정했습니다: garage 켠 부팅이 저장하는 기록(모두 플래그 4)의 불확실도와 이월한 플래그 4 기록의 전파. 플래그 4가 없는 이월 기록(beta.16·표식 부팅이 쓴 기록)은 10 %·0.002 rad/s·30°(변이 M8이 살아남았던 경로를 막음). 표식이 있으면 플래그 4 기록은 형식 오류로 거부합니다(beta.16이 플래그 > 3을 형식 오류로 읽는 것과 같음). 그래서 2 %로 자란 기록에 30° 규칙을 적용하는 빌드는 없습니다. 2 % 자체는 표본 1개(이마트 사슬, 잔여 15~20° / |회전| 1,392°)에 근거하며, 이것이 기록을 과신하게 만들 수 있다는 검토의 우려는 그대로 남습니다. 끝 조건(신뢰 4·충돌 8·첫 정책 M 진행방향·60 s/900 s)이 손해를 묶습니다. 부수 효과(첫 판과 같음): garage 켠 부팅에서는 저장할 방위의 불확실도가 덜 자라므로 요로 이어 온 참조 방위가 BETA 자체 방위(10 %로 자람)보다 더 오래 유지됩니다. 시험: `garage_stored_uncertainty`(저장 불확실도 2 %·0.0023 대 10 %·0.002), `garage_codec_restore`(플래그 없는 이월 기록의 30°·10 %, 표식의 형식 거부).

### 직전 제품 부팅 토큰 (P1)

모든 BETA 워커 시작이 기록을 읽기 **전에** `logs/g3-last-start`(`mx5g3start 1 <boot_id>`)를 읽어 직전 제품 부팅 id로 쓰고, 현재 boot_id로 바꿔 씁니다(임시 파일 O_EXCL, write, close, rename, fsync 없음; 쓰기를 잃으면 다음 부팅이 이 부팅의 기록을 거부하므로 안전 쪽). `chain_depart`는 저장 기록의 boot_id가 그 직전 제품 부팅 id와 같을 때만 열리고, 아니면 `CARRIED_UNVERIFIED`(`not_prev_boot`, 창 없음)입니다. 플래그 4 기록은 벽시계 나이를 알아도 이 조건을 거칩니다. g3-off(메뉴 8)나 g3-garage-off(메뉴 12) 표식이 있는 부팅도 토큰을 바꿉니다(그 부팅은 출발을 표시하지 않으므로, 바꾸지 않으면 그 부팅 뒤 낡은 S가 이월됨). 이것이 "모든 변경은 표식 뒤" 원칙의 유일한 예외입니다: 시작 시 작은 파일 하나를 쓸 뿐 저널 행·송신·저장 기록은 바뀌지 않고(재생 바이트 동일, 아래), 표식 부팅에서 이 파일을 읽는 코드는 없습니다. 잡는 경우: 제품이 시작했지만 G3가 돌지 않았거나 표시하지 못한 부팅(저널 조기 실패, 워커가 G3 틱 전 종료, 센서 없음, 표식 부팅), 같은 부팅의 두 번째 워커 시작(직전 = 자기 자신 → 거부). 못 잡는 경우: 제품이 아예 시작하지 않은 주행(가드가 거부하면 연쇄 토큰이 바뀌지만, 가드가 돌지 않은 시동이나 BETA가 아닌 모드), 턴테이블·기계식 주차(바퀴가 돌지 않음), 표시 쓰기가 전원 차단으로 사라진 경우. 효과의 대가: 기록을 쓴 부팅과 다음 주행 사이에 다른 제품 부팅(예: #12처럼 시동만 잠깐 켠 부팅)이 있으면 이월하지 않습니다.

분석기: `G3 garage S records left without a departure mark: n` — garage S 기록(hv 1, 이번 부팅에 쓴 것 또는 시작 때 복원한 것) 뒤 같은 부팅의 G3 행이 그 기록보다 0.5 m 넘게 더 간 바퀴 거리(`m_b`)를 보고하는데 M 기록이 없으면 셉니다. 그 뒤 G3 행이 하나도 없이 끝난 주행은 보이지 않습니다.

### P2 정량 (비공개 trace, 좌표 없음)

방법: 원시 바퀴 이벤트(10 Hz)의 정확히 0인 연속 구간(250 ms 넘는 공백이면 끊김) 5 s 이상을 정차로 보고, 그 정차의 S 요청 시각(+5 s)과 출발 시각에서 idle 규칙의 기록 가능한 원인(RAW 창 드레인 `raw_window` 행 시각부터 rows/150 s, `beta_journal_lag` lagging~current)을 대조했습니다. `writer_busy`(>300 ms)는 trace에 틱 단위 값이 없어 볼 수 없습니다.

| 구간 | 원시 바퀴 덮개 | 정차 ≥ 5 s | 필요한 출발 표시 | 막힌 표시 |
|---|---|---|---|---|
| #13 이마트 하강(GPS 상실 1005.7 s → 센서 끊김 1098.9 s) | 90/93 s | 마지막 주차 1091.3~1098.9 s 하나(S 요청 1096.3 s, 막힘 없음) | 0(출발 없이 시동 끔) | 0 |
| #14 이마트 출발·상승(부팅 → 첫 FIX 184.9 s) | 133/143 s | 42.2~100.5 s 하나 | 1(복원된 S에서 100.6 s 출발) | 0(드레인 85.5~87.2 s는 끝남, 지연 0 s) |
| #14 집 하강(GPS 상실 908.8 s → 마지막 송신) | 66/66 s | 마지막 주차 965.2 s~ 하나(S 요청 970.2 s, 막힘 없음) | 0 | 0 |

결론: 두 하강 모두 중간 정차가 없어 출발 표시가 필요한 순간은 #14 시작의 한 번뿐이었고 막히지 않았습니다(0/1). 표본이 작아 "드물다"는 주장은 약하지만, 제품 변경(드레인 중 표시 허용)을 정당화할 자료도 없으므로 바꾸지 않았습니다. 다음 주행의 분석기 계수(위 P1)가 실제 빈도를 보여 줍니다.

### P3 교체 중 저장 I/O의 경계

- 교체 중(BETA GPS_LOST/ENGAGED/NO_FIX/SPEED_ENGAGED) S 요청은 직전 S 요청에서 30 s 뒤에만(`G3_GARAGE_REPLACING_S_GAP_NS`, `episode_block` "replacing_rate"). 규칙 F 오염 기록의 재기록은 예외(안전 쓰기).
- 교체 중 출발 표시는 저널 기록 스레드의 가장 오래된 미기록 행 지연이 100 ms 이하일 때만(`G3_GARAGE_REPLACING_MARK_LAG_NS`; 저널 지연 < 500 ms를 포함). 평소 idle 규칙(300 ms, 드레인, 지연 가드)은 그대로.
- 출발 표시에는 30 s 간격을 두지 않았습니다. 리드의 문구("교체 중 garage 쓰기 30 s에 1회")를 그대로 적용하면, 차단기 S 뒤 30 s 안에 출발해 5 s 미만 정차 뒤 시동을 끄는 경우 차단기의 S가 남아 다음 시동이 낡은 방위를 이월합니다. 표시는 S를 무효로 만드는 안전 쓰기이므로 S 하나에 표시 하나만 허용하고(S 요청 간격 30 s가 둘 다를 묶음) 교체 중 최대 30 s에 S 하나와 표시 하나입니다. 마지막 주차의 S는 직전 S 요청에서 30 s가 지나지 않았으면 기다리고, 그 사이 시동을 끄면 디스크에는 표시(M)가 남아 이월하지 않습니다(안전 쪽, 효과 없음). **정정(세 번째 수정, 아래)**: 이 문장은 표시가 실제로 기록된 경우에만 맞습니다. 표시가 들어가지 못했으면 디스크에는 앞의 S가 남아 있었고 30 s 간격이 새 S를 막아 그 S가 이월되었습니다. 이제 디스크가 방위 있는 S이면 30 s 간격을 적용하지 않습니다.
- 노출(2026-10-17 내보내기 14개 부팅, `yaw_stop` 정차 ≥ 5 s와 BETA 상태 시간표, 마지막 주차 중 `yaw_stop` 행이 없는 #13 끝은 따로 더함): 교체 중 시간 1,095 s 동안 교체 중 S 쓰기 후보 6, 교체 중 표시 3, 30 s 간격에 걸린 S 0. 이번 자료에는 터널 정체가 없어 경계가 실제로 걸린 적은 없습니다. 쓰기 지연: 이번 내보내기의 fsync 없는 쓰기 3.5~67.7 ms(기존 기록).
- 분석기: `G3 garage writes by BETA state: <상태> n (max lat_ms, > 100 ms k)`와 garage 줄의 자체 `max lat_ms`.
- 사전 등록 규칙(lat_ms > 500 또는 저널 지연 겹침 → 다음 빌드 기본 끔)은 사후 검출입니다. 첫 발견은 소유자가 터널에서 겪은 뒤일 수 있습니다.

### P4 사전 등록 규칙과 결과

사전 등록(통계와 결과 계산 전, 2026-10-18 00:51 KST): garage 사슬 시간 항 = 비공개 trace의 유효 `yaw_stop` 행(지속 ≥ 5 s, 4095 표지 없음)의 정차 평균 − 2048(G3 적분 영점)의 RMS × 658.615 µrad/s, 0.0001 rad/s 단위 올림, BETA의 0.002 rad/s 아래로는 내리지 않음. 이마트 출구 모델 결과는 어떻게 나오든 보고하고, 창이 출구 전에 닫히면 "유일한 실제 출구에 효과 없음"이라고 적음.

결과: 정차 93개(2026-10-10·11·17), 평균 −1.28카운트, sd 3.16, **RMS 3.40카운트 = 0.00224 rad/s** → 0.0023 rad/s(0.132 °/s, BETA보다 15 % 큼); |편차| > 3.04카운트(BETA 시간 항에 해당) 비율 29 %. 영점이 2048보다 평균 약 1.3카운트 낮다는 계통 편차는 이 RMS에 들어 있습니다.

이마트 출구(혼합 재구성 모델, INFERENCE, 영점 2048, 참조 7.9°): 저장 불확실도 34.8° → 36.5°, 출구의 세 번째 이후 FIX 송신 시각 불확실도 56.6/56.9/57.1/57.3° → **59.7/60.0/60.2/60.4°**. 60°를 넘는 순간(187.9~188.9 s 사이) `unc_grew`로 창이 닫히므로 바꾼 송신은 많아야 첫 한두 번(오차 +14.3, +19.1°)이고, 그것들도 정확도 198 m의 mode 2 위치라 네이버가 무시했을 것입니다. 즉 **사전 등록한 시간 항으로는 유일한 실제 지하 출구에서 효과가 없습니다**(안전 쪽). 효과가 남는 것은 회전·이동 시간이 작은 주차장입니다(예: 하강·상승 각 30 s, 각 180°: 8 + 0.132 × 60 + 0.02 × 360 ≈ 23°).

### P5·P7

- 집 나선 주차장(상승 |회전| 약 1,190°, 이동 약 165 s)은 저장 불확실도에 하강분이 더해지므로 출구 전에 60°를 넘어 창이 닫힐 가능성이 큽니다. **이 빌드로도 집 출구의 방위는 바뀌지 않을 수 있습니다**(소유자의 가장 큰 문제인 집 출구에는 효과가 없을 수 있음).
- 턴테이블·기계식 주차: 바퀴가 돌지 않고 위치도 같으므로 표시도 위치 검사도 차체 회전을 잡지 못합니다. 그런 주차 뒤에는 최대 180° 틀린 방위가 끝 조건까지 나갑니다.
- 관측되지 않은 주행: P1 토큰이 "제품이 시작했지만 표시하지 않은 부팅"을 잡지만, 제품이 시작하지 않은 시동이나 BETA가 아닌 모드의 시동, trace가 없는 이동(#11 끝 → #13 시작 같은 경우의 원인이 제품 미시작이면)은 잡지 못합니다.

### 시험 (2026-10-18, 스크래치 사본)

- 호스트 `make test`(D-Bus 헤더·라이브러리를 스크래치 경로로, `MX5DR_STOCK_ROOT` 읽기 전용): 조용한 호스트에서 종료 0. unittest 합계 1,141건(건너뜀 17), C++ 시험 모두 통과(`test_g3` 1,310 검사 — 그 뒤 토큰 형식 검사 4개를 더해 단독 1,314 검사 통과, navigation 3,977, core 1,872, `test_worker_beta` 시나리오 전부, `test_model_session_input`의 새 배제 사례 4개 포함). 첫 실행(ARM 빌드와 재생이 동시에 돌던 때, 부하 최고 약 7)은 `test_worker_beta g3_cancel`에서 `!lagging && max_summary_gap<1.6 s` 단언이 한 번 실패했고, 변이 실행 중 부하 아래에서 `g3_garage`에도 같은 단언 실패가 한 번 있었습니다. 조용한 호스트에서 새 코드와 HEAD(c1f00f4)를 각각 5회씩 돌린 `g3_garage`·`g3_cancel`은 모두 0/5 실패였습니다. 원인은 확정하지 못했습니다(이 시나리오들은 표식 부팅이라 바뀐 것은 시작 시 토큰 쓰기와 파이프라인 계수뿐; 저널 지연 단언은 호스트 CPU 몫에 민감함). 부하에 따른 흔들림으로 보이나 증명은 아닙니다.
- 새·바뀐 시험: `test_g3`(`garage_codec_restore`의 직전 부팅·표식 형식 거부·플래그 없는 기록의 10 %/30°·시간 항, `garage_stored_uncertainty`, `garage_store`의 30 s 간격·표시 지연 100 ms·`wait_s`, `garage_send`의 래치 규칙·`wait` 행·상한, `garage_latch_pipeline`의 실제 Pipeline 계수 6종과 끝-끝 시나리오 3개(세션 전 R 배제, 정차 중 재설정이 대기열의 R을 버림, 유효 전진 래치가 재설정으로 지워짐), `last_start_token`), `test_worker_beta`(g3_garage: 시작 때 읽은 토큰과 남긴 토큰, g3_park: g3-garage-off 표식에서도 토큰 교체, g3_send_off: g3-off 표식에서도 토큰 교체), `test_model_session_input`(런타임 배제 지점의 계수 4사례), `test_replay_g3_garage.py`(gap 실행: 중간 부팅 → `not_prev_boot`, 토큰 값; `or True` 삭제), `test_analyze_g3.py`(garage 최대 지연 분리, BETA 상태별, 표시 없이 떠난 S, `wait` 행, `latch`/`session` 끝 사유 — 분석기는 이전에 이 두 끝 사유를 형식 오류로 표시했음, 기존 결함), `log_rate`(garage 실행에 직전 부팅 전달).
- 정확한 ARM: 고정 GCC 4.9.1 `tools/build_arm.py` 검증 빌드 통과, `tests/run_arm_all.sh`(qemu-arm, nice 10) PASS 229, FAIL 0, SKIP 2(비공개 LDS·AA 순정 필요), **`ARM_TIMING_RETRIES=0`**, `ARM_TIMING_FAILED=0`.
- 실행하지 않음: 차량, 폰, DHU, AA 설치 탐침, 순정 BusyBox 하네스, 이 수정의 독립 재검토.

### 손 변이

29개, 각각 단독 적용, **모두 잡힘**(실행기는 패턴이 정확히 한 번 있는지 확인하고, 빌드 실패는 잡힘으로 세지 않음). 처음 실행에서 둘은 잡히지 않아 시험을 고쳤습니다: P1c(토큰 판독기가 boot id 형식을 검사하지 않음)는 길이가 맞는 잘못된 id 시험이 없어 살아남았고 → 시험 추가 뒤 잡힘; P1d는 처음에 다른 단언(저널 지연)으로 실패해 다시 돌려 의도한 단언(`prev_row`)으로 잡히는 것을 확인했습니다; C1i는 패턴이 두 번 있어 패턴을 고쳐 다시 돌림.

| id | 변이 | 잡은 시험 |
|---|---|---|
| C1a | IDLE이 잃어버린 후진 정보를 무시 | test_g3 |
| C1b | ACTIVE가 정보를 잃은 무효 래치로 끝나지 않음 | test_g3 |
| C1c | 20 m 문턱 제거 | test_g3 |
| C1d | 배제된 R을 세지 않음 | test_g3 |
| C1e | 배제된 전진 값도 셈 | test_g3 |
| C1f | 지워진 유효 래치를 세지 않음 | test_g3 |
| C1g | 재설정이 버린 대기열 R을 세지 않음 | test_g3 |
| C1h | 입력 공백을 세지 않음 | test_g3 |
| C1i | 런타임 배제 지점이 계수 없는 `exclude_reverse()` 호출 | test_model_session_input |
| C1j | 거부된 R을 세지 않음 | test_g3 |
| C2a | 이월 전파 회전 항을 스위치 기준으로(검토의 M8) | test_g3 |
| C2b | 저장 불확실도를 BETA 10 %로 | test_g3 |
| C2c | 표식이 플래그 4 기록을 받아들임 | test_g3 |
| C2d | 플래그 없는 이월 기록에 60° 한계 | test_g3 |
| C3a | garage `wait_s`를 15 s 기준으로 | test_g3 |
| P1a | 직전 부팅 검사 생략 | test_g3 |
| P1b | 벽시계 나이를 알면 직전 부팅 검사 생략 | test_g3 |
| P1c | 토큰 판독기가 boot id 형식 미검사 | test_g3(추가 시험) |
| P1d | 런타임이 직전 부팅을 넘기지 않음 | test_worker_beta g3_garage |
| P1e | 런타임이 garage 켬일 때만 토큰 교체 | test_worker_beta g3_park |
| P1f | 재생기가 토큰을 바꾸지 않음 | test_replay_g3_garage |
| P1g | 분석기가 복원된 S를 표시 계수에서 뺌 | test_analyze_g3 |
| P3a | 교체 중 S 간격 30 s 제거 | test_g3 |
| P3b | S 간격을 표시를 포함한 마지막 요청부터 셈 | test_g3 |
| P3c | 교체 중 표시가 기록 스레드 지연 무시 | test_g3 |
| P3d | 지연 조건을 교체 중이 아닐 때도 적용 | test_g3 |
| P3e | 분석기가 garage 지연을 저장 규칙 최대값에 섞음 | test_analyze_g3 |
| P4a | garage 시간 항 0.002로 | test_g3 |
| P4b | 이월 플래그 4 전파에서 garage 시간 항 제외 | test_g3 |

### 비공개 재생

20개 부팅(재생 가능 14, 위치 행 없음 6 — 이 6개는 `replay_beta: no position rows for this boot`로 모든 변형에서 같은 종료 2), 하나의 G3 저장 디렉터리로 연쇄(`--real-only --g3 on --g3-send on`), report/csv/journal/g3 행/send 기록/stdout/stderr 110개 파일, 저장 스레드 시각 필드(`lat_ms`, `start_ns`, `end_ns`)만 정규화:
- 새 코드 `--g3-garage off` 대 HEAD~1(beta.16, 표식 개념 이전): **110/110 동일**.
- 새 코드 `--g3-garage off` 대 HEAD(c1f00f4) `--g3-garage off`: **110/110 동일**.
- 새 코드 `--g3-garage on`: 바꾼 송신 0개(persistent 재생은 원시 창 밖에서 `wheel_stale`로 창이 닫힘 — 효과의 근거가 아님). HEAD on과의 차이는 g3 행(직전 부팅 `prev` 필드, `wait` 행)과 da8288d5의 send 기록 주석뿐. 복원: HEAD on은 `chain_depart` 3회, 새 코드는 2회와 `not_prev_boot` 1회 — da8288d5(#14)가 읽은 기록은 재생에서 #13이 원시 창 밖 이동을 표시하지 못해 남은 더 이전 부팅(41945267)의 S였고, 직전 제품 부팅(3121d513)이 아니어서 거부되었습니다. P1이 겨냥한 상황("표시하지 못한 부팅 뒤의 낡은 S")이 재생 자료에서 실제로 한 번 나타난 것입니다(재생 산물이며 차량 사실 아님: 차량에서는 #13이 G3를 돌렸음).

### 남은 위험

- 실차·폰·DHU 미검증. 네이버가 출구의 정지·저속 방위를 어떻게 쓰는지 모릅니다.
- 2 %와 0.0023 rad/s는 각각 표본 1개(이마트 사슬)와 정차 영점 93개에 근거합니다. 이동 중 영점이 정차 영점과 같은 분포인지 모릅니다. "60° 안"이라고 보낸 방위가 실제로는 더 틀릴 수 있습니다(끝 조건이 손해를 묶음).
- 후진: 워커가 첫 움직임 메시지를 받기 전부터 R이었고 20 m 넘게 후진하는 경우(위).
- 낡은 S: 제품이 시작하지 않은 주행, 턴테이블, 출발 표시 직후 전원 차단(fsync 없음). 그 경우 출구에서 그 주행의 회전만큼 틀린 방위가 신뢰 4회·충돌 8회(게이트를 지난 FIX 기준 약 8 s)·첫 정책 M 진행방향·60 s/900 s까지 나갑니다. 순정이 맞았을 지상 출발에서도 생길 수 있습니다.
- 교체 중 저장 I/O(G1 터널 경로): 경계를 두었지만 쓰기가 기록 스레드를 막는지는 측정되지 않았고, 사전 등록 규칙은 사후 검출입니다.
- 이 수정 자체의 독립 재검토는 하지 않았습니다.

### 다음 주행에서 보여야 할 것 (메뉴 3 내보내기, `analyze_logs.py`)

1. 지하 마지막 주차: `g3_store written "st":"S" … "garage":1,"sync":0`가 정차 5 s 뒤에(교체 중이면 직전 S 요청에서 30 s 뒤에), 출발마다 `"st":"M","mark":1`. 분석기 `S records left without a departure mark: 0`.
2. 다음 시동: `g3_store loaded … "reason":"chain_depart" … "prev":"<직전 부팅 8자리>"`(다른 값이면 `not_prev_boot`), `g3_send wait` 행의 사유(`latch_unknown`이 20 m 뒤 풀리는지, `latch_unsure`가 나오는지와 `rlost`), `g3_send start "garage":1,"moved":1,"rlost":0`, 상승 중 `count` 행의 `unc`, 출구에서 `choice 6` 송신과 `end`(trusted/m_course 또는 rejected/unc_grew)와 `g3_first course`의 `err_p`.
3. 분석기 `G3 garage writes without fsync` 줄(lat_ms > 500 0, journal lag 겹침 0; 아니면 다음 빌드 garage 기본 끔)과 `G3 garage writes by BETA state` 줄(교체 중 > 100 ms 개수).
4. 소유자 관찰: 지하 출구에서 화살표가 차와 맞는지, 몇 초 안에 도로와 맞는지. 집 출구는 이 빌드로도 바뀌지 않을 수 있습니다(`rejected/unc_grew`가 예상).

## 세 번째 수정 (2026-10-18, 2차 검토 반영)

검토: `validation/G3_GARAGE_EXIT_FIX_REVIEW2_FABLE_2026-10-18.md`(C1~C4 CONFIRMED). 이 절이 위의 "독립 검토 반영" 절과 그 위의 표·문구보다 우선합니다. 차량·폰·DHU에서는 실행하지 않았습니다. 바뀐 제품 동작은 아래 C1 하나뿐이며 `g3-garage-off` 표식 뒤에 있습니다(표식이 있으면 `garage_`가 거짓이라 이 조건은 평가되지 않음).

### C1. 교체 중 30 s S 간격의 예외: 디스크가 방위 있는 S일 때 (고침)

- 결함(FACT, 2차 검토의 탐침을 그대로 시험으로 옮겨 재현): 교체 중 1 s 출발, 2~6 s 정차 → S1(6 s 요청). 7~13 s 이동은 idle 규칙(`journal_draining`), 14~22 s는 기록 스레드 지연 150 ms로 출발 표시가 막힘. 18 s부터 마지막 정차, 30 s에 시동 끔. `c75053c`: 디스크 = S1(플래그 4, S, 방위 있음) → 다음 시동이 `chain_depart`로 차단기 등 앞 정차의 방위를 이월. 원인: 마지막 주차의 S2가 직전 S 요청(6 s)에서 30 s가 지나지 않아 `replacing_rate`로 막혔고, 표시는 정차 5 s가 지나면 더 요청되지 않습니다.
- 고침(검토의 완화안 (i), 가장 작은 변경): 교체 중 S 요청의 30 s 간격은 **디스크 기록이 방위 있는 S이면 적용하지 않습니다**(`disk_=='S' && (disk_flags_&G3_FLAG_HEADING)`). 그 상태에서 새 정차의 S 요청이 생겼다는 것은 차가 그 S를 떠났는데 표시가 들어가지 못했다는 뜻이고, 새 S는 이월될 낡은 S를 바로잡는 안전 쓰기입니다. 교체 중 쓰기 수는 "표시가 들어가지 못한 출발마다 1회 더"로 묶입니다(5 s 비율 제한과 idle 규칙은 그대로). 그 밖의 경우(디스크가 M, 방위 없는 S, 없음)는 30 s 간격 그대로이고, 규칙 F 오염 재기록의 예외도 그대로 둡니다. 같은 예외가 2차 검토 P3의 변형(교체 중이 아닐 때의 60 s fsync 재기록이 `last_s_request_ns_`를 갱신하고 곧 GPS를 잃는 경우)도 덮습니다.
- 30 s 간격은 직전 S 요청(취소·실패한 요청과 60 s fsync 재기록 포함)부터 셉니다. 출발 표시는 이동 중과 정차 첫 5 s 안(정차가 "주차"가 되기 전)에만 요청되고, 그 뒤에는 주차 S만 요청됩니다.
- 위 P3 절의 "그 사이 시동을 끄면 디스크에는 표시(M)가 남아"와 `g3_heading.h`의 옛 주석 "A blocked final park leaves the mark on disk"는 **표시가 들어간 경우에만** 맞았습니다. 주석과 위 문장을 고쳤습니다.
- 시험(`test_g3 garage_store`, 모두 `c75053c` 제품 코드에서 실패하는 것을 확인): (1) 탐침 시나리오 — 23 s에 S2(seq 2, `mono_ms` 23000)가 써지고 30 s까지 그대로; (2) 60 s fsync 재기록 뒤 교체 중 표시가 막히고 15 s 뒤 마지막 주차 → 76 s에 S; (3) 표시가 들어간 경우(디스크 M)는 30 s 간격이 그대로 적용됨(기존 사례, 35 s까지 대기 → 36 s); (4) 방위 없는 S가 디스크에 있으면 30 s 간격 그대로(표시도 요청되지 않음); (5) 교체 중 규칙 F 오염 재기록이 30 s 안에도 나감(M3).
- 분석기: `G3 garage S left unmarked after replacing_rate held the next park: n` — 세션 안에서 garage S(방위 있음, 이번 부팅에 쓴 것 또는 시작 때 복원한 것) 뒤 M 없이 `g3_since unstored`의 사유가 `replacing_rate`이면 셉니다. 이 빌드에서는 그런 경우가 생길 수 없으므로 0이 아니면 결함입니다(`c75053c` 로그에서는 낡은 S의 신호). 시동을 꺼서 끝난 마지막 주차는 `g3_since` 행이 없어 보이지 않습니다.

### C2. 시간 항 0.0023 rad/s의 도출 (기록 보충, 값은 바꾸지 않음)

규칙(사전 등록 그대로): `yaw_stop` 행 중 `dur_ms ≥ 5000`, `yn > 0`, 4095 표지 제외(`|ys/yn| < 50` 카운트; 해당 행은 평균 약 1535 카운트 1개), 값 = `ys/yn`(정차 평균 − 2048, G3 적분 영점), RMS × 658.615 µrad/s, 0.0001 rad/s 단위 올림.

| 자료와 행 선택 | n | 평균 | sd | RMS(카운트) | rad/s | 올림 |
|---|---|---|---|---|---|---|
| **저자(사전 등록 때 쓴 것): 2026-10-17 USB 내보내기 하나**(`trace.2/1/0.jsonl`; 링이 2026-10-11 주행 부팅들까지 보존, 2026-10-10 부팅은 이미 밀려남; 9개 부팅) | 93 | −1.28 | 3.15 | 3.396 | 0.002237 | **0.0023** |
| 2차 검토: 비공개 내보내기 8개 전체(2026-10-10 2개, 2026-10-11 4개, 2026-10-17 2개 사본)의 합집합, 같은 부팅의 **같은 행 텍스트**는 한 번만(2026-10-17 내보내기에 없는 2026-10-10 부팅 하나의 17행과 beta.7 부팅의 1행이 더해짐) | 111 | −1.31 | 3.02 | 3.287 | 0.002165 | 0.0022 |
| 같은 합집합, `pre`가 있는 행만 | 90 | −1.16 | 2.95 | 3.171 | 0.002089 | 0.0021 |
| 같은 합집합, `first`·`last`가 있는 행만 | 109 | −1.31 | 3.04 | 3.315 | 0.002183 | 0.0022 |

- 주의: (부팅, `mono_ns`)로 중복을 지우면 108개가 됩니다. 서로 다른 정차 세 쌍이 같은 시각에 함께 기록되기 때문이며(한 번의 flush에 닫힌 정차와 열린 정차), 행 텍스트 기준이 맞습니다.
- 사전 등록 문구는 "비공개 trace"라고만 적어 어느 내보내기인지 정하지 않았습니다. 저자가 쓴 것은 2026-10-17 내보내기 하나였고, 검토의 합집합 규칙이면 0.0022가 나옵니다. **0.0023을 유지합니다**: 네 변형 가운데 가장 큰(보수적인) 값이며, 효과를 위해 줄이지 않습니다.
- 이마트 출구(혼합 재구성 모델, 영점 2048, INFERENCE)를 0.0022로 다시 계산했습니다: 저장 불확실도 35.9°(0.0023: 36.5°), 출구의 바꾼 송신 시각 불확실도 58.7/59.0/59.2/59.4°(0.0023: 59.7/60.0/60.2/60.4°). **2차 검토의 "0.0022에서도 결론이 같다"는 세부에서 맞지 않습니다**: 0.0022에서는 이 네 송신 동안 60°를 넘지 않아 창이 `unc_grew`로 닫히지 않고, 모델상 네 송신(오차 +14.3/+19.1/+19.3/+16.1°) 뒤 신뢰 조건에서 끝납니다. 차이는 약 1°이지만 60° 경계 바로 위·아래라 창의 끝 사유가 바뀝니다. 그 네 송신은 모두 정확도 198 m의 mode 2 위치라 네이버가 무시했을 것이라는 앞의 추론은 같으므로 "유일한 실제 출구에서 측정할 수 있는 효과는 없음"이라는 결론은 두 값에서 같습니다. 0.0023에서는 창이 첫 송신 뒤 닫히므로 더 보수적입니다.

### C3. 시험

- `test_beta reverse_unsure_is_observation_only`: 규칙 F 시나리오(`assume_forward_seeds_without_reverse_message`와 같은 흐름) 앞에 배제된 REVERSE 40개를 넣어, 전진 값(계수 0)과 후진 값(계수 40)의 두 실행에서 100 ms마다 `status()`, BETA 게이트·시드·규칙 F 횟수·래치, 대조 횟수, BETA 게시(위치·속도·방위·정확도), MODEL 진단과 앵커 기록 전부가 같음을 단언합니다(규칙 F가 실제로 가정했고 게시가 있었음도 단언). 변이 M4(규칙 F 조건에 `g3_.reverse_unsure||`)는 이 시험 단독으로도 잡힙니다(실행 2600 ms에서 게이트 6 대 9). 참고: 기존 `test_beta`의 `reverse_latch_cleared_in_reverse_blocks_rule_f`도 M4를 잡습니다(2040행); 2차 검토는 `test_beta`를 돌리지 않았습니다(test_g3·test_navigation·test_model_session_input·재생만).
- `test_g3 garage_store`의 규칙 F 오염 재기록 사례(M3): 교체 중 S(방위 100°) 뒤 같은 정차에서 discard → 5 s 뒤(30 s 안) 방위 없는 S로 재기록. **세 번째 수정 뒤 M3(`!stale` 제거)는 동치 변이입니다**: 오염 재기록 대상은 언제나 이번 부팅에 쓴 방위 있는 S이므로(`stale`은 `disk_flags_`의 방위 비트를 요구하고, 그 비트는 S 기록에서만 섭니다) 새 디스크-S 예외가 같은 경우를 덮습니다. `!stale`은 명시적 방어로 남겼고, 시험은 두 예외를 함께 지우는 변이를 잡습니다(아래 변이 표).
- `test_g3 garage_codec_restore`: 직전 부팅 토큰이 8번째 글자 뒤에서만 다른 경우와 앞 8자 안에서 다른 경우 모두 `not_prev_boot`(M2: 앞 8자만 비교하는 변이를 test_g3가 잡음; 2차 검토에서는 재생 시험만 잡았음).
- `tools/replay_beta.cpp --writer-lag-ms N`(기본 0 = 이전과 같은 출력): G3 표본의 `writer_lag_ns`와 런타임 idle 규칙(`g3_io_block_reason`, 300 ms 넘으면 `writer_busy`)에 상수 지연을 넣습니다. `test_replay_g3_garage.py`의 lag 실행(150 ms): 부팅 A의 마지막 garage 주차 S는 그대로 저장되고, 부팅 B의 출발 표시는 BETA가 교체하는 동안(지하 NO_FIX/SPEED_ENGAGED) 기다렸다가 출구에서 교체가 끝난 뒤(130 s 이후) 들어가며(지연 0이면 40 s), 송신은 지연 0 실행과 같습니다.
- `test_worker_beta g3_cancel`·`g3_garage`의 시간 단언(`!lagging && max_summary_gap < 1.6 s`, 문턱 그대로): 두 시나리오에서 이 단언만 실패하면 부하(`/proc/loadavg`)와 값을 출력하고, 시나리오의 다른 모든 단언과 정리가 끝난 뒤 종료 코드 77로 끝냅니다. `tests/runtime/run_worker_beta_timing.sh`(Makefile의 host 실행)가 77일 때만 최대 2회 더(5 s 간격) 돌리고 매 재시도를 부하와 함께 `WORKER_BETA_TIMING_RETRY`로 남기며, 세 번 모두 77이면 `WORKER_BETA_TIMING_FAILED`로 실패합니다. 다른 실패(논리 단언, 134)는 재시도하지 않습니다. ARM 스위트는 이미 `tests/run_arm_timing.sh`로 이 시나리오들을 돌리며 77을 실패한 시도로 셉니다(그대로).

### C4. 문구

위 본문과 AGENTS 문안을 고쳤습니다: 후진 잔여는 "제품 시작 전"이 아니라 "워커가 첫 움직임 메시지를 받기 전"부터 R인 경우; 20 m는 "시동 뒤"가 아니라 승인 뒤 첫 바퀴 움직임부터 센 바퀴 거리(`G3Motion::distance_m`); 출발 표시는 이동 중과 정차 첫 5 s 안에만; 30 s 간격의 기준(직전 S 요청: 취소·실패·60 s fsync 재기록 포함)과 두 예외(규칙 F 오염 재기록, 디스크가 방위 있는 S); 낡은 S 목록에 "같은 부팅의 두 번째 워커 시작은 직전 부팅의 기록을 거부함(`not_prev_boot`, 효과 손실)"과 "워커가 토큰 교체에 이르기 전에 끝난 부팅(stop 요청, locale 실패, 워커 스레드 없음)은 못 잡음"; 세지 않는 후진 정보 손실 두 경로(거부가 선행하지 않은 원천 epoch 변경, 워커가 붙기 전의 메시지); 0.0023의 도출(C2); 토큰 쓰기(`logs/g3-last-start`, temp+rename, fsync 없음)도 2026-10-11 "주차 쓰기만" 불변식의 예외이며 모든 BETA 워커 시작(표식 유무 무관, 같은 부팅 안의 주행 중 재시작 포함)에서 돕니다.

### 시험 결과 (2026-10-18, 스크래치 사본)

- 호스트 `make test`(D-Bus 헤더·라이브러리를 스크래치 경로로, `MX5DR_STOCK_ROOT` 읽기 전용, 1분 부하 0.3~1.3): **종료 0**. unittest 합계 1,144건(건너뜀 17: 고정 ARM 컴파일러·릴리즈 묶음·비공개 자료·선택 도구가 없어서, 이전과 같은 부류), `test_g3` 1,344 검사, `test_beta` 77,768 검사, navigation 3,977, core 1,872, `test_worker_beta` 시나리오 전부(g3_garage·g3_cancel은 새 감싸개로 실행, 재시도 0회), G3 송신 시나리오 5개와 `g3_send_identity.py`, 분석기·재생 시험(새 `test_garage_unmarked_after_replacing_rate`, `test_writer_lag_holds_the_mark_while_replacing` 포함).
- 비공개 재생(20개 부팅, 하나의 G3 저장 디렉터리로 연쇄, `--real-only --g3 on --g3-send on`, 저장 스레드 시각 필드만 정규화, 110개 파일): 새 코드 표식(`--g3-garage off`, `--writer-lag-ms` 기본 0) 대 beta.16 **110/110 동일**, 대 `c75053c` 표식 **110/110 동일**. 표식 없음(`--g3-garage on`)도 새 코드 대 `c75053c` 110/110 동일(persistent 재생은 원시 창 밖에서 `wheel_stale`이라 C1 경로에 닿지 않음; 효과의 근거가 아님).
- 손 변이(각각 단독, 별도 바이너리, 패턴이 정확히 한 번 있는지 확인; 빌드 실패는 잡힘으로 세지 않고 패턴을 고쳐 다시 돌림):

| id | 변이 | 결과 |
|---|---|---|
| C1a | 디스크-S 예외 제거(`c75053c` 동작) | 잡힘(test_g3 탐침 사례); 탐침을 뺀 사본에서는 fsync 재기록 변형 사례가 잡음 |
| C1b | 예외 조건에서 방위 비트 제거(방위 없는 S에도 예외) | 잡힘(방위 없는 S 사례) |
| C1c | 60 s fsync 재기록 뒤에는 예외 없음(`disk_synced_` 조건 추가) | 잡힘(fsync 재기록 변형 사례) |
| M3 | 규칙 F 오염 재기록 예외(`!stale`) 제거 | **생존: 동치 변이**(위 C3 설명). M3+C1a를 함께 적용하면 오염 재기록 사례가 잡음(그 사례만 남긴 사본에서 확인; 원 코드는 그 사본에서 통과) |
| M2 | 직전 부팅 비교를 앞 8자만 | 잡힘(test_g3, 8번째 뒤만 다른 토큰) |
| M4 | 규칙 F 조건이 `reverse_unsure`를 읽음 | 잡힘(test_beta; 새 시험 단독으로도 잡음, 기존 2040행도 잡음); test_g3는 통과 |
| A1 | 분석기: M 기록이 열린 S를 닫지 않음 | 잡힘(test_analyze_g3) |
| A2 | 분석기: 사유와 무관하게 셈 | 잡힘(test_analyze_g3) |
| R1 | 재생기가 `--writer-lag-ms`를 표본에 넣지 않음 | 잡힘(test_replay_g3_garage) |
| T1 | 시간 단언이 늘 실패(문턱 1 ns) | 감싸개가 77을 두 번 재시도하고 부하를 남긴 뒤 `WORKER_BETA_TIMING_FAILED`로 실패(종료 1); 결정적 실패는 여전히 실패 |

  감싸개 자체: 77 한 번 뒤 통과 → 재시도 1회 기록 후 종료 0; 77 세 번 → 실패; 다른 실패 코드(134) → 재시도 없이 그대로 종료.
- 실행하지 않음(지시대로): 정확한 ARM 스위트, 순정 BusyBox 포장 하네스, 차량·폰·DHU, 이 수정의 독립 재검토.

### 남은 위험 (바뀐 것만)

- 디스크-S 예외로 교체 중 쓰기가 "표시가 들어가지 못한 출발마다 1회" 늘 수 있습니다(터널 정체에서 표시가 계속 막히면 정차마다 S 한 번; 5 s 비율 제한과 idle 규칙 300 ms는 그대로). 쓰기가 저널 기록 스레드를 막는지는 여전히 측정되지 않았습니다.
- 표시도 S도 들어가지 못하는 경우(idle 규칙이 마지막 주차 내내 막음, 전원 차단)는 여전히 앞의 S가 남습니다(이전 기록과 같음).
- 0.0023은 정차 영점 자료로 정한 값이며 이동 중 영점의 분포는 모릅니다. 0.0022와의 차이로 이마트 모델의 창 끝 사유가 바뀌는 것처럼, 결과는 60° 경계에 민감합니다.
- 이 세 번째 수정은 독립 재검토를 받지 않았습니다.

## AGENTS.md-Text

리드가 AGENTS.md에 넣을 문단(이 작업에서는 AGENTS.md를 편집하지 않음, 1차 검토 (F) 1~7과 2차 검토 C4·(6) 1~6 반영):

"Explicit BETA exception extension (garage carry, validation/G3_GARAGE_EXIT_FIX_2026-10-17.md with the fixes for validation/G3_GARAGE_EXIT_FIX_REVIEW_FABLE_2026-10-18.md; default on; off with the g3-garage-off marker (menu 12), which restores the beta.16 rules, or with the g3-off marker (menu 8); menu 1 reinstall and menu 4 removal clear the marker): the BETA_BEARING_OVERLAY (choice 6) window of v1.0.0-beta.15 also opens for a heading restored CARRIED with reason chain_depart: a store record written by a garage-on build (flag 4), state S, heading present, stored uncertainty <= 60 deg, boot chain ok, written by another boot that is the previous product boot (logs/g3-last-start, swapped at every BETA worker start before the record is read, also with either marker; otherwise CARRIED_UNVERIFIED not_prev_boot, no window), no wall-clock age needed (the CMU realtime clock stays near 1970). State S means the car stood since the write only as far as the product observed it and only if the departure mark landed (a 110-byte write without fsync under the idle rule and the 5 s rate): a drive without any product start, a non-BETA mode, a boot whose worker did not reach the token swap (stop requested, locale failure, no worker thread), a turntable or mechanical parking system, or a power cut soon after the mark leaves a stale S (a second worker start in the same boot refuses the previous boot's record: not_prev_boot), and the next boot then sends a heading wrong by that drive's rotation (up to 180 deg) until an end condition, also where the stock bearing would have been right. The window may open after the car moved since boot (a garage exit) unless a yaw-chain break happened after the first wheel motion while not standing. Reverse latch: a valid latch saying forward is used; a latch that is not valid (no 0x118 change since the session/bus admission, a message before the admission excluded, or any pipeline reset or latch clear) counts as forward only while no 0x118 information was lost (G3Motion::reverse_unsure zero: no excluded value other than forward, no cleared valid latch, no rejected or reset-discarded REVERSE message, no motion input gap that clears the latch) and only after 20 m of wheel distance since the first wheel motion after the session/bus admission; otherwise the window waits, and an open window ends on a reverse latch or on a latch that became not valid after lost information. Uncounted reverse-information losses: a source-epoch change without a preceding rejection and 0x118 messages before the worker attached. Residual: a car in reverse since before the worker received its first motion message that reverses more than 20 m gets a bearing 180 deg wrong until the first 0x118 change or an end condition. The overlaid send, its bytes (40, 44..47 only), wrapper, return value, errno and every other end condition are those of beta.15. Uncertainty of the garage chain: the stored uncertainty of every record a garage-on boot writes and the propagation of a carried flag-4 record use a 2 % rotation term and a 0.0023 rad/s time term (BETA: 10 %, 0.002 rad/s; 0.0023 is the RMS of the 93 parked yaw zeros of the 2026-10-17 export around the integration zero, pre-registered; the union of all eight private exports gives 111 stops and 0.0022, and 0.0023 is kept as the larger value); the 60 deg limit applies only to flag-4 records; a carried record without flag 4 keeps the BETA terms and the 30 deg limit; with the marker a flag-4 record is rejected as a format error, as beta.16 decodes it. Store I/O: the parked S record needs 5 s of exact standstill and may be written without fsync while BETA replaces; while BETA replaces an S request needs 30 s since the previous S request (any S request, also a cancelled or failed one and the 60 s fsync rewrite), except the rule F taint rewrite and any S request while the record on disk is an S with heading (its departure mark did not land, so the new S replaces a record that would otherwise be carried stale; at most one extra write per such departure); the departure mark is written while the car moves or within the first 5 s of a stop (afterwards only the parked S record is requested), also while BETA replaces (then only with a journal writer lag <= 100 ms). These are the first store writes that run while BETA replaces and while the car moves: the 2026-10-11 H1 'parked write only' invariant no longer holds with the garage carry. The previous-product-boot token write (logs/g3-last-start, temp+rename without fsync) is a further exception to that invariant: it runs at every BETA worker start, with or without the markers, including a mid-drive worker restart in the same boot. No fsync is requested while BETA replaces and an in-flight fsync write is cancelled before its fsync/rename gate; an fsync already executing cannot be interrupted (unchanged residual). The pre-registered rule (a garage write without fsync with lat_ms > 500 or a journal-lag overlap makes the next build default the garage carry off) detects a problem only after a drive. Not vehicle-validated; with the 0.0023 rad/s term the digest model of the only real garage exit (E-Mart, 2026-10-17) closes the window at the exit (unc_grew; at 0.0022 rad/s it would stay below 60 deg for four overlaid sends of 198 m mode-2 fixes and end on trust), and the home spiral garage is expected not to benefit."

## 네 번째 수정 (2026-10-18): 직전 제품 부팅 토큰의 지연 교체

상태: 구현, 호스트 시험, 정확한 ARM 빌드·시험, 비공개 주행 기록의 오프라인 재생과 디지스트 수준의 반사실 판정만 했습니다. 차량·폰·DHU에서는 실행하지 않았습니다. 이 절이 위의 "직전 제품 부팅 토큰 (P1)" 절과 AGENTS 문안의 토큰 문장보다 우선합니다. 좌표·주소는 적지 않았습니다.

### 결함 (FACT, `G3_UNC_LIMIT_2026-10-18.md` 부수 발견 1)

`logs/g3-last-start`는 BETA 워커가 시작할 때마다 기록을 읽기 전에 이번 부팅 id로 바뀌었습니다. 시동만 잠깐 켠 부팅(움직임 없음, GPS 없음, 그래서 기록도 쓰지 않음)도 토큰을 바꾸므로, 그다음 부팅은 진짜 직전 주행의 S 기록을 `not_prev_boot`로 거부했습니다. 유효한 부팅 사이 사슬 6개 중 2개(집 나선 사슬, 0914e18f→5a9f9b2b)에 이런 부팅이 있었고 2026-10-17 내보내기의 #12 07ea46fa도 같은 종류입니다. 이 6개 정지 부팅은 모두 AA 연결이 없었습니다(`shadow_bus input_available:false`, 파이프라인 `events:0`). 따라서 파이프라인이 센 바퀴 거리만으로 "움직였는가/센서가 없는가"를 판단하면 바로 이 부팅들이 "센서 없음"으로 분류되어 결함이 고쳐지지 않습니다.

### 바뀐 동작 (garage carry 켬일 때만)

- 워커 시작: 토큰을 읽어 직전 제품 부팅(`prev`)으로 쓰는 것은 그대로이고, **이번 부팅 id는 아직 쓰지 않습니다**. 대신 작은 `logs/g3-worker-boot`(`mx5g3worker 1 <boot_id>`, temp+rename, fsync 없음, 0600)를 씁니다. 그 파일이 이미 이번 부팅을 가리키면(같은 부팅의 두 번째 워커 시작: 두 워커 사이 시간은 관측되지 않음) 시작 때 바로 토큰을 바꾸고 `prev`도 이번 부팅으로 두어 다른 부팅의 기록을 모두 거부합니다(`swap":"eager_restart"`). 그 파일을 쓰지 못하면 시작 때 바로 바꿉니다(`eager_unmarked`). `g3_store loaded/missing/rejected` 행에 `"swap":"lazy|eager_restart|eager_unmarked"`가 더해집니다(garage 켬일 때만).
- 토큰을 이번 부팅 id로 다시 쓰는 때(먼저 온 것 하나, `g3_store` `event:"token"` 행의 `reason`):
  - `moved`: 원시 바퀴 흐름의 평균 바퀴 거리 ≥ 2 m(`G3_MOVING_MARK_M`, `G3Motion::distance_m`과 같은 적분: 직전 속도 × min(간격, 0.5 s)). **원시 흐름**은 세션·버스 승인 전 배제된 바퀴 이벤트까지 모두 셉니다(`G3TokenWatch`, `drain_motion`에서 받은 모든 WHEELS). 파이프라인의 `distance_m` ≥ 2 m도 같은 이유로 셉니다.
  - `blind_gap`: 원시 바퀴 수신 간격 > 2 s(`navigation::G3_BOOT_GAP_MAX_NS`, "출발·회전·정지를 하기에 너무 짧음")가 **끝났을 때**(새 이벤트 도착). 2026-10-17 내보내기 14개 부팅의 최대 수신 간격은 1.03 s(디지스트 `max_receipt_gap_ms`)였습니다.
  - `blind_silent`: 워커가 도는 동안 원시 바퀴 이벤트가 30 s 동안 없음(처음부터 없거나 끊긴 채 돌아오지 않음). 30 s는 같은 14개 부팅에서 센서가 멈춘 뒤 저널이 끝날 때까지의 가장 긴 꼬리(9.6 s, 시동 끔)의 약 3배입니다. 액세서리 모드로 30 s 넘게 머무는 시동은 바꿉니다(비용만 듦, 보수 쪽).
  - `wheel_bad`: 해석할 수 없는 바퀴 값(> 40000 카운트, 음수 속도).
  - `record`: 이번 부팅의 **첫 S 기록 요청**. S 요청은 토큰 요청으로 바뀌고 S는 토큰이 들어간 뒤(5 s 간격) 요청됩니다. 그래서 이번 부팅이 쓴 S 기록은 언제나 토큰이 이번 부팅을 가리킨 뒤에 들어갑니다(다음 부팅이 비교하는 "기록의 boot id = 토큰"이 모든 순서에서 성립: 토큰만 들어감 → 이전 기록 거부; 토큰 뒤 S → 이월; S가 토큰보다 먼저 들어가는 길은 없음). 출발 표시 M은 토큰을 기다리지 않습니다(M은 이월되지 않음). 그래서 2 m 미만으로만 움직인 부팅은 M을 쓰고 토큰은 그대로 둡니다.
  - 동기 쓰기(워커, 한 번): 관측이 끝나는 때 — 저장 스레드 없음(`no_store_thread`), 저널 실패(`journal_failed`), 감사 결함(`audit_fault`), 캡처 정지(`capture_stop`, 메뉴 3). 별도 임시 이름(`.g3-last-start.sync.tmp`)으로 temp+rename, fsync 없음. 결과 `sync`/`sync_failed`.
- 저장 스레드 쓰기의 문(출발 표시와 같음): 저장 켜짐, 대기·진행 중 요청 없음, 5 s 요청 간격(S·M과 공유), idle 규칙(`journal_lagging`, `no_writer`, `journal_draining`, `writer_busy`), BETA 교체 중이면 저널 기록기 지연 ≤ 100 ms. fsync 없음, 워커를 막지 않음. 취소·늦은 수거는 5 s 뒤 재요청, 실패는 기록 실패와 함께 세어 3회면 그 부팅의 저장이 꺼집니다. 정적 내용이므로 바퀴 움직임·흐름 정지로는 취소되지 않고 idle 규칙으로는 취소됩니다. 우편함은 대상(`G3_TARGET_TOKEN`)을 따로 받으며, 대상을 줄 수 없는 호출자에게는 토큰 요청을 넘기지 않습니다(토큰 줄이 기록 파일에 써지는 일 없음).
- 표식(`g3-garage-off` 또는 `g3-off`)이 있거나 BETA 코어가 없으면 **HEAD와 똑같이 시작 때마다 바꿉니다**(같은 함수, 같은 임시 이름). 검토가 원한 "표식 부팅이 조용히 이월을 허락하지 않음"은 표식 부팅이 언제나(움직이든 아니든) 토큰을 바꾸는 것으로 지켜지고, 표식 동작은 파일까지 HEAD와 같습니다(재생 110/110 동일, 저장 디렉터리의 기록·토큰 해시 동일, `g3-worker-boot` 없음). 그 대가로 표식 부팅이 사이에 있으면 이월하지 않습니다(HEAD와 같음).
- 설치(메뉴 1 `install.sh`)와 제거(메뉴 4 `uninstall.sh`)가 `logs/g3-last-start`를 지웁니다(파일·심볼릭 링크만; 디렉터리는 두며 토큰으로 읽히지 않음). 이유(INFERENCE, 아래 반사실 3): 지연 교체에서는 시동만 켠 설치 부팅이 토큰을 바꾸지 않으므로, 제품이 없던 동안의 주행 뒤 설치 부팅을 거친 다음 부팅이 낡은 S를 이월할 수 있습니다. 2026-10-17 내보내기에서 #12 07ea46fa는 beta.16 설치 부팅으로 보이며(`installed.txt` 시각 152 s, 백업 디렉터리 133 s가 #12의 39~151 s 안), 그 앞뒤로 기록 없는 하강(관측 안 된 주행)이 있었습니다. HEAD에서는 그 설치 부팅의 시작 교체가 이것을 막았습니다.

### 시험 (각각 HEAD 코드에서는 실패: 새 API·행이 없어 빌드되지 않거나 단언이 실패)

- `test_g3` `token_codec`, `token_watch`, `token_chain`, `token_gate` (실제 파일, 실제 추적기·감시기, 저장 스레드 대신 대상별로 쓰는 동기 펌프):
  - 실제 시나리오: A가 주행·주차(S 플래그 7, 토큰 행 `moved` 뒤 S, 순서 "TS"), B는 GPS 없이 바퀴 정확히 0으로 120 s → 쓰기 없음, 토큰 A, `g3-worker-boot`=B; C는 `CARRIED chain_depart prev A`. 같은 하네스에서 B를 HEAD식 시작 교체로 돌리면 C는 `not_prev_boot`(결함 재현).
  - B가 1 m만 굴러감: 토큰 쓰기 없음, 출발 표시 M이 들어가 C는 `moving_at_end`. 표시가 부팅 내내 막히면 A의 S가 남아 C가 이월(잔여, 2 m 안의 회전은 약 25° 이하).
  - B가 3 m 이동 뒤 시동 끔: 순서 1(0.5 m에 M, 2 m에 토큰: "MT") → C는 B의 M(`moving_at_end`), 5 s 더 서면 B의 S(방위 없음, "MTS") → C `no_heading prev B`; 순서 2(M이 막힌 채 토큰만 "T") → 디스크는 A의 S지만 C는 `not_prev_boot prev B`, 그 뒤 M이 들어가면("TM") C `moving_at_end`.
  - B가 GPS 방위와 함께 주행·주차: "MTS", 디스크 S의 boot id = B = 토큰 → C `chain_depart prev B`. 2 m 없이 첫 S가 원해질 때(정지 중 GPS 방위): 토큰(`record`) 뒤 S.
  - 눈먼 부팅: 바퀴 이벤트 없이 29.5 s는 쓰기 없음, 31 s에 `blind_silent` → C `not_prev_boot`; 2.5 s 공백 뒤 재개 → `blind_gap`; 1.5 s 공백은 무시.
  - 손상(`garbage`)·없는 토큰: `prev` "unread"/"none" → `not_prev_boot`; 정지 부팅은 파일을 건드리지 않음; 움직인 부팅이 다시 씀. 같은 부팅의 두 번째 시작: `eager_restart`, `prev`=자기, A의 기록 거부. 토큰이 이미 이번 부팅이면 할 일 없음.
  - 문의 재시도: idle 규칙이 막는 동안 요청 없음 → 풀리면 요청, 취소 결과 뒤 4.8 s 안에는 재요청 없음, 5 s 뒤 들어감. 교체 중 기록기 지연 101 ms는 막고 100 ms는 통과. 대상 없는 호출자는 토큰 요청을 못 받음. 바퀴 움직임·흐름 정지로 취소되지 않고 idle 규칙으로는 취소. 우편함이 토큰 줄을 `g3-last-start`로, 기록 줄을 `g3-heading.v1`로 씀.
  - 토큰 쓰기와 표시가 모두 실패한 부팅(문서화한 잔여): 30 m 주행에도 쓰기 실패 3회로 저장이 꺼지고 A의 S·토큰이 남아 C가 이월. 종료 동기 쓰기(`capture_stop`)가 이것을 닫고 두 번째 호출은 행을 더하지 않음. 동기 쓰기 실패는 `sync_failed`.
  - 표식: garage 끔이면 `swap`을 줘도 지연 토큰 없음, `prev`·`swap` 필드 없음, 토큰 요청 없음. garage 켬이지만 호출자가 시작 때 바꾼 경우(swap 없음)도 지연 없음. boot id "unknown"은 쓸 것이 없음.
- `test_worker_beta`(실제 워커·저널 기록기·저장 스레드, 위치 행 없음): `g3_idle`(8 s 정지: `chain_depart prev cccccccc swap lazy`, 실행 중 토큰 = 이전 부팅, 캡처 정지 뒤 `capture_stop`/`sync`로 이번 부팅), `g3_restart`(`eager_restart`, `not_prev_boot`, 토큰 행 없음), `g3_blind`(움직임 이벤트 없음: 30.1 s 뒤 `blind_silent`/`written`), `g3_unadmitted`(모든 바퀴 이벤트가 세션 전 전송 시각이라 MODEL에서 배제되어 파이프라인·출발 표시는 움직임을 못 봄; 원시 감시가 `moved`로 토큰을 바꿈), `g3_garage`(첫 행이 `token moved written prev cccccccc`, 그 뒤 S·S(fsync)·M), `g3_park`(g3-garage-off 표식: 시작 교체 그대로).
- `test_replay_g3_garage.py`: 켬 실행의 A에 토큰 행 하나(`written/moved/prev none`)가 첫 S 앞, `g3-worker-boot`=A, 시작 행 `swap lazy`; 끔(표식) 실행은 토큰이 시작 때 바뀌고 토큰 행·worker-boot 파일 없음.
- `test_analyze_g3.py`: 토큰 행 검증(결과 5종, 잘못된 행은 `malformed_g3_row`), garage 쓰기(lat_ms·저널 지연 규칙)에 포함, 부팅별 `token` 목록과 출력 줄, 토큰 행 없는 옛 로그는 키·줄 없음; garage 계수 함수가 st/hv 없는 행에서 KeyError 없음(검토 잔여).
- `test_trial_menu.py`: 메뉴 1·메뉴 4가 토큰을 지우고 기록(`g3-heading.v1`)은 둠, 심볼릭 링크는 따라가지 않고 지움, 디렉터리는 둠.

### 결과

- 호스트 `make test`(D-Bus 헤더·라이브러리를 스크래치 경로로 make 변수로 넘김, `MX5DR_STOCK_ROOT` 읽기 전용): 종료 0, 33분. unittest 1,146건(건너뜀 17: 고정 ARM 컴파일러가 호스트 경로에 없어 bds 파싱 6, 릴리즈 번들 미지정 4, 비공개 자료 변수 미지정 등 — 이 변경과 무관), `test_g3` 1,699 검사(HEAD 1,344), navigation 3,977, core 1,872, `test_worker_beta` 시나리오 전부(목록의 23개 + 타이밍 래퍼 `g3_garage`·`g3_cancel`, `WORKER_BETA_TIMING_RETRY` 없음). 이 실행 뒤 더한 것: `g3_unadmitted` 시나리오(단독 실행 통과)와 `token_watch`의 null 안전 비교(`same`, 변이 M4가 컴파일러의 nonnull 경고로 빌드 실패하던 것을 고침; 단독 `test_g3` 1,699 통과). 영향받는 묶음(`test_g3`, `test_worker_beta g3_idle/g3_restart/g3_blind/g3_unadmitted/g3_garage`, `test_analyze_g3`, `test_replay_g3_garage`, `test_trial_menu`의 새 사례)은 최종 소스로 다시 통과했습니다.
- 정확한 ARM: 고정 도구체인(GCC 4.9.1)으로 `tools/build_arm.py` 빌드(`Verified ARM build`, `release_verified=true`, libmx5dr.so sha256 `d49e5785…`), `tests/run_arm_all.sh` 종료 0, FAIL 없음, PASS/passed 줄 432개(같은 방식으로 센 직전 garage 수정의 ARM 실행 428개 + 새 시나리오 4개), 시간 계열 시도 67개 모두 첫 시도 통과, **ARM_TIMING_RETRIES=0**, ARM_TIMING_FAILED=0. 새 `worker-beta-test g3_idle/g3_restart/g3_blind/g3_unadmitted`는 첫 시도 통과(부하 0.42~0.64). AA 설치 프로브(libpatch)는 이번에 돌리지 않았습니다(이 변경은 설치·후킹 경로를 건드리지 않음).
- 비공개 재생(20개 부팅, 재생 가능 14·위치 행 없음 6, 하나의 G3 저장 디렉터리로 연쇄, `--real-only --g3 on --g3-send on`, 111개 파일 중 저장 디렉터리를 뺀 110개, 저장 스레드 시각 필드 `lat_ms`·`start_ns`·`end_ns`만 정규화):
  - 새 코드 `--g3-garage off` 대 beta.16(`0af9a0f`, 표식 개념 이전): **110/110 동일**. 대 HEAD(`7358b08`) `--g3-garage off`: **110/110 동일**, 저장 디렉터리의 `g3-heading.v1`·`g3-last-start` 해시 동일, `g3-worker-boot` 없음.
  - 새 코드 `--g3-garage on` 대 HEAD `--g3-garage on`: 송신 기록·보고서·CSV·저널 96개 파일 동일, 다른 것은 14개 부팅의 g3 행뿐이며 차이는 (1) 토큰 행(`moved` 5개 부팅, `blind_silent` 8개 부팅 — 영속 프로파일 재생은 원시 행이 트리거 창에만 있어 창 밖은 바퀴 이벤트가 없으므로 30 s 침묵이 됨, 차량 의미 아님), (2) 시작 행의 `"swap":"lazy"`, (3) 05_ef2c2e67의 `prev`(04_7cb3b7dc 재생이 토큰 조건에 이르지 않아 `65258c5f`, HEAD는 `7cb3b7dc`; 결정은 둘 다 `no_heading`)뿐입니다. 복원 결정(`chain_depart` 2, `not_prev_boot` 1, 나머지 거부)·쓰기(st·seq)·송신은 같습니다. 정지 부팅 6개는 위치 행이 없어 재생되지 않으므로(HEAD에서도) 이 결함은 재생으로 재현되지 않으며, 효과의 근거는 아래 디지스트 판정입니다.
- 실제 정지 부팅 6개의 지연 규칙 판정(2026-10-17 내보내기 디지스트, FACT): 094c62b5·e792f96d·5d6260d5·62bb2476·7ce61caf·07ea46fa 모두 10 s마다 바퀴 이벤트 58~99개, 최대 수신 간격 104~995 ms(< 2 s), 끝의 센서 정지 뒤 저널 끝까지 0.0~6.3 s(< 30 s), 최고 속도 0.00~0.20 km/h에서 거리 상한(최고 속도 × 창 길이) 0.00~0.83 m(< 2 m), 위치 행 없음(방위 없음 → S 요청 없음), 캡처 정지 없음. 따라서 새 코드는 이 6개 부팅 어디에서도 토큰을 바꾸지 않습니다(디지스트 사이의 공백은 디지스트에 보이지 않으므로 INFERENCE의 여지는 남음).
- 반사실 사슬(`G3_UNC_LIMIT_2026-10-18.md`의 수치, 그 사슬들은 옛 빌드라 실제로는 플래그 4 기록이 아니었음; INFERENCE):
  1. 322800ef → [094c62b5] → e5584f62(집 나선): HEAD `not_prev_boot` → 새 코드는 직전 제품 부팅 검사를 통과합니다. 그러나 첫 FIX의 한계값 80.5° > 60°라 상승 중 `unc_grew`로 닫혀 송신은 바뀌지 않습니다(운반 오차 −34.3°; L ≥ 90이었다면 62개 FIX).
  2. 0914e18f → [e792f96d, 5d6260d5, 62bb2476, 7ce61caf] → 5a9f9b2b: HEAD `not_prev_boot` → 새 코드는 창이 열립니다(첫 FIX 한계값 51.7°, 운반 오차 +4.9°, `trusted`로 5개 송신). 그 60 s 동안 순정은 30° 넘게 틀린 FIX가 없었으므로(0/0/0) 이득도 손해도 거의 없습니다.
  3. 41945267 → [07ea46fa] → 3121d513(관측 안 된 하강이 사이에 있어 연구에서 뺀 사슬, 운반 오차 +69.5°, 첫 FIX 한계값 49.5°): HEAD는 07ea46fa의 시작 교체로 거부했습니다. 지연 교체만이면 이월되어 60개 FIX를 +69.5° 틀린 채 보냈을 것입니다(그 순간 순정은 더 나빴음: 60개 중 38개가 90° 넘게 틀림). 07ea46fa는 beta.16 설치 부팅으로 보이므로(위) 설치 시 토큰 삭제로 다시 거부됩니다(`prev` "none"). 실제로는 41945267의 마지막 기록이 방위 없음(hv=0)이라 어느 빌드도 이월하지 않았습니다.
  4. 이마트(3121d513 → da8288d5)와 나머지 직접 사슬 3개: 사이 부팅이 없어 바뀌지 않습니다.
- 손 변이 18개, 각각 단독 적용(패턴이 정확히 한 번 있는지 확인, 빌드 실패는 잡힘으로 세지 않음): **모두 잡힘**. 처음 실행에서 세 개(M4, M6, M17)는 빌드 실패(미사용 매개변수 `-Werror`, 시험 쪽 nonnull 경고)였고, 패키징 두 개(M12, M18)는 시험 모듈 경로를 잘못 불러 가져오기 오류로 실패했습니다. 변이 표현을 고치고(`(void)`·`false &&`) 시험을 null 안전하게 바꾸고 패키징 시험을 올바른 디렉터리에서 불러 다시 돌려 의도한 단언으로 잡히는 것을 확인했습니다.

| 변이 | 내용 | 잡은 시험 |
|---|---|---|
| M1 | 지연 경로도 시작 때 토큰 교체 | test_worker_beta g3_idle |
| M2 | 첫 S를 토큰 요청으로 바꾸지 않음 | test_g3 (순서 "TS", `record`) |
| M3 | `blind_gap` 없음 | test_g3 token_watch |
| M4 | `blind_silent` 없음 | test_g3 token_watch (worker g3_blind도) |
| M5 | `moved` 문턱 0.5 m | test_g3 token_watch (1.9 m) |
| M6 | 토큰이 idle 규칙 무시 | test_g3 token_gate |
| M7 | 교체 중 기록기 지연 100 ms 규칙 무시 | test_g3 token_gate |
| M8 | 토큰이 바퀴 움직임으로 취소됨 | test_g3 token_gate |
| M9 | 같은 부팅 재시작을 첫 시작으로 봄 | test_g3 token_codec (worker g3_restart도) |
| M10 | 재시작에서 `prev`를 읽은 토큰으로 둠 | test_worker_beta g3_restart |
| M11 | 캡처 정지의 종료 동기 쓰기 없음 | test_worker_beta g3_idle |
| M12 | 설치가 토큰을 지우지 않음 | test_trial_menu |
| M13 | 대상 없는 호출자에게 토큰 줄을 넘김 | test_g3 token_gate |
| M14 | 이미 이번 부팅인 토큰을 다시 씀 | test_g3 token_chain |
| M15 | garage 표식에서도 지연 교체 | test_worker_beta g3_park |
| M16 | 실패·취소된 토큰도 끝난 것으로 봄(그 뒤 S 허용) | test_g3 token_gate |
| M17 | 원시 감시 공급 없음(파이프라인 거리만) | test_worker_beta g3_unadmitted |
| M18 | 제거가 토큰을 지우지 않음 | test_trial_menu |

### 남은 위험 (지연 교체로 새로 생기거나 커진 것; HEAD는 시작 교체가 들어갔다면 잡았음)

1. 토큰 쓰기와 출발 표시가 모두 실패한 주행 부팅: 쓰기 실패 3회(저장 꺼짐)나 주행 내내 5 s마다의 재시도가 모두 idle 규칙에 막힌 경우(저널 실패는 종료 동기 쓰기가 잡음). 그러면 낡은 S가 그 주행의 회전만큼 틀린 채 이월될 수 있습니다. 재생에서는 토큰이 첫 2 m에서 바로 들어갔지만(재생은 idle 규칙 중 교체 상태·기록기 지연만 모사) 차량의 실패율은 측정되지 않았습니다.
2. 워커가 붙기 전(2026-10-17 부팅들에서 커널 부팅 뒤 38.6~44.3 s)에 출발·정지가 모두 끝나고 그 뒤 움직이지 않은 부팅(예: 시동 직후 주차 칸 옮기기).
3. 2 m 미만 이동에 출발 표시까지 막힌 경우: 2 m 호에서 최소 회전 반경(약 4.7 m)으로 약 25°까지의 회전이 이월 방위에 들어가지 않습니다.
4. 프로세스가 부팅 중간에 죽고 같은 부팅에서 다시 시작하지 않은 뒤의 주행, 원시 바퀴 흐름이 주행 중 끊긴 채 돌아오지 않고 30 s 안에 부팅이 끝난 경우.
5. 제품이 없던 기간: 설치·제거가 토큰을 지우므로 메뉴 1/4를 거치면 잡습니다. 가드 거부는 연쇄 토큰(`chain_broken`)이 잡습니다. BETA가 아닌 모드의 부팅과 제품이 시작하지 않은 시동은 HEAD처럼 못 잡습니다(모드 변경은 설치를 거침).
6. 비용: 토큰이 들어간 부팅 다음에 표식 부팅·액세서리 30 s 이상·캡처 정지(메뉴 3) 부팅이 있으면 이월하지 않습니다(보수 쪽).

### AGENTS.md 토큰 문장 교체 문안 (AGENTS.md 자체는 고치지 않음)

`AGENTS.md`의 garage carry 문단(위 "AGENTS.md-Text" 절의 문안과 같은 문단)에서 아래 세 곳을 그대로 바꿉니다. 나머지 문장은 그대로입니다.

1. "written by another boot that is the previous product boot (logs/g3-last-start, swapped at every BETA worker start before the record is read, also with either marker; otherwise CARRIED_UNVERIFIED not_prev_boot, no window)," →

"written by another boot that is the previous product boot (logs/g3-last-start, read at every BETA worker start before the record; with the garage carry on it is rewritten lazily: a boot writes its own id only once it observed 2 m of raw wheel distance (every received wheel event, also those excluded before the session/bus admission), was blind (a raw wheel receipt gap above 2 s that ended, no raw wheel event for 30 s while the worker ran, or an undecodable wheel value), wants its first S record (the token write lands before that record, so a carried record and the token always name the same writer) or stops observing (journal or audit fault, capture stop, no store thread: one synchronous write), so a boot that only stood with the ignition on keeps the earlier boot's record carryable; a second worker start in the same boot (logs/g3-worker-boot already names it) or a lost worker-boot write swaps at start and also refuses every other boot's record; with either marker or without the BETA core the token is swapped at every worker start as before; menu 1 installation and menu 4 removal delete it; otherwise CARRIED_UNVERIFIED not_prev_boot, no window),"

2. "a drive without any product start, a non-BETA mode, a boot whose worker did not reach the token swap (stop requested, locale failure, no worker thread), a turntable or mechanical parking system, or a power cut soon after the mark leaves a stale S (a second worker start in the same boot refuses the previous boot's record: not_prev_boot)," →

"a drive without any product start and without a menu 1/4 installation step in between, a non-BETA mode, a boot whose worker did not reach the token read (stop requested, locale failure, no worker thread), a turntable or mechanical parking system, a power cut soon after the mark, and with the lazy token a garage-on boot that moved but whose token write and departure mark both failed (three store I/O failures, or the idle rule blocking every 5 s retry for the whole drive without a journal fault), whose whole motion ended before its worker attached (about 39-44 s after power-on in the 2026-10-17 boots) and that never moved again, whose process ended mid-boot without a same-boot restart before the token landed, whose raw wheel stream stopped while the car still moved and the boot ended within 30 s, or that rolled less than 2 m while its mark was blocked (up to about 25 deg of turning) leaves a stale S (a second worker start in the same boot refuses the previous boot's record: not_prev_boot),"

3. "The previous-product-boot token write (logs/g3-last-start, temp+rename without fsync) is a further exception to that invariant: it runs at every BETA worker start, with or without the markers, including a mid-drive worker restart in the same boot." →

"The previous-product-boot token write (logs/g3-last-start, temp+rename without fsync) is a further exception to that invariant: with the garage carry on it is requested through the store thread under the departure mark's gate (idle rule, 5 s request spacing shared with the records, while BETA replaces only with a journal writer lag <= 100 ms, also while the car moves, not cancelled by moving wheels or a stale stream) until it lands once per boot, written synchronously on the worker only at a journal or audit fault, a capture stop or without a store thread, and every garage-on worker start writes the small logs/g3-worker-boot file; with either marker or without the BETA core it runs at every BETA worker start as before, including a mid-drive worker restart in the same boot."
