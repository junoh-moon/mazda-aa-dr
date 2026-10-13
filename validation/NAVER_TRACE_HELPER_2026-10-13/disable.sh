#!/bin/sh
# Removes navi.config (or restores the saved navi.config.bak). Existing log files stay until collect.sh --clear.
. "$(dirname "$0")/common.sh"
need_adb
if adb shell "[ -f '$CONF.bak' ]" >/dev/null 2>&1; then
    adb shell "mv '$CONF.bak' '$CONF'" && echo "restored the previous navi.config"
else
    adb shell "rm -f '$CONF'" && echo "removed navi.config"
fi
adb shell ls -la "$DIR" 2>&1
