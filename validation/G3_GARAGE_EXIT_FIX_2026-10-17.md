# G3 지하주차장 출구 방위 수정 (garage carry, 2026-10-17)

상태: 원인 분석, 구현, 호스트 시험, 정확한 ARM 빌드·시험, 비공개 주행 기록의 오프라인 재생과 디지스트 수준 재구성만 했습니다. 차량·폰·DHU에서는 실행하지 않았습니다. 이 기록은 릴리즈 승인이나 차량 방문 요청이 아닙니다. 좌표는 적지 않았습니다. 표시: **FACT**(로그·코드에서 직접 확인), **INFERENCE**(해석·모델).

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
| 복원 | flags 4, S, 방위 있음, 연쇄 ok, 다른 부팅이면 벽시계 없이 `CARRIED`(`chain_depart`). 나이를 알면 7일 초과는 거부. 첫 위치의 위치·GPS 나이 검사, `moving_at_boot`는 그대로 | - |
| 불확실도 | 규칙 5 시간 항(0.002 rad/s)과 레일 여유는 그대로, 회전 항 2 %(BETA 10 % 대신), 이 기록의 한계 60° | `G3_GARAGE_ROTATION_PER_RAD` 0.02, `G3_GARAGE_UNC_MAX_DEG` 60° |
| 송신 창 | `chain_depart`에서도 열림. 부팅 뒤 이미 움직였어도 열림(출구에서야 첫 FIX가 오므로), 단 첫 바퀴 움직임 뒤 서 있지 않을 때 요 사슬이 끊기면 열지 않음(`none/yaw_break`). 후진 신호가 아직 없으면 전진으로 봄(0x118은 변화 때만 오므로 "없음" = 시동 뒤 후진 없음), 후진이면 기다리고, 열린 창은 후진 신호로 끝. 그 밖의 끝 조건(신뢰 4회, 충돌 8회, 정책 M, 900 s/60 s, 세션, 이상)은 beta.15 그대로 | - |
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
- **(B) 통과.** 20개 부팅(재생 가능 14, 위치 행 없음 6)에서 `--g3-garage off` 대 master 110개 파일(report, csv, journal, g3 행, sends, stdout/stderr; 저장 스레드 시각 필드 정규화) 모두 동일. 켬: g3 행과 send 기록의 주석(phase/g3 상태, 사유 코드)만 다르고 바꾼 송신 0개(persistent 재생은 원시 창 밖에서 `wheel_stale`로 창이 닫힘 — 근거가 아님). 합성 시험에서 바꾼 송신은 바이트 40, 44..47만 다름. 교체 중 fsync 금지와 표시의 방위 없음은 단위 시험.
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

## 위험과 한계

- 실차·폰 미검증. 네이버가 정지·저속에서 차 방위를 어떻게 쓰는지는 여전히 모릅니다(DHU 10 미완). 실제 자료의 근거는 이마트 사슬 1개의 모델입니다.
- 집 나선 주차장은 회전이 커서(상승 |회전| 약 1,190°, 이동 약 165 s) 모델상 출구에서 불확실도가 50~60°에 이를 수 있습니다. 60°를 넘으면 창은 출구 전에 닫히고 순정으로 돌아갑니다(안전 쪽, 효과 없음).
- 출발 표시와 정차 기록은 fsync 없이 씁니다. 쓰기 직후 30 s 남짓 안에 전원이 끊기면 이전 내용이 남을 수 있습니다. 최악은 출발 표시가 사라져 오래된 S가 되살아나는 경우입니다(표시는 출발 직후 쓰고 지하 주행은 보통 수십 초 이상이라 그 사이 기록되었을 가능성이 큼, INFERENCE). 이번 #13은 정지 뒤 128 s 이상 CMU가 기록을 계속했습니다(FACT).
- 제품이 돌지 않은 이동(이번 #11 끝 → #13 시작 사이처럼 로그 없는 이동)은 연쇄 토큰이 바뀌지 않으면 잡지 못합니다. 첫 위치의 위치 검사(50 m + 위치 예산 + 부팅 뒤 바퀴 거리)는 지하에서 수백 m를 달린 뒤라 약합니다. 그런 경우 틀린 이월 방위가 출구에서 신뢰·충돌·한계까지 나갈 수 있습니다.
- 마지막 정차가 5 s 미만이면 저장되지 않고 디스크에는 출발 표시(M)가 남아 다음 시동은 이월하지 않습니다(안전 쪽).
- 출구 첫 두 FIX 송신은 원본입니다(세대 전환).
- 정차 영점이 정차마다 ±6 카운트 흔들립니다. 이동 중 적분은 고정 2048이며 1 카운트가 100 s에 약 3.8°입니다.
- 회전 항 2 %와 한계 60°는 표본 1개로 정했습니다.

## 다음 주행에서 보여야 할 것 (메뉴 3 내보내기, `analyze_logs.py`)

1. 지하 주차 끝: `g3_store written "st":"S" ... "garage":1, "sync":0`가 마지막 정차 5 s 뒤에 있고 그 시점 BETA가 ENGAGED여도 됨. 출발마다 `"st":"M","mark":1`.
2. 다음 시동: `g3_store loaded reason chain_depart`, `g3_send start "garage":1`, 지하를 오르는 동안 `count` 행의 `unc`(60 미만인지), 출구에서 `choice 6` 송신과 `end`(trusted/m_course)와 `g3_first course`의 `err_p`.
3. 분석기의 `G3 garage writes without fsync` 줄: lat_ms > 500 0, journal lag 겹침 0(아니면 다음 빌드에서 garage 기본 끔).
4. 소유자 관찰: 집 출구 차단기 앞에서 정지했을 때 화살표 방향, 출발 뒤 몇 초 안에 도로와 맞는지.

## AGENTS.md 예외 문구 (리드가 넣을 문장, 이 작업에서는 편집하지 않음)

"Explicit BETA exception extension (garage carry, validation/G3_GARAGE_EXIT_FIX_2026-10-17.md): the BETA_BEARING_OVERLAY (choice 6) window of v1.0.0-beta.15 also opens, without the g3-garage-off marker (menu 12) and without the g3-off marker, for a heading restored CARRIED with reason chain_depart: a store record written by a departure-marking build (flag 4), state S (a departure from it would have replaced it by M), heading present, uncertainty <= 60 deg, boot chain ok, another boot, no wall-clock age needed (the CMU realtime clock stays near 1970). It may open after the car moved since boot (a garage exit) unless a yaw-chain break happened after the first wheel motion while not standing; an unknown reverse latch counts as forward, a reverse latch ends it. The overlaid send, its bytes (40, 44..47 only), wrapper, return value, errno and every end condition are those of beta.15; the propagated uncertainty uses a 2 % rotation term and ends the window above 60 deg. The parked store record may be written without fsync while BETA replaces (5 s of exact standstill); the departure mark is a 110-byte write without fsync; no fsync runs while BETA replaces."
