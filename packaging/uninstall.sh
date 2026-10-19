#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
ALLOW_REMOUNT=1
[ -z "$ROOT" ] || ALLOW_REMOUNT=0
for arg in "$@"; do case "$arg" in --remount) ALLOW_REMOUNT=1;; --no-remount) ALLOW_REMOUNT=0;; *) fail "Unknown option $arg";; esac; done
[ -z "$ROOT" ] || [ "$ALLOW_REMOUNT" = 0 ] || fail 'No remounts permitted for fixtures'
# Menu 4 also recovers an interrupted install or uninstall (power loss): an
# install lock no live run owns is removed (reclaim_stale_lock proves that
# with the kernel mount lock), and the pending transaction marker is removed
# at the end like after every removal. Stock files are never restored or
# deleted here; owned blocks and tokens are removed exactly as below.
validate_persist
mount_rw "$persist"
reclaim_stale_lock
prepare_storage
mount_rw "$ROOT/jci/sm"
mount_rw "$ROOT/usr/bin"
rm -f "$BASE/guard/arm" "$BASE/guard/persist"
# The product loaded in this boot keeps running until the reboot (menu 5): ask
# its worker to stop replacing (it drops to observe-only within a second) so
# that choosing 4 and then driving does not leave tunnel replacement active.
# Best effort; menu 1 and the guard clear the marker.
if [ -d "$BASE/logs" ] && [ ! -L "$BASE/logs" ] && [ ! -e "$BASE/logs/capture.stop" ] && [ ! -L "$BASE/logs/capture.stop" ]; then
    mkdir "$BASE/logs/capture.stop" 2>/dev/null || :
fi
# Every owner switch (menus 7-12, the removed diagnostic marker diag) and the
# garage heading chain token files: a later menu 1 starts from the defaults
# and no heading stored before the removal is carried after it. A directory
# of such a name is not a marker file and is left for menu 6.
for name in $OWNER_MARKERS; do
    if [ -d "$BASE/$name" ] && [ ! -L "$BASE/$name" ]; then
        echo "Warning: cannot remove the $name marker; menu 6 removes it."
    else
        remove_entry "$BASE/$name" || echo "Warning: cannot remove the $name marker; menu 6 removes it."
    fi
done
for name in $G3_TOKEN_FILES; do
    if [ -d "$BASE/logs/$name" ] && [ ! -L "$BASE/logs/$name" ]; then
        echo "Warning: cannot remove logs/$name; menu 6 removes it."
    else
        remove_entry "$BASE/logs/$name" || echo "Warning: cannot remove logs/$name; menu 6 removes it."
    fi
done
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
[ "$STALE_LOCK_RECLAIMED" = no ] || echo 'Recovered an interrupted run: choose 5, or 1 to install again.'
echo 'Removed owned one-boot autostart blocks, the persistent enablement, the owner switches and mx5dr preload tokens; OFF config staged. No restart/kill. Library/logs/backups retained for mapped-code lifetime and diagnosis. After a CMU reboot (menu 5), menu 6 deletes them all.'
