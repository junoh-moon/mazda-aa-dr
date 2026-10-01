#!/bin/sh
# Read-only, parked collection check. Never arms, stops, restarts or changes mode.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
[ "$#" = 0 ] || fail 'Usage: sh trial_status.sh (while parked)'
validate_persist
for directory in "$BASE" "$BASE/logs" "$BASE/guard"; do
    [ -d "$directory" ] && [ ! -L "$directory" ] || fail "Missing or symlink directory: $directory"
done
regular "$ROOT/proc/sys/kernel/random/boot_id"
regular "$ROOT/proc/uptime"
regular "$HERE/trial_status.awk"
boot_id=$(cat "$ROOT/proc/sys/kernel/random/boot_id")
now=$(awk 'NR==1 && $1 ~ /^[0-9]+\.[0-9]+$/ {print $1}' "$ROOT/proc/uptime")
[ -n "$now" ] || fail 'Cannot read current monotonic uptime'
valid_boot_id "$boot_id" || fail 'Invalid current boot ID'
# A guard marker alone proves neither a live process nor successful capture.
oneboot=unconfirmed
if [ -e "$BASE/guard/last-boot" ] || [ -L "$BASE/guard/last-boot" ]; then
    regular "$BASE/guard/last-boot"
    [ "$(wc -c < "$BASE/guard/last-boot")" -le 37 ] || fail 'Invalid guard boot marker'
    [ "$(cat "$BASE/guard/last-boot")" != "$boot_id" ] || oneboot=consumed_this_boot
fi
if [ -e "$BASE/guard/arm" ] || [ -L "$BASE/guard/arm" ]; then
    regular "$BASE/guard/arm"
    oneboot=arm_present
fi
set --
retained=0
# Fixed bounded filenames only, oldest first within each stream. A rotated-away
# boot is unavailable evidence; do not attribute orphan rows to this boot.
for name in trace.2.jsonl trace.1.jsonl trace.0.jsonl collector.1.jsonl collector.0.jsonl \
            trace.storage.json collector.storage.json; do
    file=$BASE/logs/$name
    if [ -e "$file" ] || [ -L "$file" ]; then
        regular "$file"
        bytes=$(wc -c < "$file")
        case "$name" in *.storage.json) limit=1024;; collector.*) limit=1048576;; *) limit=8388608;; esac
        [ "$bytes" -le "$limit" ] || fail "Log exceeds expected per-file bound: $name"
        retained=$((retained + bytes))
        set -- "$@" "$file"
    fi
done
echo "Parked capture evidence only; this does not approve driving or ASSIST."
echo "current_boot_id=$boot_id. Recent checks can be unavailable after AA disconnect or reboot; retained rows are reported separately."
echo "one_boot=$oneboot retained_bytes=$retained trace_cap_bytes=25165824 collector_cap_bytes=2097152 (24+2 MiB, rotates)"
echo 'Retention duration is unknown until this vehicle log rate is measured. Export at the first parked USB return, without reinstalling or rearming. Reboot may leave incomplete final rows; do not repeat a drive just to obtain a status pass.'
space_ok=0
space_free=$(storage_free_kib "$persist") || space_free=unknown
if [ "$space_free" != unknown ] && [ "$space_free" -gt 8256 ]; then space_ok=1; fi
echo "storage_available_kib=$space_free reserve_kib=8192 margin_kib=64"
[ "$#" -gt 0 ] || fail 'No retained logs; current collection evidence unavailable'
LC_ALL=C awk -v boot="$boot_id" -v now="$now" -v oneboot="$oneboot" -v space_ok="$space_ok" -f "$HERE/trial_status.awk" "$@"
