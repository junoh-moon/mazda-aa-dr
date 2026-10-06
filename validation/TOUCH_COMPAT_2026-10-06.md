# v1.0.0-beta.2 persistent BETA vs the installed oem-aa-mod touch patch (2026-10-06)

Scope: offline replica only. No vehicle, phone, dongle or CMU was used. The bundle is the
published v1.0.0-beta.2 ZIP content (source commit eb795d4), unpacked and unmodified.

## Inputs

- Owner's vehicle state, taken from the private shadow.5 export (not copied here):
  `/jci/sm/sm.conf`, `/jci/sm/sm_WCP.conf`, `/usr/bin/autostart` (current, with the
  shadow.5 one-boot blocks) and its pre-mx5dr backup `autostart.before`, `/jci/version.ini`,
  the exported `/data_persist/mx5-aa-dr` tree (one-boot guard with `consumed`, `last-boot`,
  `armed-boot`, SHADOW config, retained logs), `libpatch.conf`, the shadow.5 trial
  `sm.conf` the guard really generated, and the shadow.5 boot row `install_diag`.
- What the touch mod changes on this vehicle: relative to stock, exactly three added
  `LD_PRELOAD` lines in each of `sm.conf` and `sm_WCP.conf` (aap_service →
  `libpatch-aap_service.so`, jciAAPA → `libpatch-blmjciaapa.so`, jcinavi →
  `libpatch-svcjcinavi.so`). The pre-mx5dr autostart and `version.ini` are byte-identical to
  stock; the touch mod has no autostart edit.
- The vehicle `libpatch.conf` is byte-identical to the template shipped in oem-aa-mod 0.9.0
  and 0.9.1 (not 0.10.0). The DSO version on the vehicle was not exported; 0.9.1 DSOs were
  used in the replica.
- The board is the normal (non-WCP) branch: the vehicle SM ran the trial derived from
  `sm.conf`.

## Method

Replica root from the stock tree (BusyBox 1.19.2, libc, firmware-hashed files) with the
vehicle files written over the stock copies, `/data_persist/oem-aa-mod` populated with the
0.9.1 DSOs and the vehicle `libpatch.conf`. Driven under proot + qemu-arm through the real
USB `trial` menu; the installed ARM guard ran through the installed owned autostart blocks
for both `SMCFG_NORMALMODE` and `SMCFG_WCPMODE` (collector disabled, confirm delay 3 s
instead of 90 s, SM `/proc` cmdline authored). No OEM SM/AA/VBS process was executed.

- Scenario A: vehicle files with the pre-mx5dr autostart and no mx5dr state. 76/76 checks.
- Scenario B: upgrade from the exported shadow.5 state (current autostart, guard
  `consumed`, previous `armed-boot`, retained logs), installed in a new boot. 77/77 checks.

## Results (both scenarios unless noted)

1. Generated trial `sm.conf` (normal branch) is byte-identical to the shadow.5 trial the
   vehicle generated. Diff against the vehicle baseline: +1 line jciVBS
   `libmx5dr-vimtap.so`, jciAAPA value changed to
   `/data_persist/mx5-aa-dr/libmx5dr.so:/data_persist/oem-aa-mod/libpatch-blmjciaapa.so`,
   +1 line jciLDS `libmx5dr-ldstap.so`. Removing exactly these three tokens gives the vehicle
   file byte for byte. aap_service and jcinavi libpatch preloads unchanged; every service has
   at most one `environ_var LD_PRELOAD`.
2. WCP trial: the same three changes against the vehicle `sm_WCP.conf`; all WCP arguments and
   the other lines unchanged (same byte-identity check).
3. `/jci/sm/sm.conf`, `/jci/sm/sm_WCP.conf` and the oem-aa-mod folder stayed byte-identical
   after install, 6 product boots (normal + WCP), the reset trip and menu 4 uninstall.
4. Installed autostart = vehicle pre-mx5dr autostart + two owned blocks (before the WCP and
   normal `taskset ... sm -f` lines); removing the blocks gives the vehicle file byte for byte.
   Scenario B replaces the shadow.5 blocks with the same result; the installed autostart and
   templates are identical in A and B. Menu 4 restores the pre-mx5dr autostart byte for byte
   (in B, not the shadow.5 variant).
5. Later edit of `sm.conf` by the touch mod: an identical rewrite keeps the product; a content
   change shows `PERSIST enabled but bindings changed` / `sm.conf edited by another tool:
   run menu 1` in menu 2 in the same boot, the next boot runs the edited stock config
   (reason `baseline_edited:sm.conf` shown), and menu 1 re-enables with the edited libpatch
   chain kept after our token. An edit of only `sm_WCP.conf` also turns the product off on a
   normal board (both files are bound), and menu 2 names `sm_WCP.conf`.
6. Upgrade differences (B): the stale one-boot `consumed` marker stays in the guard directory
   (no effect on persistent selection; uninstall leaves it too), and menu 2 `DATA` counts the
   retained shadow.5 journal bytes before new BETA data exists.

## AA preload chain with the real BLM and libpatch 0.9.1

`tests/adapter/run_aa_install_probe.sh` with the beta.2 ARM `libmx5dr.so` and oem-aa-mod
0.9.1 `libpatch-blmjciaapa.so`: 4/4 PASS (no patch: observing; libpatch at its known path:
install ok, session observation declined at stage 3 with libpatch as owner; unknown path:
fail-closed; BETA with libpatch: install ok, BETA armed). This matches the shadow.5 vehicle
boot row (`install=ok`, `session_hooks=declined_third_party_interposer`, stage 3 owner
libpatch on `aap_destroy_session`).

## Not verified

Touch input, HUD guidance, mute/pause, media-play blocking and GAL 1.6 behaviour with our
hooks present; libpatch's own D-Bus traffic passing through our jciAAPA libjcidbus wrappers;
the actual DSO version on the vehicle; whole SM startup; phone acceptance. QEMU/replica
success is not vehicle validation.
