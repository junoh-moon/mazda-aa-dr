#!/bin/sh
# Pulls NaverNavi/trace, NaverNavi/nmea and NaverNavi/altitude into ./naver-trace-<time>/ and zips them (private data: coordinates).
# --clear additionally deletes those logs on the phone after a successful pull and a non-empty zip.
. "$(dirname "$0")/common.sh"
need_adb
out="naver-trace-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
adb pull "$DIR/NaverNavi/trace" "$out/" 2>&1 | tail -2
adb pull "$DIR/NaverNavi/nmea" "$out/" 2>&1 | tail -2
adb pull "$DIR/NaverNavi/altitude" "$out/" 2>&1 | tail -2
adb shell ls -la "$DIR/NaverNavi" > "$out/listing.txt" 2>&1 || true
adb shell dumpsys package "$PKG" 2>/dev/null | grep -m1 versionName > "$out/version.txt" || true
zip -qr "$out.zip" "$out" && ls -la "$out.zip"
[ -s "$out.zip" ] || { echo "zip is empty: nothing cleared"; exit 1; }
if [ "${1:-}" = "--clear" ]; then
    adb shell "rm -rf '$DIR/NaverNavi/trace' '$DIR/NaverNavi/nmea' '$DIR/NaverNavi/altitude'" && echo "cleared the logs on the phone"
fi
echo "Upload (private data):  curl --upload-file $out.zip https://transfer.sixtyfive.me/"
