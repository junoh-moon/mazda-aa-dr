# G3 first-connection heading: feasibility and design - 2026-10-11

Status: feasibility study and design only. No product code changed. Offline analysis of the owner's private vehicle logs (read in a
scratch copy; no coordinates, OEM binaries or raw rows are reproduced here). Nothing here was run on the vehicle, in the DHU, on exact ARM
or in QEMU. This record does not authorize a vehicle visit, a drive or a release.

Goal (docs/PROBLEM_DEFINITION_KO.md, G3, owner 2026-10-11): at the FIRST AA connection after ignition on, before a GPS course exists,
the direction Naver shows must match the car, so that no wrong-direction guidance follows. Owner hypothesis: a parked car does not
rotate, so the last driving heading can be stored and restored at the next boot.

## Sources

| Boot (log) | Build | Date | Rows used | Coverage limits |
| --- | --- | --- | --- | --- |
| 07987d33 (trip2) | v0.3.12-shadow.4 | 10-04 | LDS sideband replies, full motion, collector 1 Hz | no SEND rows; ended by the in-drive CMU reset (1,459 s) |
| 843d1bbf (trip3) | v0.3.12-shadow.5 | 10-05 | position, SEND, full motion, collector | full boot, export while parked |
| 65258c5f (trip4) | v1.0.0-beta.3 | 10-08 | position/SEND in raw windows, beta_anchor 1 Hz, log_digest 10 s, collector (last 1,590 s) | persistent profile: raw rows only 60 s before / 30 s after a trigger |
| 82d37f8a (trip6a) | v1.0.0-beta.7 | 10-10 | as trip4 (trace.1 of the trip6 export) | ended by power cut (last row 2,230 s, no capture_end) |
| 322800ef (trip6b) | v1.0.0-beta.7 | 10-10 | as trip4, collector (last 2,100 s) | export while parked |
| 7cb3b7dc, ef2c2e67 (beta7) | v1.0.0-beta.7 | 10-10 | collector, motion | parked install boots, no AA session |

Duplicated archives (trip3 trace.1 = trip2, trip4 trace.1 = trip3, older/ shadow.4 = trip2) were removed by checksum. The older 0.3.9 /
diag archives contain no traces. LOCATION payload decoding follows src/adapter/adapter.cpp (bytes 16/20 accuracy, 32/36 speed, 40/44
bearing). "Session start" is the first logged type-1 LOCATION SEND (trip2: first LDS reply; its AA session start is not logged).

## 1. What the stock sends at and after the first connection (measured)

Two classes appeared.

Class A, start without a GPS fix (home underground garage, trip2 and trip3). The LDS answers GetPosition with a stored record: mode 1,
utc 0, a fixed position, heading 335 deg, 4 km/h, HDOP 4.4. The stock forwards it as a normal LOCATION (accuracy 8.8 m, speed 1.1 m/s,
hasBearing 1, bearing 335). trip3: 523 of 523 LOCATION sends were this record (byte-identical), from the session start until the USB
unplug; the car drove about 1,990 m (spiral ramp, net rotation about -455 to -510 deg) and the first real fix came 535 s after the
session start, after parking; no GPS course existed in that boot. trip2: the stored record was served until 939 s after boot; the car
started moving at 776 s and the first GPS course at >= 10 km/h came 163 s and 537 m (wheel distance) later. TRIP_SHADOW5 measured the
stored 335 deg about 100 deg away from the real departure direction and the stored position 44-195 m away. The stored record changes
between some boots (another boot: 144 deg / 1 km/h; the beta7 boots elsewhere: 305 deg / 0 km/h, 15 m from that boot's first fix), so it
is an LDS-kept record of unknown age, not "the last ignition-off state".

Class B, start parked outdoors with a fix (trip4, trip6a, trip6b). The position is good, but the bearing carries no heading information:

| Boot | First fix after session | Stock LOCATION in the first 31 sends (car parked, all four wheels 0) | First wheel motion | GPS >= 10 km/h | Distance to it | Departure |
| --- | --- | --- | --- | --- | --- | --- |
| trip4 | at session (0.9 s) | accuracy 2.2 m; bearing 0 in 30, 234 deg in 1 (first metre of motion); speed 0-0.28 m/s | +28.4 s | +43.9 s | about 27 m | forward |
| trip6a | at session (1.2 s) | accuracy 7.4/5.0 m; bearing 0 in 21, GPS-noise courses 67-241 deg in 10 (GPS reports 1-2 km/h, speed 0.3-0.6 m/s sent, position wanders 13 m) | about +63 to +73 s | +80.1 s | about 19 m | forward |
| trip6b | at session (0.7 s) | accuracy 3.0/2.6 m; GPS-noise courses 85-114 deg in 20 (speed up to 0.56 m/s sent, position wanders 11 m), then bearing 0 in 11 | about +24 to +34 s | +51.5 s | about 45 m | forward |

Every send had hasBearing = 1. At 0 km/h the LDS heading is 0 (a placeholder, "north"); with GPS phantom speed it is GPS noise. The same
pattern holds through the whole drives (trip2: 638 of 728 LDS replies at 0 km/h had heading 0). Times after +30 s come from beta_anchor
(1 Hz GPS speed) and log_digest (10 s wheel means), so "first wheel motion" and distances are +-10 s / +-30 % estimates. A reliable GPS
course therefore appeared 44-80 s after the session start in class B and never, or after 12-15 min, in class A.

Count: in 4 of 4 boots with logged LOCATION payloads (trip3, trip4, trip6a, trip6b) and in trip2's LDS replies, the first-connection
bearing was a placeholder (0), GPS noise or the stale stored value. Only the trip6a -> trip6b pair below gives a reference for the true
parked heading.

## 2. What a stored heading would have been worth (measured where possible)

Consecutive boot pairs. 6 boot transitions exist in the logs; only 1 is a clean pair with GPS on both sides.

| Transition | Gap | End fix -> next first fix | Usable? |
| --- | --- | --- | --- |
| trip2 -> trip3 | 12.2 h | trip2 ended by a CMU reset while driving | no (not a park) |
| trip3 -> trip4 | 75.8 h | 16.5 km | no (driven without logging in between) |
| trip4 -> beta7a | 2 days | about 21 km | no (driven in between) |
| beta7a -> beta7b | minutes | both parked, wheels 0, no GPS course | trivially unchanged; no heading truth |
| beta7b -> trip6a | 2.0 h | 698 m | no (driven in between; the guard's `recent=CCCCUCCC` shows more boots than the two kept traces) |
| trip6a -> trip6b | 3.2 h | 6 m (within the 11-13 m standstill GPS wander) | yes, 1 pair |

Pair trip6a -> trip6b (measured end, assumed unchanged while parked):
- trip6a end: GPS course 194-196 deg at 4-11 km/h, then a stop, a short reverse (0x118 = 1, 1.4 km/h) and power-off 145 s after the
  final stop. Yaw integrated from the last good course (counts / count field, zero estimated from the local standstill, 0.000658615 rad
  per count, clockwise positive; verified against GPS course changes in the same window): +1 to +5 deg. Stored body heading: about
  197 deg (194-199 depending on the reference fix).
- trip6b start: the stock sent 85-114 deg for 20 s (error 83-112 deg vs 197) and then 0 deg for 11 s (error 163 deg).
- trip6b departure: forward; 10 s yaw means show about -11 deg then +49 deg, so at GPS >= 10 km/h the travel direction was about
  stored +37 deg (yaw-integrated, no GPS course logged in that window). This is the car turning out of its space, which a continuously
  integrated heading follows; it is not an error of the stored value.
- The stored heading itself is not verified by a GPS course in trip6b (the persistent profile stops raw rows 30 s after the session).

Direction classes over all observed departures and parking ends:
- Departures: 5 of 5 forward (trip2, trip3, trip4, trip6a, trip6b; the 0x118 value was 0 at the first motion). "Stored + 180 deg"
  (reverse out): 0 of 5. "Neither" at the moment of the first reliable course: 2 of 3 measurable (trip4 about +50 to +80 deg, trip6b
  about +37 deg, trip6a about +2 deg relative to the parked heading), all explained by the turn out of the space.
- Parking ends: 4 of 4 contain a reverse manoeuvre (trip3 699.8 s, trip4 4,209 s, trip6a 2,066 s, trip6b 3,356/3,369 s): the owner
  reverses in and drives out forward.

Why the stored value must be the yaw-integrated BODY heading, not the last GPS course (measured at trip ends):

| End | Last good GPS course | Rotation after it (yaw) | Body heading at power-off | Error of "last GPS course carried over" |
| --- | --- | --- | --- | --- |
| trip6a (outdoor space) | 194-196 deg | +1 to +5 deg, small reverse | about 197 deg | 1-5 deg |
| trip4 (underground garage, GPS lost at 0 km/h) | 262 deg at 10 km/h (237 deg at 40 km/h) | about -277 deg incl. reverse-in (zero choice changes it by about 10-20 deg) | about 345 deg | about 83-108 deg |
| trip6b (home spiral garage) | 55 deg at 3 km/h, then mode 0 | >= 372 deg in the logged windows plus a 15 s unlogged ramp segment (about +86 to +137 deg from 10 s means) | not computable from the log | meaningless (more than a full turn) |

Small sample, stated plainly: 1 usable pair; 3 measurable end manoeuvres; 5 departures. The record cannot claim a distribution of the
stored-heading error.

## 3. What the product can do (assessment)

(a) Persist the last reliable body heading. Boots end abruptly: trip6a (power cut), trip2 (reset), beta7a have no capture_end; there is
no shutdown hook. The final stop came 145 s before the power cut in trip6a, so writing at every confirmed stop captures the final park.
Implementable inside the runtime without OEM interaction: the journal writer thread already writes /data_persist/mx5-aa-dr; the guard
already uses write-temp + fsync + renameat + directory fsync (src/guard/guard.cpp). No vehicle risk beyond a small extra file write.

(b) Publish a bearing on the first positions. Implementable as a new send choice that copies the original 48 bytes and changes only
hasBearing/bearing (40, 44) and, while all four wheels read 0, speed (32, 36) to 0. It changes only what is injected (same hook, same
contract as the speed overlay). What is NOT known is whether Naver uses a car bearing at standstill or at session start. Known DHU facts:
a fix without bearing turns gray (EXP5 E2, T9), so a bearing must always be sent; stopped fixes at 5-25 m are accepted, 40 m rejected
(EXP8/9); moving fixes with bearings wrong by 90/180 deg at 40 m were followed on route without harm (EXP7 H3/H4); the stale record with
335 deg and a changing speed kept a blue arrow (EXP5 E1, the arrow "mostly pointed up", so map rotation was not measured). No experiment
measured the displayed direction at a fresh session with a stationary fix. Risk: the stock already sends 2-9 m fixes with bearings that
were wrong by 83-163 deg in the one checkable case, so a stored heading with a budget of a few tens of degrees cannot be worse in the
same situation; a new harm arises only if the stored value is badly wrong (car moved by a drive without the product, towed, turntable) or
if the injected bearing survives after a reliable GPS course exists. AA LocationData has no bearing-accuracy field, so confidence can only
be expressed by sending or not sending (gate by the internal budget, below).

(c) Reverse start. The core already uses travel = body + 180 deg while 0x118 = 1. Measured limit: 0x118 is change-driven and its state is
unknown after boot until the first event (trip3: first event 95 s after the car started moving; trip4: 5.9 s before moving; trip2: 7.2 s
before; trip6a/6b: before the session). With an unknown reverse state the travel direction cannot be derived honestly; the observed
habit (5/5 forward departures, 4/4 reverse-in parks) favours "forward", which is an assumption that must be journaled.

(d) Correction once moving. Policy M already seeds and resyncs the heading from a GPS course (3 m chord, 5-fix consistent course,
course-vs-yaw agreement) and the FIX class passes the original. The restored heading must end at the first M-accepted course; the error at
that moment is the key validation metric.

Without new vehicle risk: (a) fully; (b) and (c) technically yes (injection only), but their benefit depends on Naver's behaviour, which
is unmeasured; (d) exists.

## 4. Honest limits

- Measured: stock behaviour (class A and B), departure/park reverse pattern, end manoeuvre rotations, abrupt boot ends. Assumed: a parked
  car keeps its heading (6 m position agreement over 3.2 h in 1 pair is consistent with it but does not measure heading).
- The spiral home garage: about 500 deg of rotation on the way in and out without GPS. With the BETA heading budget rule
  (0.03 rad + 0.002 rad/s x moving time + 0.10 x accumulated rotation) the stored heading carries about 50 deg there, above any sensible
  injection limit; stationary yaw zero varied 2042-2056 counts in these logs and drifts after engine start (trip6a: -2.5 deg/s mean over
  10 s with all wheels 0), so standstill yaw must never be integrated. G3 in the home garage is therefore tied to G2 (yaw calibration) and
  is mostly NOT solved by storage alone; the outdoor/flat parking case (trip4/6a/6b type) is where storage helps.
- Car moved while off (towing, turntable, rolling): not observed; detectable only by a position check when a fix exists at boot.
- Store staleness: drives without the product (installs of other builds, guard fallback) happened 4 times between logged boots; a chain
  check is mandatory. Age itself is physically irrelevant but bounds unknown events; a cap is a policy choice.
- The stock GPS course at a start from standstill is unreliable (phantom 1-2 km/h with 67-241 deg courses while all wheels read 0; first
  course at 1 km/h = 234 deg in trip4 while the car turned about 90 deg in the next 10 s).
- Naver's own heading source on the phone (compass/IMU at standstill, map heading-up vs north-up) is unknown; the product can change only
  the car-provided LOCATION. If Naver ignores car bearings at standstill, injection while parked has no effect.
- Class A position: the stale LDS position (44-195 m off) is not addressed by a bearing overlay; replacing it is forbidden by
  BETA_DECISIONS 1 and is a non-goal of G3.

## 5. Minimal design

State machine (worker thread; new component BootHeading, BETA domain, never ASSIST):
- RESTORE (once at worker start): read state/heading.v1. Valid only if magic/version/CRC32 pass, state = STOPPED, budget <= 30 deg, the
  guard's persist-state shows the previous boot as confirmed (`previous=confirmed`; semantics to be verified in implementation), the
  store's boot_id differs from the current one, and the age (store utc vs first GPS utc, when one exists) <= 7 days. Otherwise
  INVALID(reason).
- HELD: no wheel motion since boot. Heading = stored, budget = stored. If a fix exists, compare with the stored position: > 50 m ->
  INVALID(moved). Sends: overlay bearing (and speed 0) only in BETA states where the original is sent today (FIX, NO_FIX); position and
  accuracy stay original.
- TRACKING: wheels moving. Heading = stored + integral of yaw (counts/count, current zero rule, integration only while a wheel moves);
  travel = heading + 180 deg when 0x118 = 1; reverse unknown -> forward, journaled. Budget grows by the BETA rule.
- END: first M-accepted GPS course (journal the error), budget > 30 deg, 10 min, or any fence/fault. Afterwards the existing pipeline is
  the only heading source.

Persistence: one 128-byte text line `mx5hd1 seq boot_id utc mono_ns state heading_cdeg budget_cdeg lat_e7 lon_e7 pos_src crc32`,
written by the journal writer thread (never the OEM path): write temp, fsync, renameat, directory fsync. Write at each confirmed stop
(the existing 1.5 s stop confirmation) when heading or budget changed, at most once per 5 s; write state = MOVING once after >= 2 m of
motion so a reset while driving cannot leave a valid STOPPED record. The installer and uninstaller delete the file. Failures are journaled
and never block.

Journal rows: `heading_store` (seq, state, heading, budget, result, latency_ms), `heading_restore` (result, reason, age_s, chain,
distance_to_first_fix_m), `heading_seed_end` (reason, seconds and metres since session start, error_deg vs the first M course, stock
bearing at session start), and a counter of overlaid sends in beta_summary.

Tests and replay: store encode/decode, CRC, truncated/old-version files, rename fault injection; restore rules (chain, age, budget, moved,
same boot); BootHeading (held, yaw integration gated by wheels, reverse known/unknown, budget end, M handover); adapter overlay changes
only bytes 32/36/40/44; replay: synthetic store from the trip6a end fed into the trip6b start (expect about 197 deg instead of 85-114/0),
trip4 end garage manoeuvre (expect about 345 deg with the zero uncertainty journaled), trip3 class A (budget refusal through the spiral).

Effort: log-only phase (store, restore, BootHeading, journal, analyzer, tests) about 2 working days; injection (adapter choice,
controller wiring, replay, DHU follow-up) about 1-2 more.

## 6. Recommendation

1. Build now in LOG-ONLY form: persist, restore and compute the would-be bearing every boot and journal its error against the first
   M-accepted GPS course and the stock bearing, without changing any send. It adds only a small file write and rides along the next
   owner-approved beta drive; no extra trip is requested.
2. Run a DHU experiment (EXP10, no vehicle) before enabling injection. Fresh AA session and fresh Naver process per run; stationary fix
   (accuracy 3 m, speed 0) for 60 s, then motion along a route that starts with a 90 deg turn. Conditions: C0 bearing 0 (stock), C1 true
   heading, C2 true + 180, C3 true + 90, C4 GPS-noise bearings 60-120 deg with 0.3-0.6 m/s and 10 m wander (trip6b copy), C5 stale
   position 100 m off with bearing 335 and wheel speed (class A, stock beta), C6 like C5 with the yaw-integrated true heading. Measure
   from photos every 2 s: arrow angle and map rotation (compass indicator) vs north and vs the road, arrow colour, guidance text (wrong
   direction, U-turn, reroute) and the time/distance after motion until the arrow aligns with the route; optionally rotate the phone
   during C1 to test whether Naver uses the phone compass. Decision: enable injection only if C1 shows the true direction while C0/C4 do
   not, and C2/C3 are no worse than C0; if all conditions look alike at standstill, inject only during TRACKING (moving before a course)
   or drop G3 injection.
3. Not worth pursuing: carrying the last GPS course (83-108 deg wrong after a garage manoeuvre) or using the LDS stored record.
