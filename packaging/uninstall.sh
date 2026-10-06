#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
ALLOW_REMOUNT=1
[ -z "$ROOT" ] || ALLOW_REMOUNT=0
for arg in "$@"; do case "$arg" in --remount) ALLOW_REMOUNT=1;; --no-remount) ALLOW_REMOUNT=0;; *) fail "Unknown option $arg";; esac; done
[ -z "$ROOT" ] || [ "$ALLOW_REMOUNT" = 0 ] || fail 'No remounts permitted for fixtures'
prepare_storage
mount_rw "$ROOT/jci/sm"
mount_rw "$ROOT/usr/bin"
rm -f "$BASE/guard/arm" "$BASE/guard/persist"
sync
file=$ROOT/usr/bin/autostart
regular "$file"
autostart_before=$(hash "$file")
cp -p "$file" "$file.mx5dr-remove.$$"
awk -v action=remove -f "$HERE/edit_autostart.awk" "$file" > "$file.mx5dr-remove.$$" || fail 'Unsupported autostart; arm removed, inspect manually'
sh -n "$file.mx5dr-remove.$$" || fail 'Invalid autostart shell'
TARGETS=''
# Recovery is allowed after firmware changes: removal has no firmware gate.
# Inspect both pinned files, removing exact owned tokens in jciAAPA/jciVBS/jciLDS.
for name in sm.conf sm_WCP.conf; do
    file=$ROOT/jci/sm/$name
    [ -e "$file" ] || continue
    regular "$file"
    before=$(hash "$file")
    edit_to "$file" "$file.mx5dr-remove.$$" remove
    [ "$(hash "$file")" = "$before" ] || fail 'Concurrent service configuration edit'
    printf '%s\n' "$before" > "$BASE/$name.remove-before"
    TARGETS="$TARGETS $name"
done
MODE=OFF; set_config
# Make the staged contents durable before replacing any OEM directory entry.
sync
for name in $TARGETS; do
    file=$ROOT/jci/sm/$name
    before=$(cat "$BASE/$name.remove-before")
    [ "$(hash "$file")" = "$before" ] || fail 'Concurrent service configuration edit'
    if [ "$(hash "$file.mx5dr-remove.$$")" = "$before" ]; then
        rm -f "$file.mx5dr-remove.$$"
    else
        mv -f "$file.mx5dr-remove.$$" "$file"
    fi
    rm -f "$BASE/$name.remove-before"
done
[ "$(hash "$ROOT/usr/bin/autostart")" = "$autostart_before" ] || fail 'Concurrent autostart edit'
if [ "$(hash "$ROOT/usr/bin/autostart.mx5dr-remove.$$")" = "$autostart_before" ]; then
    rm -f "$ROOT/usr/bin/autostart.mx5dr-remove.$$"
else
    mv -f "$ROOT/usr/bin/autostart.mx5dr-remove.$$" "$ROOT/usr/bin/autostart"
fi
rm -f "$BASE/pending" "$BASE/installed.txt"
sync
echo 'Removed owned one-boot autostart blocks, the persistent enablement and mx5dr preload tokens; OFF config staged. No restart/kill. Library/logs/backups retained for mapped-code lifetime and diagnosis.'
