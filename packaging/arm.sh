#!/bin/sh
# Explicitly authorize one future guarded boot. Never restart CMU services.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
ALLOW_REMOUNT=0
MODE=OBSERVE
for arg in "$@"; do case "$arg" in --remount) ALLOW_REMOUNT=1;; --mode=OBSERVE|--mode=SCRUB|--mode=SHADOW) MODE=${arg#--mode=};; *) fail "Unknown option $arg";; esac; done
verify_firmware
prepare_storage
[ ! -e "$BASE/pending" ] || fail 'Pending installation transaction'
[ -d "$BASE/guard" ] && [ ! -L "$BASE/guard" ] || fail 'Missing guard directory'
regular "$BASE/guard/mx5dr-guard"
regular "$BASE/libmx5dr-vimtap.so"
# Disarm first: interrupted template refresh must not retain a previous authorization.
rm -f "$BASE/guard/arm"
sync
set_config
for pair in 'sm.conf normal.trial' 'sm_WCP.conf wcp.trial'; do
    set -- $pair
    regular "$ROOT/jci/sm/$1"
    # Any old persistent experimental token is an installation error, not silently accepted.
    ! grep -F "$TOKEN" "$ROOT/jci/sm/$1" >/dev/null || fail 'Persistent preload token remains; reinstall one-boot package'
    ! grep -F "$TAP_TOKEN" "$ROOT/jci/sm/$1" >/dev/null || fail 'Persistent VBS tap token remains; reinstall one-boot package'
    trial_to "$ROOT/jci/sm/$1" "$BASE/guard/$2.new.$$"
    mv -f "$BASE/guard/$2.new.$$" "$BASE/guard/$2"
done
sync
[ -n "$ROOT" ] || "$BASE/guard/mx5dr-guard" arm || fail 'Arm command failed; inspect guard status before reboot'
echo "Armed one future boot for $MODE. Same-boot repeat trial remains blocked. No restart."
