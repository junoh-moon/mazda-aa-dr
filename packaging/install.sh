#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
MODE=OBSERVE
# Maintainer-selected trial default, never shell-sourced and never ASSIST.
# Validate even when an explicit CLI mode overrides it; reject before writes.
if [ -e "$HERE/bundle-default-mode" ] || [ -L "$HERE/bundle-default-mode" ]; then
    regular "$HERE/bundle-default-mode"
    [ "$(wc -c < "$HERE/bundle-default-mode")" -le 8 ] || fail 'Invalid bundle default mode'
    MODE=$(awk 'NR==1 && ($0=="OBSERVE" || $0=="SHADOW") {mode=$0}
        END {if(NR!=1 || mode=="") exit 1; print mode}' "$HERE/bundle-default-mode") || fail 'Invalid bundle default mode'
    case "$MODE" in OBSERVE|SHADOW) ;; *) fail 'Invalid bundle default mode';; esac
fi
WITH_WCP=0
# Running the installer authorizes the temporary writes it needs. Restore every
# mount we changed in common.sh's exit trap, including on installation failure.
ALLOW_REMOUNT=1
[ -z "$ROOT" ] || ALLOW_REMOUNT=0 # Fixtures must never remount the host.
for arg in "$@"; do
    case "$arg" in
      --with-wcp) WITH_WCP=1;;
      --remount) ALLOW_REMOUNT=1;;
      --no-remount) ALLOW_REMOUNT=0;;
      --mode=OBSERVE|--mode=SCRUB|--mode=SHADOW|--mode=OFF) MODE=${arg#--mode=};;
      *) fail "Unknown option $arg (ASSIST is not deployable)";;
    esac
done
[ -z "$ROOT" ] || [ "$ALLOW_REMOUNT" = 0 ] || fail 'No remounts permitted for fixtures'
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
done
prepare_collector_storage
[ ! -e "$BASE/pending" ] || fail 'Incomplete transaction: run uninstall.sh before retrying'
mount_rw "$ROOT/jci/sm"
mount_rw "$ROOT/usr/bin"
[ ! -L "$BASE/guard" ] || fail 'Symlink guard directory'
mkdir -p "$BASE/guard"
chmod 0700 "$BASE/guard"
if [ -z "$ROOT" ]; then chown 0 "$BASE" "$BASE/guard"; fi
# No old arm may survive a partial replacement.
rm -f "$BASE/guard/arm"
clear_capture_markers
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
for name in libmx5dr.so libmx5dr-vimtap.so mx5dr-guard mx5dr-collector; do
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
for name in common.sh edit_service.awk edit_autostart.awk arm.sh uninstall.sh export_logs.sh start_collector.sh stop_collector.sh finish_capture.sh trial_status.sh trial_status.awk firmware.sha256 mx5dr-sha256; do
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
printf '%s\n' 'one-boot install: baseline configs and autostart' > "$BASE/pending"
sync
for name in $TARGETS; do
    file=$ROOT/jci/sm/$name
    [ "$(hash "$file")" = "$(cat "$TX/$name.before.sha256")" ] || fail "Concurrent edit: $name"
done
file=$ROOT/usr/bin/autostart
[ "$(hash "$file")" = "$(cat "$TX/autostart.before.sha256")" ] || fail 'Concurrent autostart edit'
mv -f "$file.mx5dr-new.$$" "$file"
# Templates bind the current preserved touch settings; later changes decline a trial.
printf 'mode=%s\npolicy=one-boot\nbackup=%s\npayload_sha256=%s\ntap_sha256=%s\n' "$MODE" "$TX" "$want" "$tap_want" > "$BASE/installed.txt.new.$$"
mv -f "$BASE/installed.txt.new.$$" "$BASE/installed.txt"
rm -f "$BASE/pending"
sync
if [ -z "$ROOT" ]; then
    [ "$MODE" = OFF ] || "$BASE/guard/mx5dr-guard" arm || fail 'Arm command failed; inspect guard status before reboot'
else
    echo 'Fixture staged only; target guard not executed.'
fi
echo "Staged $MODE for one guarded boot. Persistent service configs retain existing touch only. No processes restarted."
if [ -z "$ROOT" ] && [ "$MODE" != OFF ]; then
    echo 'Install steps finished. After successful command exit, power off normally and start again.'
    echo "The next guarded boot collects $MODE logs automatically; no driving-time commands are needed."
fi
