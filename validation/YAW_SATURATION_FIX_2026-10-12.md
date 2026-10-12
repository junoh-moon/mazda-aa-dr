# Yaw rail handling and the G3 start-up movement check (2026-10-12)

Scope: host code and offline replay only. Nothing here ran on the vehicle, a phone or the DHU. No coordinates
are reproduced. Inputs: `validation/YAW_SATURATION_2026-10-12.md` (VIP accumulator: 0xFFE dropped, 0xFFF summed;
rail about -/+77.2 deg/s), `validation/G3_FIRST_DRIVE_2026-10-11.md` (the trip8c event) and
`validation/NNG_G2_G3_LESSONS_2026-10-12.md` (stock NNG resets its saved state on motion in the first epoch).

## Problem

A yaw window whose truncated mean is >= 4094 made `Pipeline::enqueue_raw_event` return BAD_INPUT, which resets the
whole pipeline: MODEL core, BETA core, reverse latch and the G3 yaw chain. In trip8c (boot 5a9f9b2b) ten
consecutive windows at 2026.7-2027.6 s (receipt time) were genuine saturation during a sharp low-speed turn: BETA
was unseeded until 2382 s on the vehicle and the stops after it stored no heading. The lower rail (mean 0) was
never checked: it was integrated as an ordinary window.

## Changed behaviour

- `BetaProfile::yaw_rail_policy` (new): 0 in `beta_profile()` (bounded/legacy), 1 in `beta_profile_tunnel()`
  (the product). A pipeline without BETA (SHADOW, holdout, QUALIFIED) and policy 0 behave as before.
- Policy 1, for a window with truncated mean 4094/4095 (upper rail) or 0 (lower rail), decided by the newest
  enqueued wheel event within the sample age (250 ms) of the window end:
  - any wheel above exactly zero: saturation. No reset; the rail rate from the exact mean and the existing
    research scale (-0.000658615 rad/count, zero 2048: -77.2 / +77.3 deg/s) is integrated. The BETA core
    (`mx5_dr_config::yaw_rail_excess_rad_s`, MODEL-domain opt-in, rejected by `mx5_dr_init`) accepts the rail
    encoding and adds `BETA_YAW_RAIL_EXCESS_DEG_S` = 12 deg/s per second of rail interval to its heading budget
    (so also to the position budget through the existing heading term). 12 deg/s is the excess of the
    wheel-differential estimate over the rail in the saturated trip8c windows (YAW_SATURATION_2026-10-12.md).
  - all four wheels exactly zero: init marker. A zero-rotation hold: no reset, the value is neither integrated
    nor given to the stationary zero learner; the window's time stays covered so a seeded core is not starved.
    The BETA core still adds the excess allowance for that 0.1 s (it budgets every rail encoding).
  - no fresh wheel event: unknown, the beta.12 reset for the upper rail; a lower-rail window stays ordinary.
- The MODEL core keeps its own guard: an upper-rail interval is a core rejection (E_QUALITY, MODEL core unseeded),
  no longer a pipeline reset, in the BETA tunnel pipeline only. The SHADOW/QUALIFIED cores never opt in.
- G3: `G3Motion::rail_excess_rad` (12 deg/s x rail time while moving) grows the propagated and reference
  uncertainty like the rule-5 terms; the yaw chain is no longer broken by a saturated turn.
- Journal: `beta_summary` gains `"yaw_rail":{"moving","parked","reset","rail_s"}` (boot-lifetime counters),
  only once a rail window was seen; it is rate-limited with the summary row (quiet persistent profile).
- G3 start-up movement check (`G3Motion::boot_motion`, `G3_BOOT_*`): a carried heading (CARRIED or
  CARRIED_UNVERIFIED) is REJECTED with reason `moving_at_boot` (one `g3_store` `checked` row with `boot_motion`
  flags and `gap_ms`) when the first drained wheel event of the boot has a moving wheel (1), the first moving
  event follows a wheel gap > 250 ms (2), or a wheel gap before the first motion exceeds 2 s (4). 2 s: above
  the largest stream gap before the first motion found in startup raw rows (1.6 s, boot 1c1de788, parked) and
  too short to drive off, turn and stop again at parking acceleration.
- Replay: `--yaw-rail on|off` (off = beta.12 counterfactual) and `--heading-trace FROM:TO:FILE`.
- Not done: the wheel-differential bridge (optional, record-only) was skipped.

## Tests (host)

- `test_beta`: `yaw_rail_saturation_while_driving_keeps_the_chain` (1.0 s at the upper rail at 36 km/h after
  the GPS loss: no reset, heading change = rail rate x 1.0 s within 1e-6 rad, latch kept, G3 breaks unchanged,
  budget = near-rail run + 12 deg/s x 1 s + rotation term, MODEL core E_QUALITY; lower rail: same heading as
  policy 0, budget + excess), `yaw_rail_parked_window_is_a_hold` (heading and G3 turn identical to a straight
  window, no reset), `yaw_rail_policy_off_is_beta12` (policy 0 and policy 1 without a fresh wheel event reset
  BETA and the latch; without rail windows policy 0 and 1 give identical per-window publications and MODEL
  diagnostics), profile/config checks (bounded profile 0, tunnel 1, QUALIFIED init rejects the opt-in).
  Mutations checked: tunnel policy 0 (caught by the profile check) and no excess term in the core (caught by
  the budget check) each fail the new tests.
- `test_g3`: `moving_at_boot` (each flag, CARRIED and CARRIED_UNVERIFIED, stays rejected through a course,
  nothing carried = no row, clean start confirms; the rail allowance pushes the carried uncertainty past 30 deg),
  `pipeline_boot_motion` (first sample moving, 0.5 s gap into motion, 1.6 s and 2.2 s parked gaps, a gap after
  the first motion is ignored).

## Replay of the private trips (product tunnel profile, `--real-only --g3 on`, master 3199630 vs this change)

12 boots: 07987d33, 843d1bbf, 65258c5f, 82d37f8a, 322800ef, 094c62b5, e5584f62, 1c1de788, 0914e18f, 5a9f9b2b,
41945267, ef2c2e67. With `--yaw-rail off` every output (report, CSV, journal, G3 rows, stdout) is byte-identical
to master in all 12. With the default (on), report, CSV and stdout are byte-identical in all 12 (no recorded
GPS-loss send changed); the journal differs in the 5 boots that contain rail windows and only there:

| boot | rail windows (receipt s, wheels) | change (on vs master) |
| --- | --- | --- |
| 65258c5f (trip4) | 4236.42, parked | hold instead of reset; the latch is cleared 0.6 s later by the existing stream-gap reset |
| 82d37f8a (trip6a) | 72.32, 72.66 parked; 1631.91-1632.11 (3) moving | parked: as above. Moving: BETA stays seeded through the turn; 5 anchor rows ACCEPTED instead of PREVIOUS/REVERSE/COURSE, one rule F assumption row fewer; the DR heading before the first GPS blend (64.4 deg at 1633.6 s) is within 2 deg of the later GPS-based reseed (66.0 deg) |
| 094c62b5 (trip7) | 60.83, 60.86 parked | hold; a later stream-gap reset clears the latch instead |
| 1c1de788 (trip8a) | 51.19 parked | only the `yaw_rail` summary field |
| 5a9f9b2b (trip8c) | 2026.71-2027.63 (10) moving | see below; 117 anchor/latch rows differ (2027-2427 s), only a sequence number in the 2384 s ENTRY_FRESH row |

No lower-rail window (mean 0) exists in the private data. The record's "1631.9->1632.1" and "2025.5->2027.7" are
the same events on another clock.

trip8c (DATA: `--heading-trace`, BETA body heading; GPS course is not ground truth and lags in the turn):

| | 2024.0 s | 2028.0 s | 2037.2 s | yaw integral 2024.0->2037.2 |
| --- | --- | --- | --- | --- |
| master | 113.5 deg, seeded | unseeded (10 resets 2026.8-2027.8) | unseeded (reseeded 2038.2 in the replay) | -71.0 deg (12 chain breaks) |
| this change | 113.5 deg, budget 11.1 | 298.2 deg, budget 40.2 | 315.5 deg, budget 43.6 (no GPS blend in between) | -157.6 deg (no break) |
| GPS course | 113 deg (32 km/h) | 60 deg (lagging) | 308 deg (71 km/h) | -165 deg |

The DR heading change 2024.0->2037.2 s is -158.0 deg against -165 deg of GPS course (7 deg; the record's rail
reconstruction found 4 deg with other end points), inside the honest budget of 43.6 deg. The first GPS blend at
2037.4 s moved the heading by 1.1 deg.

## Start-up movement frequency (DATA: recorded wheel rows)

Of the 11 boots with motion, 2 have a moving first wheel sample: 5a9f9b2b (about 10 km/h, already turning,
43.8 s after the CMU start) and 322800ef (about 0.4 km/h). Both start inside the capture-start raw window
(span 0), so this was the first sample the worker saw. Gaps > 2 s before the first motion appear only as
persistent-profile logging gaps (65258c5f, 82d37f8a, e5584f62, 0914e18f) whose real stream is not recorded;
the largest gap inside startup raw rows is 1.6 s (1c1de788). The replay therefore over-states LONG_GAP for those
boots; the vehicle frequency of flags 2 and 4 is unknown.

## Limits

- One car, offline replay; the saturation cases are 2 events. The 12 deg/s allowance comes from one turn.
- The parked/moving split uses the newest enqueued wheel event; a car that starts moving inside a rail window is
  treated as parked for that 0.1 s.
- In the BETA tunnel pipeline the MODEL core now survives an upper-rail window without the pipeline reset
  (its calibration and latch are kept) but stops as a core rejection; SHADOW-only builds are unchanged.
- A lower-rail window with all four wheels exactly zero also becomes a zero-rotation hold for the MODEL core of
  the BETA pipeline (before: integrated at +77 deg/s). No such window exists in the private data.
- moving_at_boot also rejects a carried heading after a mere crawl in the first sample (322800ef); the stock rule
  uses 5 km/h. G3 is record-only, so nothing sent changes.
- Exact-ARM suite not run for this change; QEMU/host success is not vehicle validation.

## Bekannte Grenzen aus der unabhaengigen Pruefung

2026-10-12 beta.13 병합본(터널 보고 상한 25 m + 진단 버전 2 + yaw 레일 처리 + G3 시작 이동 검사)을 독립 검토했습니다. Codex 검토는 샌드박스 초기화 실패(`bwrap: setting up uid map: Permission denied`)로 실행되지 않았고, 독립 Fable 검토와 독립 Opus 검토로 대체했습니다. 두 검토 모두 차단 결함은 찾지 못했습니다. 아래 항목은 모두 **수용한 한계**이며 각 항목에 결과를 적습니다. 실차·폰·DHU에서 확인한 것은 없습니다.

1. **주행 중 CMU 재시작(수용)**: 센서 시작 표식(0xFFF 창)이 바퀴가 이미 움직이는 중에 도착하면 정책 1은 이를 포화로 보고 ±77 deg/s를 12 deg/s 예산과 함께 적분합니다. 결과: 영향이 제한됩니다. 재시작 직후에는 앵커가 없고, 조용한 yaw 게이트가 BETA 시드를 막으며, G3는 FIRST_MOVING으로 이월 방위를 거부합니다.
2. **레일 분류 기준(수용)**: 주차/주행 판정은 yaw 창 구간이 아니라 가장 최근에 넣은 바퀴 이벤트를 씁니다. 결과: 정차·출발 경계의 0.1 s 창은 잘못 분류될 수 있습니다. `dr_core.c` 306-316행은 원시 값 0/4094/4095를 이와 별개로 분류합니다.
3. **12 deg/s 초과 허용치(수용)**: 한 주행(trip8c)의 한 회전에서 얻은 경험값입니다. 결과: 보내는 정확도는 25 m 상한에 가려지므로 과소 추정은 로그(`accuracy_honest_m`, 방위 예산)에만 드러납니다.
4. **BETA 켠 SHADOW MODEL 코어 동작 변경(수용)**: BETA 터널 파이프라인에서는 MODEL 코어도 달라집니다. 위쪽 레일은 파이프라인 재설정 대신 E_QUALITY 거부이고, 주차 중 아래쪽 레일은 회전 0으로 처리되며 자이로 영점 학습에서 빠집니다. 결과: BETA 부팅의 MODEL 진단은 beta.12와 직접 비교할 수 없습니다. SHADOW 전용 빌드는 그대로입니다.
5. **G3_BOOT_GAP_MAX 2 s(수용)**: 한 부팅의 최대값 1.6 s(1c1de788)에 근거합니다. 결과: 주차 중 바퀴 간격 2 s 초과가 흔하다면 `moving_at_boot`가 이월 방위를 조용히 거부합니다. G3는 기록 전용이라 송신에는 영향이 없습니다.
6. **`acc-cap-off` 읽기 오류(수용, fail-closed)**: ENOENT가 아닌 오류(EIO, EACCES, ENOTDIR)는 '있음'으로 셉니다. 결과: 그 부팅은 상한과 진단이 모두 꺼져 beta.12 숫자로 돌아갑니다. `shadow_boot` `acc_cap_m=0`과 `diag_boot` `reason=acc_cap_off`로 보입니다.
7. **계수기와 경계 값(수용)**: `yaw_rail` 계수기는 버려진 첫 창도 셉니다. 새 바퀴 표본이 없는 아래쪽 레일은 beta.12와 같이 동작합니다. `DIAG_SAT_ARM_CAP` 15는 넘침 칸(15)과 겹치지만 도달할 수 없습니다. 결과: 로그 해석에서만 주의가 필요합니다.
8. **기록 정정**: `TUNNEL_REPORT_CAP_2026-10-12.md`의 비공개 재생 문단에서 "기본값에서 보고서의 정확도 관련 통계가 달라졌다"는 문장은 틀렸습니다. `--real-only` 보고서에는 정확도 필드가 없어 모든 변형에서 바이트 일치합니다. 차이는 CSV와 journal의 `accuracy_m`뿐입니다. 또한 기록되지 않았던 차이가 하나 있습니다. trip8c(5a9f9b2b)의 G3 `g3_store` 행(2593.5 s)에서 `m_b`가 7055.5에서 7061.5로 바뀝니다. 레일 변경의 효과이며 기본값(상한 켬)에서도 같습니다(독립 검토의 재생 결과이며 이 기록에서 다시 실행하지는 않았습니다).

검토 뒤 수정: 진단 CONTROL/CAP 송신 정확도의 종단 시험(`ReplayBetaDiagnosticArms`), 분석기의 버전 판정을 `diag_boot.version`으로 변경, `replay_beta --diag`와 `--acc-cap off` 동시 지정 시 제품처럼 진단 끄기.
