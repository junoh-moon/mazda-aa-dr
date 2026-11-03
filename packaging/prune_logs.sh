#!/bin/sh
# Menu 3 (and the touch SAVE LOGS button) after a successful capture freeze
# and export: delete the exported journal files from the CMU, so the next
# export carries only the boots after this one (owner request 2026-11-03,
# validation/PRUNE_EXPORTED_LOGS_2026-11-03.md). Fail closed: nothing is
# deleted unless every check passes, and then only the rotating journals
# (trace.N.jsonl, collector.N.jsonl, pimu.N.jsonl). State files the features
# need (guard, G3 heading record, markers, installation record) stay.
#   - this boot's writers stopped: capture.done holds the current boot ID,
#     no collector PID marker, the collector lock is free;
#   - the archive on the USB re-reads to the checksum written next to it;
#   - every journal to delete is listed in the archive with its current size.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
[ "$#" = 1 ] || fail 'Usage: sh prune_logs.sh /usb/mx5dr-logs-....tar'
archive=$1
case "$archive" in /*.tar) ;; *) fail 'Archive must be an absolute .tar path; nothing deleted';; esac
regular "$archive"
regular "$archive.sha256"
validate_persist
logs=$BASE/logs
[ -d "$BASE" ] && [ ! -L "$BASE" ] && [ -d "$logs" ] && [ ! -L "$logs" ] ||
    fail 'Missing or symlink log directory; nothing deleted'
persist_path=$(CDPATH= cd -P -- "$persist" && pwd -P) || fail 'Cannot resolve persistent storage; nothing deleted'
archive_dir=$(CDPATH= cd -P -- "$(dirname -- "$archive")" && pwd -P) || fail 'Cannot resolve the archive directory; nothing deleted'
case "$archive_dir/" in "$persist_path/"*) fail 'Archive is inside persistent storage, not on the USB; nothing deleted';; esac

# 1. Nothing of this boot writes the journals any more.
regular "$ROOT/proc/sys/kernel/random/boot_id"
boot_id=$(cat "$ROOT/proc/sys/kernel/random/boot_id")
valid_boot_id "$boot_id" || fail 'Invalid current boot ID; nothing deleted'
[ -f "$logs/capture.done" ] && [ ! -L "$logs/capture.done" ] ||
    fail 'No capture freeze acknowledgement; nothing deleted'
[ "$(cat "$logs/capture.done")" = "$boot_id" ] ||
    fail 'Capture freeze acknowledgement is not from this boot; nothing deleted'
[ ! -e "$logs/collector.pid" ] && [ ! -L "$logs/collector.pid" ] ||
    fail 'Collector PID marker present; nothing deleted'
if [ -e "$logs/collector.lock" ] || [ -L "$logs/collector.lock" ]; then
    regular "$logs/collector.lock"
    ( exec 7< "$logs/collector.lock" && flock -n 7 ) 2>/dev/null ||
        fail 'Collector still holds its lock; nothing deleted'
fi

# 2. The archive on the USB is the one export_logs.sh wrote.
sync
want=$(awk 'NR==1{print $1}' "$archive.sha256")
have=$(hash "$archive") || fail 'Cannot re-read the archive; nothing deleted'
[ -n "$want" ] && [ "$have" = "$want" ] || fail 'Archive checksum does not match; nothing deleted'

# 3. Each journal is in the archive with its current size.
listing=$(tar -tvf "$archive") || fail 'Cannot list the archive; nothing deleted'
prefix=${BASE#/}/logs/
victims=
count=0
bytes=0
for journal in "$logs"/trace.[0-9].jsonl "$logs"/collector.[0-9].jsonl "$logs"/pimu.[0-9].jsonl; do
    [ -e "$journal" ] || [ -L "$journal" ] || continue
    [ -f "$journal" ] && [ ! -L "$journal" ] || fail "Not a regular file: ${journal##*/}; nothing deleted"
    size=$(wc -c < "$journal") || fail "Cannot read ${journal##*/}; nothing deleted"
    printf '%s\n' "$listing" | awk -v p="$prefix${journal##*/}" -v s="$size" \
        '$NF==p && $3==s {found=1} END{exit !found}' ||
        fail "${journal##*/} is not in the archive with its current size; nothing deleted"
    victims="$victims $journal"
    count=$((count + 1))
    bytes=$((bytes + size))
done
if [ "$count" = 0 ]; then
    echo 'prune=nothing_to_delete'
    exit 0
fi

ALLOW_REMOUNT=1
mount_rw "$persist"
deleted=0
for journal in $victims; do
    rm -f "$journal" || fail "Deleted $deleted of $count exported journals, then failed at ${journal##*/}; the rest stay"
    deleted=$((deleted + 1))
done
sync
echo "prune=done files=$count bytes=$bytes (exported journals deleted from the CMU; the USB archive is now the only copy)"
