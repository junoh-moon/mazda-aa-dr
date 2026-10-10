# 정지 후 출발 시 E_FRAME 철회 (2026-10-10)

대상: v1.0.0-beta.7 실차 주행 로그(2026-10-10, trip6, 비공개), 기준 HEAD 1767396(beta.8 로직 포함).
두 갈래의 독립 조사(Claude 서브에이전트, Codex 독립 실행)가 같은 원인과 같은 수치에 도달했습니다.
최종 수정은 Codex 안(`hold_stopped_yaw`, 정지 유지 + 방위 고정 + 불확실성 가산)을 채택했고,
비공개 재생 테스트·추가 테스트·이 기록은 두 결과를 합친 것입니다. 오프라인 분석·재생·호스트
테스트이며 차량·폰·DHU 검증이 아닙니다. 좌표는 쓰지 않습니다.

## 1. 증상

가장 긴 터널 모드 BETA 에피소드(GPS 끊김 3253.55 s, ENGAGED 3253.67 s)가 3358.97 s에
`ENGAGED->WITHDRAWN core_rejected`, `core_result E_FRAME`(정직 예산 368 m: 마지막 발행 예산이며
원인이 아님)로 끝났고 capture_stop(3524.7 s)까지 다시 ENGAGED가 되지 않았습니다.

## 2. 증거 (raw_window 행 + 직후 direct 행, 3299-3389 s 연속 10 Hz)

| 입력 | 값 |
|---|---|
| 정지 | 3341.796-3358.859 s 네 바퀴 raw 10000(0 km/h). 3342-3352 s digest yaw 평균 2045.8-2050.8(|rate| ≤ 0.0025 rad/s) |
| 후진 | seq 65636, 3355.764 s `reverse=1`(그 전 마지막은 843 s의 0) |
| 적용 바퀴 | seq 65697, 3358.859 s, `[10000,10000,10000,10000]` |
| yaw | seq 65700, 합 10043 / count 5 = 2008.6, 창 [3358.861, 3358.963] s, `(2008.6-2048)×-0.000658615 = +0.025949 rad/s` |
| 다음 바퀴 | seq 65699, 3358.960 s, `[10000,10015,10000,10013]`(평균 0.07 km/h) |

yaw 창 평균은 3358.657 s 창부터 커졌습니다(+0.004, +0.011, +0.018, +0.026 rad/s). yaw가 첫 바퀴
펄스보다 약 300 ms 앞서 오르기 시작했습니다(0.02 rad/s를 넘은 창은 99 ms 하나)(조향하며 후진 출발).

리뷰의 yaw_stop 통계(같은 주행): 정지 중 |yaw| 편차는 최대 18 count(한계 30 count = 0.02 rad/s)였고,
한계를 넘은 것은 출발 직전 0.3 s 이내뿐이었습니다.

## 3. 원인 (확인)

Pipeline은 yaw 콜백을 직전 콜백부터의 평균 창으로 만들고 창 시작 시각으로 정렬합니다. 창
[3358.861, 3358.963]은 3358.960 s 바퀴 이벤트보다 먼저 drain되어 마지막 0 바퀴 표본과 함께 BETA
코어에 들어갑니다. BETA 코어는 네 바퀴가 정확히 0일 때만 정지로 보며(`stop_enter_mps` 0,
`stop_exit_mps` 0.0005), 정지 상태에서 |yaw| > 0.02 rad/s면 `fail(E_FRAME)`이 seed를 지워
BetaController가 `core_rejected`로 철회합니다. 창 시작부터 다음 바퀴까지 99.296 ms 동안 정지 해제
조건이 없습니다.

**재생 재현:** `tests/replay/test_replay_frame_restart.py`가 기록된 motion(3299-3372 s)을
`build/replay_beta`(실제 Pipeline/BetaController/브리지, 가짜 OEM send)에 넣습니다. 이 구간에는
GPS가 없으므로 **seed용 GPS fix는 기록된 바퀴/yaw를 가상 원점에서 적분해 합성**하고(3300.37-3314.37 s
1 Hz) 3315.37 s부터 mode 0이며, 시작 시 forward REVERSE 하나를 넣습니다(digest: 843 s 이후 0). 수정 전
HEAD에서 `3359.0 s ENGAGED->WITHDRAWN core_rejected E_FRAME`이 재현되었고, 임시 계측으로 실패 구간이
`[3358.861, 3358.900] s, speed 0, yaw 0.02595 rad/s, reverse 1, stopped`임을 확인했습니다(계측 원복).
Codex 실행도 3340-3361 s 국소 재생에서 3359.000 s에 같은 결과를 얻었습니다. 원래 에피소드 전체
재생은 앞부분 RAW가 압축 기록이어서(에피소드 중 15.4 s RAW 공백) 불가능합니다.

**두 번째 후보(약 3365.5 s 정지):** 바퀴 0은 3365.469-3366.993 s, yaw 창 [3366.895, 3366.996]
0.027 rad/s, [3366.996, 3367.100] 0.040 rad/s, 첫 바퀴 펄스 3367.096 s. 실차에서는 에피소드가 이미 끝나
도달하지 않았습니다. 재생에서는 정지 확정(1.5 s 연속 속도 0 + |yaw| ≤ 0.02)이 0.027 rad/s 창 때문에
약 74 ms 모자라(3366.895 s에 dwell 1.43 s) 정지 상태가 되지 않았고, 따라서 E_FRAME 경로가 아니라 기존
"정지 확인 대기" 경로(속도 0, yaw 적분)를 지났습니다. 계측으로 hold 경로는 후보 1의 세 구간에서만
발동함을 확인했습니다. 정지 시작 시점과 dwell 경계에 따라 실차에서는 E_FRAME 후보가 될 수 있었으므로
수정은 필요합니다.

## 4. 다른 종료 경로

`mx5_dr_step` 오류를 BETA 구간 생성(`Pipeline::advance`/`beta_step`)과 대조했습니다.

| 오류 | 판단 |
|---|---|
| E_FRAME 정지 yaw | **실차 발생**, 이번 수정 |
| E_FRAME reverse 값 | 불가능: latch 값은 0/1만, 그 외는 enqueue에서 BAD_INPUT |
| E_NUMERIC | 사실상 불가능: 바퀴 raw ≤ 40000(83 m/s), 음수 바퀴 BAD_INPUT, yaw 평균 0-4093 → ≤ 1.35 rad/s |
| E_QUALITY | 불가능: raw_yaw ≥ 4094·count 0은 Pipeline이 먼저 거부, 증거는 항상 MODEL |
| E_SEQUENCE, E_CONTEXT, DUPLICATE | 불가능: interval_seq 단조 증가(소진 시 DISABLE), 종류별 receive_seq 단조 검사, latch 증거 구간별 새 seq |
| E_TIME | **바퀴만 250 ms 넘게 끊기고 yaw는 계속 오면 `core_rejected`로 철회될 수 있음.** 비공개 로그 연속 구간에서 > 250 ms 간격은 3건(trip6 trace.1 72.7·74.1 s, trip4 trace.0 4237.1 s)이고 모두 바퀴·yaw 동시(→ MISSING_SENSOR/`sensor_silence` 경로), 에피소드 밖. 바퀴 단독 간격은 0건. `sensor_gap` 사유 분리는 BetaController 사유 선택과 worker 테스트가 필요해 이번에는 넣지 않았고 미관측 한계로 남깁니다 |
| E_LIMIT, E_STALE | `budget_limit`/`lease_expired` 등 별도 사유, core_rejected 아님 |

다른 비공개 로그: trip2 trace.0, trip3 trace.0/1, trip4 trace.1은 BETA 이전 모드(4)입니다. trip4(beta.3)
trace.0의 `core_rejected` 537건은 ENGAGED가 된 적 없는 상태의 `last_skip`이고 WITHDRAWN은 없습니다.
beta7 로그에는 없습니다. 코어 오류로 철회된 것은 trip6의 이 한 건뿐입니다. 코어 밖 종료 경로
(`sensor_silence`, `lease_expired`, `cadence_gap`, `session_storage_changed`, `model_not_ready`,
`reverse_latch_suspect` 등)의 노출도 표는 Codex 조사 결과를 따릅니다(상: sensor_silence, lease_expired,
cadence_gap/session_storage_changed).

## 5. 변경 (Codex 안)

- `mx5_dr_config.hold_stopped_yaw`(0/1). MODEL 초기화(`mx5_dr_init_model`)에서만 허용, QUALIFIED 초기화와
  값 > 1은 E_CONFIG.
- 이미 확정된 정지 상태에서 |yaw| > `stop_yaw_max_rad_s`이고 `hold_stopped_yaw && domain==MODEL &&
  speed==0.0`이면 E_FRAME을 내지 않습니다. 정지 상태를 유지하므로 적분 루프의 기존 정지 분기가 실행됩니다:
  위치·body heading 고정, `heading_budget += (yaw_error + |yaw|)·dt`, `error_budget += (speed_error + speed)·dt`
  (코드로 확인; 추가 수정 불필요). 첫 바퀴 이동(speed ≥ 0.0005 m/s)에서 정지가 풀리고 적분이 재개됩니다.
  속도가 0이 아니면(정지 해제 임계 미만이라도) E_FRAME은 그대로입니다.
- `beta_core_config`: `hold_stopped_yaw = p.unbounded ? 1 : 0`(터널 BETA만). bounded BETA, MODEL/SHADOW,
  QUALIFIED, 다른 가드, OEM 계약, beta.8 정지 정확도 규칙은 변경 없음.

## 6. heading 영향

- 출발 직전의 실제 선행 회전은 적분되지 않습니다. 후보 1에서 약 0.026 rad/s × 0.099 s ≈ 0.15°입니다.
- 정지 중 지속 편향(예: 0.03 rad/s)은 회전이 아니라 센서 오프셋으로 보고 방위를 돌리지 않으며, 에피소드도
  끝내지 않고 heading 예산만 |yaw|·dt만큼 정직하게 커집니다. 파이프라인의 회전 예산(`beta_rotation_rad_`)도
  같은 |yaw|·dt를 더하므로 게시 heading 예산에는 보수적으로 이중 반영됩니다(회전 항 계수 0.10이 붙어 정확히는 약 1.1배).
- 정지 확정 전(1.5 s 대기) 속도 0 구간은 종전대로 yaw를 적분합니다(후보 2가 이 경로).
- 주차장 방위(G2) 정확도 보장은 아닙니다.

## 7. 테스트 (스크래치 사본, nice 19, 단일 작업)

- `make test-core` 1872 checks 통과: Codex `model_stopped_yaw_guards`(플래그 0/2/QUALIFIED, E_NUMERIC/E_QUALITY/
  reverse E_FRAME/E_TIME 유지, 속도 0.1 E_FRAME, 이동 시 적분, 정지 hold와 예산 0.0202) + `hold_stopped_yaw_bias_and_restart`
  (BETA 임계: 플래그 없으면 E_FRAME, 0.03 rad/s 30 s 지속 → 실패 없음·방위/위치 고정·heading 예산 +(0.03+0.002)·30,
  0.3 s 선행 hold 후 첫 펄스에서 +0.02 rad 적분 재개, 조용하면 1.5 s 후 재정지).
- `make test-navigation` 통과(test_beta 65580 checks): Codex `tunnel_zero_wheel_yaw_holds_heading`(실측 구간 수치),
  `tunnel_restart_with_yaw_leading_the_wheels_stays_engaged`(10 s 정지, 정지 중 후진, 300/0 ms 선행 → 매 tick 게시,
  선행 동안 방위 고정, 이후 회전), `tunnel_standstill_yaw_bias_holds_heading`(30 s 편향 → 철회 없음, 방위 고정,
  코어 heading 예산 증가 ≈ |yaw|·dt, bounded BETA는 E_FRAME). 기존 `creeping_turn_is_not_a_frame_fault`(bounded)는 그대로.
- `make test-replay-beta` 13 tests OK(비공개 skip), `MX5DR_TRIP6_TRACE` 지정 시 13 OK: 3372 s까지 철회 없음,
  두 정지의 바퀴 0 구간(첫 펄스 직전 10 Hz 호출 포함) 25 m, 이동 구간 40 m.
- `build/test_worker_beta` 11개 케이스 통과.
- 변이: 플래그 무시 → test_core·test_beta·비공개 재생 실패; `beta_core_config` 플래그 0 → test_beta(새 두 테스트 각각
  단독으로도)·재생 실패, test_core 통과(직접 설정, 예상대로); 속도 0 조건 제거 → test_core(이동 입력 E_FRAME) 실패.
- 실행 안 함: 전체 `make test`, ARM/QEMU, test-runtime 나머지, test-adapter, test-integration. 차량·폰·DHU 미검증.

## 8. 한계

- seed GPS가 합성이라 재생은 원인 재현이지 위치 정확도 증거가 아닙니다.
- 출발 선행 회전 누락(≈0.15°/회)은 정지-출발이 잦으면 누적됩니다.
- 바퀴 단독 250 ms 이상 끊김의 E_TIME `core_rejected`(미관측), `sensor_gap` 사유 미분리.

## 9. 다음 주행 로그에서 확인할 것

- 정지 후 출발(조향·후진 포함)에서 `core_rejected`/`E_FRAME` 철회가 없고, 에피소드가 정지 중 25 m(정직 예산 ≤ 150 m),
  출발 후 40 m로 이어져 `gps_returned`로 끝나야 합니다.
- 긴 정지 뒤 `accuracy_honest_m`이 정지 중 yaw 크기에 맞게 증가하는지, 출발 직후 raw window에서 바퀴 0 + |yaw| > 0.02
  구간이 수백 ms 이내인지 확인합니다.

## 알려진 한계 (독립 재검토, 2026-10-10)

- 바퀴가 0이고 |yaw|가 0.02 rad/s를 넘는 상태가 길게 이어지면(실측 정차 중 최대 편차는 13-18 count, 한도 30 count의 약 절반 이하이며 관측된 적 없음) heading 예산이 상한 없이 커집니다. unbounded 모드는 예산으로 게시를 거부하지 않고 보고값은 min(정직 예산, 40 m)이지만, 다음 정지의 래치가 150 m를 넘으면 정지 중 40 m가 보고되고 GPS 복귀 앵커의 heading 오차가 부풀 수 있습니다. 수정 전에는 같은 상황에서 E_FRAME으로 seed 자체를 잃었으므로 순효과는 개선입니다. 정지 중 가산량 상한은 다음 주행 데이터를 보고 판단합니다.
- 출발할 때마다 선행 회전 약 0.0026 rad(약 0.15도)는 적분되지 않고 예산으로만 덮입니다.
