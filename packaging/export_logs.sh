#!/bin/sh
# Run after parking, with the removable destination mounted. No daemon changes.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
[ "$#" = 1 ] || fail 'Usage: sh export_logs.sh /mounted/destination/directory'
dest=$1
case "$dest" in /*) ;; *) fail 'Destination must be absolute';; esac
[ -d "$dest" ] && [ ! -L "$dest" ] || fail 'Destination is not a real directory'
[ -d "$BASE/logs" ] && [ ! -L "$BASE/logs" ] || fail 'No log directory'
name=mx5dr-logs-$(date +%Y%m%dT%H%M%S)-$$.tar
[ ! -e "$dest/$name" ] || fail 'Export already exists'
# Export only our bounded logs/config; not vehicle-wide logs or shell credentials.
tar -cf "$dest/$name.partial" -C "$BASE" logs mx5dr.conf || fail 'Export failed; partial retained'
mv "$dest/$name.partial" "$dest/$name"
sha256sum "$dest/$name" > "$dest/$name.sha256"
sync
echo "$dest/$name"
echo 'Live writer may have rotated during export; partial final JSONL records are possible. Originals retained.'
