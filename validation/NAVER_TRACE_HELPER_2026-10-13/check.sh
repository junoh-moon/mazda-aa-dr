#!/bin/sh
# READ-ONLY check: nothing on the phone is changed.
. "$(dirname "$0")/common.sh"
need_adb
echo "== app version"
adb shell dumpsys package "$PKG" 2>/dev/null | grep -m1 versionName || echo "package not found"
echo "== files dir (access test)"
adb shell ls -la "$DIR" 2>&1
echo "== navi.config (if present)"
adb shell cat "$CONF" 2>&1 | head -20
echo "== existing trace/nmea folders"
adb shell ls -la "$DIR/NaverNavi" 2>&1 | head -10
