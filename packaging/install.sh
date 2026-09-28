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
prepare_storage
[ ! -e "$BASE/pending" ] || fail 'Incomplete transaction: run uninstall.sh before retrying'
TARGETS=sm.conf
[ "$WITH_WCP" = 0 ] || TARGETS="$TARGETS sm_WCP.conf"
mount_rw "$ROOT/jci/sm"
TX=$BASE/backups/$(date +%Y%m%dT%H%M%S)-$$
mkdir "$TX"
# Stage and validate every selected config before touching any launcher.
for name in $TARGETS; do
    file=$ROOT/jci/sm/$name
    regular "$file"
    cp -p "$file" "$TX/$name.before"
    edit_to "$file" "$file.mx5dr-new.$$" add
    hash "$file" > "$TX/$name.before.sha256"
done
# Never truncate a mapped library. Atomic replacement leaves the old inode alive.
cp "$HERE/libmx5dr.so" "$BASE/libmx5dr.so.new.$$"
chmod 0644 "$BASE/libmx5dr.so.new.$$"
mv -f "$BASE/libmx5dr.so.new.$$" "$BASE/libmx5dr.so"
cp "$HERE/mx5dr-collector" "$BASE/mx5dr-collector.new.$$"
chmod 0755 "$BASE/mx5dr-collector.new.$$"
mv -f "$BASE/mx5dr-collector.new.$$" "$BASE/mx5dr-collector"
set_config
[ ! -L "$BASE/tools" ] || fail 'Symlink tools directory'
mkdir -p "$BASE/tools"
for name in common.sh edit_service.awk uninstall.sh export_logs.sh start_collector.sh stop_collector.sh firmware.sha256; do
    cp "$HERE/$name" "$BASE/tools/$name.new.$$"
    chmod 0644 "$BASE/tools/$name.new.$$"
    mv -f "$BASE/tools/$name.new.$$" "$BASE/tools/$name"
done
printf '%s\n' "$TARGETS" > "$BASE/pending"
sync
for name in $TARGETS; do
    file=$ROOT/jci/sm/$name
    [ "$(hash "$file")" = "$(cat "$TX/$name.before.sha256")" ] || fail "Concurrent edit: $name; run uninstall to remove only our token"
    mv -f "$file.mx5dr-new.$$" "$file"
done
printf 'mode=%s\ntargets=%s\nbackup=%s\npayload_sha256=%s\n' "$MODE" "$TARGETS" "$TX" "$want" > "$BASE/installed.txt.new.$$"
mv -f "$BASE/installed.txt.new.$$" "$BASE/installed.txt"
rm -f "$BASE/pending"
sync
echo "Staged $MODE for next normal service start. No processes restarted. Media may be removed."
