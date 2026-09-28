#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
MODE=OBSERVE
WITH_WCP=0
ALLOW_REMOUNT=0
for arg in "$@"; do
    case "$arg" in
      --with-wcp) WITH_WCP=1;;
      --remount) ALLOW_REMOUNT=1;;
      --mode=OBSERVE|--mode=SCRUB|--mode=SHADOW|--mode=OFF) MODE=${arg#--mode=};;
      *) fail "Unknown option $arg (ASSIST is not deployable)";;
    esac
done
[ -z "$ROOT" ] || [ "$ALLOW_REMOUNT" = 0 ] || fail 'No remounts permitted for fixtures'
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
done
prepare_storage
[ ! -e "$BASE/pending" ] || fail 'Incomplete transaction: run uninstall.sh before retrying'
mount_rw "$ROOT/jci/sm"
mount_rw "$ROOT/usr/bin"
[ ! -L "$BASE/guard" ] || fail 'Symlink guard directory'
mkdir -p "$BASE/guard"
chmod 0700 "$BASE/guard"
if [ -z "$ROOT" ]; then chown root "$BASE" "$BASE/guard"; fi
# No old arm may survive a partial replacement.
rm -f "$BASE/guard/arm"
sync
TX=$BASE/backups/$(date +%Y%m%dT%H%M%S)-$$
mkdir "$TX"
# Both persistent configs remain clean; the actual board-selected path is gated at boot.
TARGETS='sm.conf sm_WCP.conf'
for name in $TARGETS; do
    file=$ROOT/jci/sm/$name
    regular "$file"
    cp -p "$file" "$TX/$name.before"
    edit_to "$file" "$file.mx5dr-new.$$" remove
    hash "$file" > "$TX/$name.before.sha256"
done
file=$ROOT/usr/bin/autostart
regular "$file"
cp -p "$file" "$TX/autostart.before"
hash "$file" > "$TX/autostart.before.sha256"
cp -p "$file" "$file.mx5dr-new.$$"
awk -v action=add -f "$HERE/edit_autostart.awk" "$file" > "$file.mx5dr-new.$$" || fail 'Unsupported autostart anchors'
sh -n "$file.mx5dr-new.$$" || fail 'Invalid staged autostart shell'
# Never truncate mapped objects. Guard and config are root-owned; logs alone are cmu writable.
for name in libmx5dr.so mx5dr-guard mx5dr-collector; do
    dest=$BASE/$name
    [ "$name" != mx5dr-guard ] || dest=$BASE/guard/$name
    cp "$HERE/$name" "$dest.new.$$"
    chmod 0755 "$dest.new.$$"
    if [ -z "$ROOT" ]; then chown root "$dest.new.$$"; fi
    mv -f "$dest.new.$$" "$dest"
done
set_config
[ ! -L "$BASE/tools" ] || fail 'Symlink tools directory'
mkdir -p "$BASE/tools"
chmod 0755 "$BASE/tools"
if [ -z "$ROOT" ]; then chown root "$BASE/tools"; fi
for name in common.sh edit_service.awk edit_autostart.awk arm.sh uninstall.sh export_logs.sh start_collector.sh stop_collector.sh firmware.sha256; do
    cp "$HERE/$name" "$BASE/tools/$name.new.$$"
    chmod 0644 "$BASE/tools/$name.new.$$"
    if [ -z "$ROOT" ]; then chown root "$BASE/tools/$name.new.$$"; fi
    mv -f "$BASE/tools/$name.new.$$" "$BASE/tools/$name"
done
printf '%s\n' 'one-boot install: baseline configs and autostart' > "$BASE/pending"
sync
for name in $TARGETS; do
    file=$ROOT/jci/sm/$name
    [ "$(hash "$file")" = "$(cat "$TX/$name.before.sha256")" ] || fail "Concurrent edit: $name"
    mv -f "$file.mx5dr-new.$$" "$file"
done
file=$ROOT/usr/bin/autostart
[ "$(hash "$file")" = "$(cat "$TX/autostart.before.sha256")" ] || fail 'Concurrent autostart edit'
mv -f "$file.mx5dr-new.$$" "$file"
# Templates bind the current preserved touch settings; later changes decline a trial.
awk -v action=add -v token="$TOKEN" -f "$HERE/edit_service.awk" "$ROOT/jci/sm/sm.conf" > "$BASE/guard/normal.trial"
awk -v action=add -v token="$TOKEN" -f "$HERE/edit_service.awk" "$ROOT/jci/sm/sm_WCP.conf" > "$BASE/guard/wcp.trial"
chmod 0600 "$BASE/guard/normal.trial" "$BASE/guard/wcp.trial"
printf 'mode=%s\npolicy=one-boot\nbackup=%s\npayload_sha256=%s\n' "$MODE" "$TX" "$want" > "$BASE/installed.txt.new.$$"
mv -f "$BASE/installed.txt.new.$$" "$BASE/installed.txt"
rm -f "$BASE/pending"
sync
if [ -z "$ROOT" ]; then
    [ "$MODE" = OFF ] || "$BASE/guard/mx5dr-guard" arm || fail 'Arm command failed; inspect guard status before reboot'
else
    echo 'Fixture staged only; target guard not executed.'
fi
echo "Staged $MODE for one guarded boot. Persistent service configs retain existing touch only. No processes restarted."
