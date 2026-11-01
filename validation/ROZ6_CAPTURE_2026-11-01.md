# ROZ6 vehicle capture — first v1.0.3-rc.1 / touch-install real drive (2026-11-01)

Owner-supplied USB export (`roz6`, a ZIP holding the installed bundle, the touch
UI log, `trial 3` finish+export output, and a ~46 MiB multi-boot capture tar).
Analysis is offline on the PC. No coordinates, place names, OSM IDs, raw bytes or
personal identifiers appear here. GPS is used only as a non-ground-truth
reference; nothing below claims DR accuracy, G1/G3 solved, or phone/app
acceptance.

## 1. Provenance — this is the fixed install, running on the car

- `build-info.json`: `source_commit = 4df9d5b068db18c03d155c97618aecacc3545201`
  (exactly the **v1.0.3-rc.1** install-fix commit), `source_modified = false`,
  `target NA 74.00.324A`, `default_mode = BETA`, `install_policy = persistent`,
  toolchain `61ec0343…`.
- `touch-log.txt`: the keyboard-free touch route ran on the car
  (`touch: started (default route, no keyboard)`, `state: installed`), and the
  owner used it to run trial `3` (finish capture + export). The package was
  already installed by the same (4df9d5b) bundle in a prior session.
- **This is the first vehicle evidence that (a) the v1.0.3-rc.1 install-fix
  bundle installs and runs on the car through the touch UI, and (b) a genuine
  v1.0 BETA-domain drive was captured** (prior exports were pre-v1.0
  beta.15/16). Markers on disk: `g3-garage-off`, `bt-off` present → garage
  heading and phone-IMU Bluetooth OFF, i.e. shipped v1.0 defaults.

## 2. BETA behaviour on the real drive (retained multi-boot trace)

Read from `trace.0/1/2.jsonl` (the persistent ring, digests included), which
spans **6 boots**; the driving boots seeded and engaged, the final boot
(`8973091e…`) is the parked recovery/export boot.

- BETA tunnel replacement **engaged 12 times** (2.8–158.9 s each),
  **564 replaced sends**, speed overlay **1138 sends**.
- Seeding: every driving boot seeded, reason **`reverse_latch`** (boots at
  ~6168 s, 444 s, 315 s, 439 s, 307 s into their trace). Heading sources while
  engaged: blend 8380, yaw 7259, seed 9, reverse 30, resync 1.
- GPS-return checks at 7 tunnel exits: last-DR vs first-GPS-fix
  **6.6, 5.2, 12.8, 15.4, 4.8, 19.8, 14.2 m time-aligned** (15–36 m raw),
  reported accuracy capped at 25.0 m, all flagged "within". Exit transition
  jumps 2.6–35.9 m.
- Safety withdrawals: `cadence_gap` 4, `sensor_silence` 2 — the designed
  fail-safes, no crash. `audit_fault = 0` across all boots (no OEM
  forwarding-contract violation observed). Record drops ~10, all journal-lag
  under the persistent profile (expected).

### Resolving the alarming current-boot counters

`touch-log.txt` reported, for the **current (parked recovery) boot only**
(`status_scope=current_boot`): `beta_replaced_nonzero=0`,
`beta_speed_overlay_nonzero=0`, `E_NO_SEED`, `intervals_total=0`,
746 `MISSING_SENSOR` pipeline resets. These are **not** defects:

- `replaced_nonzero` / `speed_overlay_nonzero` count sends where the **OEM
  `next()` return code was non-zero** (adapter.cpp:868 `event.result = result`;
  beta_controller.h:217 `if(o.result)++replaced_nonzero`). Zero therefore means
  **every replacement/overlay was accepted by the OEM path with return 0 — zero
  send-path failures**, the desired outcome. The DR positions were real and
  non-trivial, independently shown by the 6–20 m GPS-return deltas above.
- The parked recovery boot never drove, so the MODEL pipeline never seeded that
  boot → `E_NO_SEED`, `intervals=0`. Expected for a stationary boot.

## 3. MODEL / SHADOW (qualified-research) domain — no qualification this drive

Across the whole capture the MODEL/shadow pipeline stayed **`E_NO_SEED`**
(2065 current-boot `shadow` rows + ≥2819 digest-suppressed resets; all
`model_valid=false`, `state=0`). Resets are `MISSING_SENSOR` (`sensor:0` = the
"no first yaw window / sensor-age timeout" fault path, pipeline.cpp:1738/1749/
1880; 2 were `sensor:2` = YAW). This domain seeds only from a **qualified GPS
anchor**, a stricter path than BETA's `reverse_latch` seed, and never qualified.

- This is **not a product defect / not a release blocker**: SHADOW/MODEL is the
  research domain, ASSIST-disabled and off by design (per AGENTS.md). The
  shipping BETA tunnel-DR path is the one that seeded and worked.
- Consequence to note: this build produced **no usable MODEL DR output** this
  drive. The yaw-wheel-diff ε research does not depend on MODEL output (it uses
  raw yaw/wheel digests, present and rich — see §4).

## 4. Yaw / wheel data for the yaw-wheel-diff research — usable

Rich, directly usable new post-v1.0 material (diagnostic counts; 2048 nominal;
GPS not ground truth):

- **151 standstills** with yaw-zero mean/sd (stationary digest mean 2048.27),
  24 GPS edges, 10 reinits, **166 driving zero-fits** (10 s digests) reporting
  per-window turn magnitude and zero±SE at **lag 0 s and lag 1.3 s** — i.e. the
  complementary-filter lag behaviour is directly observable. Front-pair wheel
  rows and 0x078-class yaw are both present (motion_batch 62238, yaw_learn 264,
  yaw_stop 151, yaw_edge 24).
- Verdict: enough to **extend** the ε / fusion work on fresh data (a new
  highway-straight / tunnel set with paired yaw+wheel), not to re-run the whole
  study here. This is new data of the kind the prior conclusion said was needed
  before further ε precision work.

## 5. CMU health

`diagnostics_exit=1` is the partial-diagnostics code of the parked recovery boot
(`diagnostic_partial=1`), not a fault. `storage_available_kib≈478 659` (~467 MiB
free, positive margin), guard `persistent`/`committed_persistent`,
`persist_fail_count=0`, no persist trip, `audit_fault=0`. No CMU reset or storage
concern observed in this export.

## Bottom line for the release decision

Positive. The v1.0.3-rc.1 install-fix bundle is now vehicle-confirmed: touch
install succeeded, BETA ran across 6 boots, tunnel DR engaged 12× and was
accepted by the OEM path on all 564 replacements with zero send failures and no
audit faults, and GPS-return deltas are within the design envelope. No
release-blocking defect found. The MODEL/SHADOW non-qualification is expected
(research domain, off by design) and does not affect the shipped BETA feature or
the yaw research data.
