#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
MODE=OBSERVE
# Maintainer-selected trial default, never shell-sourced and never ASSIST.
# BETA (SHADOW capture plus the MODEL-domain replacement) is accepted only as
# an explicit bundle default or option; it is never the unmarked default.
# Validate even when an explicit CLI mode overrides it; reject before writes.
if [ -e "$HERE/bundle-default-mode" ] || [ -L "$HERE/bundle-default-mode" ]; then
    regular "$HERE/bundle-default-mode"
    [ "$(wc -c < "$HERE/bundle-default-mode")" -le 8 ] || fail 'Invalid bundle default mode'
    MODE=$(awk 'NR==1 && ($0=="OBSERVE" || $0=="SHADOW" || $0=="BETA") {mode=$0}
        END {if(NR!=1 || mode=="") exit 1; print mode}' "$HERE/bundle-default-mode") || fail 'Invalid bundle default mode'
    case "$MODE" in OBSERVE|SHADOW|BETA) ;; *) fail 'Invalid bundle default mode';; esac
fi
WITH_WCP=0
# BETA is the v1.0 product: installed PERSISTENT, decided again by the guard on
# every boot with an automatic fail-safe. --one-boot keeps the earlier single
# guarded trial for BETA; OBSERVE/SCRUB/SHADOW are always one-boot trials.
FORCE_ONE_BOOT=0
# Running the installer authorizes the temporary writes it needs. Restore every
# mount we changed in common.sh's exit trap, including on installation failure.
ALLOW_REMOUNT=1
[ -z "$ROOT" ] || ALLOW_REMOUNT=0 # Fixtures must never remount the host.
for arg in "$@"; do
    case "$arg" in
      --with-wcp) WITH_WCP=1;;
      --remount) ALLOW_REMOUNT=1;;
      --no-remount) ALLOW_REMOUNT=0;;
      --one-boot) FORCE_ONE_BOOT=1;;
      --mode=OBSERVE|--mode=SCRUB|--mode=SHADOW|--mode=BETA|--mode=OFF) MODE=${arg#--mode=};;
      *) fail "Unknown option $arg (ASSIST is not deployable)";;
    esac
done
[ -z "$ROOT" ] || [ "$ALLOW_REMOUNT" = 0 ] || fail 'No remounts permitted for fixtures'
POLICY=one-boot
if [ "$MODE" = BETA ] && [ "$FORCE_ONE_BOOT" = 0 ]; then POLICY=persistent; fi
if [ "$MODE" != OFF ]; then prepare_arm_boot; fi
# Published bundles carry a full manifest; developer bundles still use the
# mandatory per-binary hashes below. Do not require a separate user command.
if [ -e "$HERE/SHA256SUMS" ] || [ -L "$HERE/SHA256SUMS" ]; then
    verify_bundle_manifest
fi
regular "$HERE/mx5dr-sha256"
regular "$HERE/mx5dr-sha256.sha256"
[ "$(hash "$HERE/mx5dr-sha256")" = "$(awk 'NR==1{print $1}' "$HERE/mx5dr-sha256.sha256")" ] || fail 'Hash helper checksum mismatch'
verify_firmware
regular "$HERE/libmx5dr.so"
regular "$HERE/libmx5dr.so.sha256"
[ "$(wc -c < "$HERE/libmx5dr.so")" -ge 52 ] || fail 'Truncated ELF header'
want=$(awk 'NR==1{print $1}' "$HERE/libmx5dr.so.sha256")
[ "$(hash "$HERE/libmx5dr.so")" = "$want" ] || fail 'Payload checksum mismatch'
# ELF magic, 32-bit, little-endian, current version; ET_DYN and EM_ARM.
magic=$(od -An -t u1 -N 7 "$HERE/libmx5dr.so" | tr -s ' ' | sed 's/^ //;s/ $//')
arch=$(od -An -t u1 -j 16 -N 4 "$HERE/libmx5dr.so" | tr -s ' ' | sed 's/^ //;s/ $//')
[ "$magic" = '127 69 76 70 1 1 1' ] && [ "$arch" = '3 0 40 0' ] || fail 'Payload must be little-endian ARM32 ELF shared object'
regular "$HERE/libmx5dr-vimtap.so"
regular "$HERE/libmx5dr-vimtap.so.sha256"
[ "$(wc -c < "$HERE/libmx5dr-vimtap.so")" -ge 52 ] || fail 'Truncated VBS tap ELF header'
tap_want=$(awk 'NR==1{print $1}' "$HERE/libmx5dr-vimtap.so.sha256")
[ "$(hash "$HERE/libmx5dr-vimtap.so")" = "$tap_want" ] || fail 'VBS tap checksum mismatch'
magic=$(od -An -t u1 -N 7 "$HERE/libmx5dr-vimtap.so" | tr -s ' ' | sed 's/^ //;s/ $//')
arch=$(od -An -t u1 -j 16 -N 4 "$HERE/libmx5dr-vimtap.so" | tr -s ' ' | sed 's/^ //;s/ $//')
[ "$magic" = '127 69 76 70 1 1 1' ] && [ "$arch" = '3 0 40 0' ] || fail 'VBS tap must be little-endian ARM32 ELF shared object'
regular "$HERE/libmx5dr-ldstap.so"
regular "$HERE/libmx5dr-ldstap.so.sha256"
[ "$(wc -c < "$HERE/libmx5dr-ldstap.so")" -ge 52 ] || fail 'Truncated LDS tap ELF header'
lds_want=$(awk 'NR==1{print $1}' "$HERE/libmx5dr-ldstap.so.sha256")
[ "$(hash "$HERE/libmx5dr-ldstap.so")" = "$lds_want" ] || fail 'LDS tap checksum mismatch'
magic=$(od -An -t u1 -N 7 "$HERE/libmx5dr-ldstap.so" | tr -s ' ' | sed 's/^ //;s/ $//')
arch=$(od -An -t u1 -j 16 -N 4 "$HERE/libmx5dr-ldstap.so" | tr -s ' ' | sed 's/^ //;s/ $//')
[ "$magic" = '127 69 76 70 1 1 1' ] && [ "$arch" = '3 0 40 0' ] || fail 'LDS tap must be little-endian ARM32 ELF shared object'
regular "$HERE/mx5dr-collector"
regular "$HERE/mx5dr-collector.sha256"
[ "$(wc -c < "$HERE/mx5dr-collector")" -ge 52 ] || fail 'Truncated collector ELF header'
collector_want=$(awk 'NR==1{print $1}' "$HERE/mx5dr-collector.sha256")
[ "$(hash "$HERE/mx5dr-collector")" = "$collector_want" ] || fail 'Collector checksum mismatch'
magic=$(od -An -t u1 -N 7 "$HERE/mx5dr-collector" | tr -s ' ' | sed 's/^ //;s/ $//')
arch=$(od -An -t u1 -j 16 -N 4 "$HERE/mx5dr-collector" | tr -s ' ' | sed 's/^ //;s/ $//')
[ "$magic" = '127 69 76 70 1 1 1' ] || fail 'Collector must be little-endian ARM32 ELF'
case "$arch" in '2 0 40 0'|'3 0 40 0') ;; *) fail 'Collector must be ARM32 executable';; esac
regular "$HERE/mx5dr-guard"
regular "$HERE/mx5dr-guard.sha256"
[ "$(hash "$HERE/mx5dr-guard")" = "$(awk 'NR==1{print $1}' "$HERE/mx5dr-guard.sha256")" ] || fail 'Guard checksum mismatch'
# Same target ABI envelope as the experimental library; never run host helper on CMU.
magic=$(od -An -t u1 -N 7 "$HERE/mx5dr-guard" | tr -s ' ' | sed 's/^ //;s/ $//')
arch=$(od -An -t u1 -j 18 -N 2 "$HERE/mx5dr-guard" | tr -s ' ' | sed 's/^ //;s/ $//')
[ "$magic" = '127 69 76 70 1 1 1' ] && [ "$arch" = '40 0' ] || fail 'Guard must be ARM32 little-endian ELF'
# Do not replace a DSO still referenced by a legacy permanent installation.
# Its removal is a separate, inspectable operation before this one-boot install.
for name in sm.conf sm_WCP.conf; do
    regular "$ROOT/jci/sm/$name"
    ! grep -F "$TOKEN" "$ROOT/jci/sm/$name" >/dev/null || fail 'Legacy persistent mx5dr preload found; run uninstall.sh first, then install one-boot package'
    ! grep -F "$TAP_TOKEN" "$ROOT/jci/sm/$name" >/dev/null || fail 'Persistent VBS tap preload found; run uninstall.sh first, then install one-boot package'
    ! grep -F "$LDS_TOKEN" "$ROOT/jci/sm/$name" >/dev/null || fail 'Persistent LDS tap preload found; run uninstall.sh first, then install one-boot package'
done
payload_bytes=0
for name in libmx5dr.so libmx5dr-vimtap.so libmx5dr-ldstap.so mx5dr-collector mx5dr-guard mx5dr-sha256; do
    payload_bytes=$((payload_bytes + $(wc -c < "$HERE/$name")))
done
require_trial_space "$payload_bytes"
prepare_collector_storage
[ ! -e "$BASE/pending" ] || fail 'Incomplete transaction: run uninstall.sh before retrying'
mount_rw "$ROOT/jci/sm"
mount_rw "$ROOT/usr/bin"
[ ! -L "$BASE/guard" ] || fail 'Symlink guard directory'
mkdir -p "$BASE/guard"
chmod 0700 "$BASE/guard"
if [ -z "$ROOT" ]; then chown 0 "$BASE" "$BASE/guard"; fi
# A retry must not reuse the old trial's boot marker if later staging fails.
if [ "$MODE" != OFF ]; then stash_arm_boot; fi
# Menu 1 resets the persistent state (guard enable) and acknowledges a runtime
# disable marker. Keep the previous files first: numbered, no clock, last 3.
if [ "$POLICY" = persistent ]; then
    evidence=''
    for item in "$BASE/guard/persist-state" "$BASE/guard/last-decision" "$BASE/logs/disable-next-start"; do
        if [ -e "$item" ] || [ -L "$item" ]; then regular "$item"; evidence="$evidence $item"; fi
    done
    if [ -n "$evidence" ]; then
        previous_trip=unknown
        guard_exec=$BASE/guard/mx5dr-guard
        if [ -n "$ROOT" ]; then guard_exec=${MX5DR_FIXTURE_GUARD:-$ROOT/missing-guard}; fi
        if [ -e "$BASE/guard/persist" ] && [ -f "$guard_exec" ] && [ -x "$guard_exec" ]; then
            previous_trip=$(MX5DR_GUARD_ROOT=$ROOT "$guard_exec" status 2>/dev/null |
                awk -F= '$1=="persist" {p=$2} $1=="tripped" {t=$2} END {if(p=="tripped") print t; else if(p!="") print "no"; else print "unknown"}') ||
                previous_trip=unknown
            case "$previous_trip" in ''|*[!a-z_]*) previous_trip=unknown;; esac
        fi
        kept=$BASE/backups/persist-evidence
        [ ! -L "$kept" ] || fail 'Symlink evidence directory'
        mkdir -p "$kept"
        number=0
        for entry in "$kept"/[0-9]*; do
            name=${entry##*/}
            case "$name" in *[!0-9]*) continue;; esac
            [ -d "$entry" ] && [ ! -L "$entry" ] || continue
            [ "$name" -le "$number" ] || number=$name
        done
        number=$((number + 1))
        mkdir "$kept/$number"
        for item in $evidence; do cp -p "$item" "$kept/$number/" || fail 'Cannot keep previous BETA evidence'; done
        sync
        for entry in "$kept"/[0-9]*; do
            name=${entry##*/}
            case "$name" in *[!0-9]*) continue;; esac
            if [ -d "$entry" ] && [ ! -L "$entry" ] && [ "$name" -le $((number - 3)) ]; then rm -rf "$entry"; fi
        done
        echo "Kept the previous BETA state in backups/persist-evidence/$number (tripped: $previous_trip)."
        if [ "$previous_trip" != no ] && [ "$previous_trip" != unknown ]; then
            echo "BETA was tripped ($previous_trip). Run menu 3 export first if you have not; menu 1 now re-enables BETA."
        fi
    fi
fi
# No old arm or persistent enablement may survive a partial replacement.
rm -f "$BASE/guard/arm" "$BASE/guard/persist"
clear_capture_markers
if [ "$POLICY" = persistent ]; then
    # A failure from here on must not leave an enablement behind.
    PERSIST_PENDING=1
    # Re-enabling is the owner's acknowledgement of a runtime self-disable
    # (kept above with the previous state).
    if [ -e "$BASE/logs/disable-next-start" ] || [ -L "$BASE/logs/disable-next-start" ]; then
        regular "$BASE/logs/disable-next-start"
        rm -f "$BASE/logs/disable-next-start" || fail 'Cannot clear the runtime disable marker'
        echo 'Cleared the runtime disable marker (disable-next-start) for re-enable.'
    fi
fi
sync
TX=$BASE/backups/$(date +%Y%m%dT%H%M%S)-$$
mkdir "$TX"
# Both persistent configs remain clean; the actual board-selected path is gated at boot.
TARGETS='sm.conf sm_WCP.conf'
for name in $TARGETS; do
    file=$ROOT/jci/sm/$name
    regular "$file"
    cp -p "$file" "$TX/$name.before"
    case "$name" in sm.conf) trial=normal.trial;; sm_WCP.conf) trial=wcp.trial;; esac
    trial_to "$TX/$name.before" "$BASE/guard/$trial.new.$$"
    hash "$TX/$name.before" > "$TX/$name.before.sha256"
    cp "$TX/$name.before.sha256" "$BASE/guard/${trial%.trial}.source.sha256.new.$$"
    chmod 0600 "$BASE/guard/${trial%.trial}.source.sha256.new.$$"
done
file=$ROOT/usr/bin/autostart
regular "$file"
cp -p "$file" "$TX/autostart.before"
hash "$TX/autostart.before" > "$TX/autostart.before.sha256"
cp -p "$TX/autostart.before" "$file.mx5dr-new.$$"
awk -v action=add -f "$HERE/edit_autostart.awk" "$TX/autostart.before" > "$file.mx5dr-new.$$" || fail 'Unsupported autostart anchors'
sh -n "$file.mx5dr-new.$$" || fail 'Invalid staged autostart shell'
# Never truncate mapped objects. Guard/config are UID 0 owned; the separate
# collector's existing account owns logs (service when cmu itself is UID 0).
for name in libmx5dr.so libmx5dr-vimtap.so libmx5dr-ldstap.so mx5dr-guard mx5dr-collector; do
    dest=$BASE/$name
    [ "$name" != mx5dr-guard ] || dest=$BASE/guard/$name
    cp "$HERE/$name" "$dest.new.$$"
    chmod 0755 "$dest.new.$$"
    if [ -z "$ROOT" ]; then chown 0 "$dest.new.$$"; fi
    mv -f "$dest.new.$$" "$dest"
done
set_config
[ ! -L "$BASE/tools" ] || fail 'Symlink tools directory'
mkdir -p "$BASE/tools"
chmod 0755 "$BASE/tools"
if [ -z "$ROOT" ]; then chown 0 "$BASE/tools"; fi
for name in common.sh edit_service.awk edit_autostart.awk arm.sh uninstall.sh export_logs.sh start_collector.sh stop_collector.sh finish_capture.sh trial_status.sh trial_status.awk startup_diagnostics.sh firmware.sha256 mx5dr-sha256; do
    cp "$HERE/$name" "$BASE/tools/$name.new.$$"
    chmod 0644 "$BASE/tools/$name.new.$$"
    if [ -z "$ROOT" ]; then chown 0 "$BASE/tools/$name.new.$$"; fi
    mv -f "$BASE/tools/$name.new.$$" "$BASE/tools/$name"
done
# Publish local templates while disarmed, and exercise the real guard against
# the real filesystem/libc before changing the OEM startup script.
mv -f "$BASE/guard/normal.trial.new.$$" "$BASE/guard/normal.trial"
mv -f "$BASE/guard/wcp.trial.new.$$" "$BASE/guard/wcp.trial"
mv -f "$BASE/guard/normal.source.sha256.new.$$" "$BASE/guard/normal.source.sha256"
mv -f "$BASE/guard/wcp.source.sha256.new.$$" "$BASE/guard/wcp.source.sha256"
if [ -z "$ROOT" ]; then
    "$BASE/guard/mx5dr-guard" check || fail 'Guard preflight failed; autostart not changed'
fi
printf '%s\n' "$POLICY install: baseline configs and autostart" > "$BASE/pending"
sync
for name in $TARGETS; do
    file=$ROOT/jci/sm/$name
    [ "$(hash "$file")" = "$(cat "$TX/$name.before.sha256")" ] || fail "Concurrent edit: $name"
done
file=$ROOT/usr/bin/autostart
[ "$(hash "$file")" = "$(cat "$TX/autostart.before.sha256")" ] || fail 'Concurrent autostart edit'
mv -f "$file.mx5dr-new.$$" "$file"
# Templates bind the current preserved touch settings; later changes decline a trial.
printf 'mode=%s\npolicy=%s\nbackup=%s\npayload_sha256=%s\ntap_sha256=%s\nlds_tap_sha256=%s\n' "$MODE" "$POLICY" "$TX" "$want" "$tap_want" "$lds_want" > "$BASE/installed.txt.new.$$"
mv -f "$BASE/installed.txt.new.$$" "$BASE/installed.txt"
rm -f "$BASE/pending"
sync
if [ -z "$ROOT" ]; then
    if [ "$POLICY" = persistent ]; then
        "$BASE/guard/mx5dr-guard" enable || fail 'Enable command failed; inspect guard status before reboot'
    else
        [ "$MODE" = OFF ] || "$BASE/guard/mx5dr-guard" arm || fail 'Arm command failed; inspect guard status before reboot'
    fi
else
    echo 'Fixture staged only; target guard not executed.'
fi
PERSIST_PENDING=0
if [ "$MODE" != OFF ] && [ "$POLICY" = one-boot ]; then
    if ! record_arm_boot; then
        rm -f "$BASE/guard/arm" || fail 'Arming boot marker failed and arm could not be revoked'
        sync
        fail 'Arming boot marker failed; trial arm revoked. Autostart remains staged but disarmed; use menu 4 to remove it before retrying.'
    fi
fi
ARM_PENDING=0
if [ "$POLICY" = persistent ]; then
    echo "Installed $MODE persistent: the guard starts it on every CMU boot. Persistent service configs retain existing touch only. No processes restarted."
    echo 'Automatic fallback: after 2 boots in a row that end in a CMU reset, the guard starts stock only and keeps the logs. Menu 1 re-enables, menu 4 removes.'
fi
if [ -z "$ROOT" ] && [ "$POLICY" = persistent ]; then
    echo 'Install steps finished. Remain parked with the engine running and this USB connected. Choose trial menu 5 once to start the first product boot; later boots start it automatically.'
    echo 'After CMU restart reopen trial menu 2: check PERSIST enabled, this boot selected yes, ok BETA armed, ok HOOK and ok FENCE.'
    echo 'Then exit the menu and replace the USB with the AA dongle while parked.'
fi
[ "$POLICY" = persistent ] || echo "Staged $MODE for one guarded boot. Persistent service configs retain existing touch only. No processes restarted."
if [ -z "$ROOT" ] && [ "$MODE" != OFF ] && [ "$POLICY" = one-boot ]; then
    echo 'Install steps finished. A vehicle ignition cycle alone does not prove a new CMU Linux boot.'
    echo 'Remain parked with the engine actually running and this USB connected. Choose trial menu 5 to request CMU reboot; do not press the engine start/stop button.'
    echo "The next guarded CMU startup requests automatic $MODE capture; no driving-time commands are needed."
    echo "After CMU restart reopen trial menu 2: check reboot_check=new_boot_observed, startup_state=guard_committed_after_new_boot, one_boot=consumed_this_boot, config_mode=$MODE, runtime_disable_next_start=absent, retained_bytes>0 and collector_poll_recent=observed."
    if [ "$MODE" = BETA ]; then
        echo 'BETA: SHADOW capture plus a dead-reckoned LOCATION only while the original reports no GPS fix. Menu 2 must also show ok BETA armed, ok HOOK and ok FENCE.'
    fi
    echo 'Then exit the menu and replace the USB with the AA dongle while parked, keeping the same engine/CMU boot.'
fi
