#!/bin/sh
# Cooperative request only; never signal a potentially reused PID or AA process.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
[ -n "$ROOT" ] || [ "$(id -u)" = 0 ] || fail 'Run this helper from an authorized root shell'
[ "$#" = 0 ] || fail 'Usage: sh stop_collector.sh'
[ -d "$BASE/logs" ] && [ ! -L "$BASE/logs" ] || fail 'Missing logs directory'
# Directory creation is atomic and does not follow a final-component symlink.
if [ ! -e "$BASE/logs/collector.stop" ]; then
    mkdir "$BASE/logs/collector.stop" || fail 'Could not request collector stop'
fi
echo 'Collector stop requested. Confirm collector_stop after the current poll; no PID was killed.'
echo 'If the collector is stuck in kernel/DBus work, this request cannot guarantee immediate termination.'
