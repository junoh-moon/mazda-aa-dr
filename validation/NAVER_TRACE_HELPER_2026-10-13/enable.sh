#!/bin/sh
# Writes ONE line (trace=1) to navi.config so Naver Map writes its decision and per-location logs.
# A previous navi.config is saved as navi.config.bak (restored by disable.sh).
. "$(dirname "$0")/common.sh"
need_adb
printf 'This writes %s/navi.config with the single line trace=1.\nThe logs will contain precise coordinates. Type yes to continue: ' "$DIR"
read -r answer
[ "$answer" = yes ] || { echo "aborted"; exit 1; }
if adb shell "[ -f '$CONF' ]" >/dev/null 2>&1; then
    echo "existing navi.config found; saving it as navi.config.bak"
    adb shell "cp '$CONF' '$CONF.bak'" || { echo "backup failed"; exit 1; }
fi
printf 'trace=1\n' | adb shell "cat > '$CONF'" || { echo "write failed"; exit 1; }
echo "== now in navi.config:"
adb shell cat "$CONF"
echo "Done. Restart Naver Map navigation once so it re-reads the file. Never add other keys (ignore_input_location drops all locations)."
