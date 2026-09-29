#!/bin/sh
# Parked cooperative freeze. No signal, process restart, mode change or export.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
[ "$#" = 0 ] || fail 'Usage: sh finish_capture.sh (while parked)'
validate_persist
for directory in "$BASE" "$BASE/logs"; do
    [ -d "$directory" ] && [ ! -L "$directory" ] || fail "Missing or symlink directory: $directory"
done
regular "$ROOT/proc/sys/kernel/random/boot_id"
[ "$(wc -c < "$ROOT/proc/sys/kernel/random/boot_id")" -eq 37 ] || fail 'Invalid current boot ID'
boot_id=$(cat "$ROOT/proc/sys/kernel/random/boot_id")
valid_boot_id "$boot_id" || fail 'Invalid current boot ID'
for name in capture.stop collector.stop; do
    path=$BASE/logs/$name
    if [ -e "$path" ] || [ -L "$path" ]; then
        [ -d "$path" ] && [ ! -L "$path" ] || fail "Non-directory or symlink stop marker: $name"
    fi
done
if [ -e "$BASE/logs/capture.done" ] || [ -L "$BASE/logs/capture.done" ]; then
    regular "$BASE/logs/capture.done"
fi
[ -d "$BASE/logs/capture.stop" ] || mkdir "$BASE/logs/capture.stop" || fail 'Cannot request capture freeze'
sh "$HERE/stop_collector.sh" || fail 'Cannot request collector stop'
# Fixture-only shortened deadlines make rejection tests fast; target always 15s.
limit=15
if [ -n "$ROOT" ]; then
    limit=${MX5DR_FIXTURE_FINISH_WAIT:-15}
    case "$limit" in 0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15) ;; *) fail 'Invalid fixture wait';; esac
fi
elapsed=0
while :; do
    ack=0
    if [ -e "$BASE/logs/capture.done" ] || [ -L "$BASE/logs/capture.done" ]; then
        regular "$BASE/logs/capture.done"
        [ "$(wc -c < "$BASE/logs/capture.done")" -eq 37 ] &&
            [ "$(cat "$BASE/logs/capture.done")" = "$boot_id" ] || fail 'Capture acknowledgement is not from this boot; retain/export available logs without claiming completion'
        ack=1
    fi
    if [ "$ack" = 1 ] && [ ! -e "$BASE/logs/collector.pid" ] && [ ! -L "$BASE/logs/collector.pid" ]; then
        echo 'Current-boot capture freeze acknowledged and collector PID marker absent. Export logs now, before another boot.'
        exit 0
    fi
    [ "$elapsed" -lt "$limit" ] || fail 'Freeze not confirmed within deadline; do not assume complete logs. No process killed. Preserve/export available logs and report this result.'
    sleep 1
    elapsed=$((elapsed + 1))
done
