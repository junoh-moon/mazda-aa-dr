#!/bin/sh
# Explicitly authorize one future guarded boot. Never restart CMU services.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
ALLOW_REMOUNT=1
[ -z "$ROOT" ] || ALLOW_REMOUNT=0
MODE=OBSERVE
for arg in "$@"; do case "$arg" in --remount) ALLOW_REMOUNT=1;; --mode=OBSERVE|--mode=SCRUB|--mode=SHADOW) MODE=${arg#--mode=};; *) fail "Unknown option $arg";; esac; done
[ -z "$ROOT" ] || [ "$ALLOW_REMOUNT" = 0 ] || fail 'No remounts permitted for fixtures'
verify_firmware
require_trial_space 0
prepare_collector_storage
[ ! -e "$BASE/pending" ] || fail 'Pending installation transaction'
[ -d "$BASE/guard" ] && [ ! -L "$BASE/guard" ] || fail 'Missing guard directory'
regular "$BASE/guard/mx5dr-guard"
regular "$BASE/libmx5dr-vimtap.so"
regular "$BASE/libmx5dr-ldstap.so"
# Disarm first: interrupted template refresh must not retain a previous authorization.
rm -f "$BASE/guard/arm"
clear_capture_markers
sync
set_config
for pair in 'sm.conf normal.trial' 'sm_WCP.conf wcp.trial'; do
    set -- $pair
    regular "$ROOT/jci/sm/$1"
    # Any old persistent experimental token is an installation error, not silently accepted.
    ! grep -F "$TOKEN" "$ROOT/jci/sm/$1" >/dev/null || fail 'Persistent preload token remains; reinstall one-boot package'
    ! grep -F "$TAP_TOKEN" "$ROOT/jci/sm/$1" >/dev/null || fail 'Persistent VBS tap token remains; reinstall one-boot package'
    ! grep -F "$LDS_TOKEN" "$ROOT/jci/sm/$1" >/dev/null || fail 'Persistent LDS tap token remains; reinstall one-boot package'
    snapshot=$BASE/guard/$1.source.new.$$
    cp -p "$ROOT/jci/sm/$1" "$snapshot"
    trial_to "$snapshot" "$BASE/guard/$2.new.$$"
    hash "$snapshot" > "$BASE/guard/${2%.trial}.source.sha256.new.$$"
    chmod 0600 "$BASE/guard/${2%.trial}.source.sha256.new.$$"
    rm -f "$snapshot"
done
for pair in 'sm.conf normal' 'sm_WCP.conf wcp'; do
    set -- $pair
    [ "$(hash "$ROOT/jci/sm/$1")" = "$(cat "$BASE/guard/$2.source.sha256.new.$$")" ] || fail "Concurrent edit: $1"
done
# Publish the pair only while disarmed. The guard checks the saved source
# identity again, closing the interval between this shell comparison and arm.
for trial in normal wcp; do
    mv -f "$BASE/guard/$trial.trial.new.$$" "$BASE/guard/$trial.trial"
    mv -f "$BASE/guard/$trial.source.sha256.new.$$" "$BASE/guard/$trial.source.sha256"
done
sync
[ -n "$ROOT" ] || "$BASE/guard/mx5dr-guard" arm || fail 'Arm command failed; inspect guard status before reboot'
echo "Armed one future boot for $MODE. Same-boot repeat trial remains blocked. No restart."
