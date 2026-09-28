#!/bin/sh
# Parked authorized root shell, or verified root startup. Not a recovery guard.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
[ -n "$ROOT" ] || [ "$(id -u)" = 0 ] || fail 'Run this helper from an authorized root shell'
[ "$#" -le 1 ] || fail 'Usage: sh start_collector.sh [session-seconds]'
seconds=${1:-28800}
case "$seconds" in ''|*[!0-9]*) fail 'Session seconds must be 1..86400';; esac
[ "$seconds" -ge 1 ] && [ "$seconds" -le 86400 ] || fail 'Session seconds must be 1..86400'
regular "$BASE/mx5dr-collector"
regular "$BASE/mx5dr.conf"
[ -x "$BASE/mx5dr-collector" ] || fail 'Collector not executable'
[ -d "$BASE/logs" ] && [ ! -L "$BASE/logs" ] && [ -w "$BASE/logs" ] || fail 'Writable owned logs directory required'
# The production executable drops root to cmu before DBus/config/log access.
# No OEM preload in the collector or its SMDB children. Preserve the authorized
# shell's other environment; the necessary DBus/library setup is site-specific.
# A kernel flock in the executable rejects duplicate starts, without stale-PID kills.
(
    unset LD_PRELOAD LD_AUDIT
    exec nohup "$BASE/mx5dr-collector" --session-seconds "$seconds"
) </dev/null >/dev/null 2>&1 &
echo "Collector launch requested (PID $!); verify collector_boot while parked. No AA process was changed."
echo 'The collector does not restart after reboot; use the verified external startup path when available.'
