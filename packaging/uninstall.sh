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
# The diagnostic tunnel test (menu 9) never survives an uninstall; a
# directory of that name is not a marker (off) and menu 6 removes it.
if [ -f "$BASE/diag" ] || [ -L "$BASE/diag" ]; then rm -f "$BASE/diag" || echo 'Warning: cannot remove the diag marker; menu 6 removes it.'; fi
# Likewise the tunnel accuracy ceiling off switch (menu 10).
if [ -f "$BASE/acc-cap-off" ] || [ -L "$BASE/acc-cap-off" ]; then rm -f "$BASE/acc-cap-off" || echo 'Warning: cannot remove the acc-cap-off marker; menu 6 removes it.'; fi
# Likewise the G3 garage heading switch (menu 12).
if [ -f "$BASE/g3-garage-off" ] || [ -L "$BASE/g3-garage-off" ]; then rm -f "$BASE/g3-garage-off" || echo 'Warning: cannot remove the g3-garage-off marker; menu 6 removes it.'; fi
# The garage heading chain token: no heading stored before the removal is
# carried after a later installation (install.sh removes it again).
if [ -f "$BASE/logs/g3-last-start" ] || [ -L "$BASE/logs/g3-last-start" ]; then rm -f "$BASE/logs/g3-last-start" || echo 'Warning: cannot remove the garage heading chain token; menu 1 removes it.'; fi
# The phone IMU Bluetooth configuration copy: bds copies it to /tmp only at
# its start, so removing it now changes nothing in this boot; later boots
# never select a trial (stock bds arguments).
remove_bt_config || echo 'Warning: cannot remove the Bluetooth configuration copy; menu 6 removes it.'
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
echo 'Removed owned one-boot autostart blocks, the persistent enablement and mx5dr preload tokens; OFF config staged. No restart/kill. Library/logs/backups retained for mapped-code lifetime and diagnosis. After a CMU reboot (menu 5), menu 6 deletes them all.'
