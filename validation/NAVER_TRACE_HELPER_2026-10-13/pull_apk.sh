#!/bin/sh
# READ-ONLY: copies the installed Naver Map APK files (base + splits) from the phone to ./naver-apk-<version>/ and zips them.
# Nothing on the phone is changed. The APKs stay private (never publish them).
. "$(dirname "$0")/common.sh"
need_adb
ver=$(adb shell dumpsys package "$PKG" 2>/dev/null | grep -m1 versionName | sed 's/.*=//' | tr -d '\r ')
[ -n "$ver" ] || { echo "package not found"; exit 1; }
out="naver-apk-$ver"
mkdir -p "$out"
adb shell pm path "$PKG" | tr -d '\r' | sed 's/^package://' | while read -r p; do
    [ -n "$p" ] || continue
    adb pull "$p" "$out/" 2>&1 | tail -1
done
ls -la "$out"
zip -qr "$out.zip" "$out" && ls -la "$out.zip"
echo "Upload (private):  curl --upload-file $out.zip https://transfer.sixtyfive.me/"
