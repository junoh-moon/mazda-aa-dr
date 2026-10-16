#!/bin/sh
set -eu
umask 077
imu_project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
imu_sdk=${1:-${ANDROID_SDK_ROOT:-"$HOME/.local/share/android-sdk"}}
imu_out=${2:-"$imu_project/build"}
rm -rf "$imu_out/classes" "$imu_out/dex"
mkdir -p "$imu_out/classes" "$imu_out/dex"
imu_out=$(CDPATH= cd -- "$imu_out" && pwd)
imu_tools="$imu_sdk/build-tools/36.0.0"
imu_android="$imu_sdk/platforms/android-36/android.jar"
test -r "$imu_android"
javac --release 11 -Xlint:all,-deprecation -Werror -cp "$imu_android" -d "$imu_out/classes" "$imu_project"/src/me/sixtyfive/mx5imu/*.java "$imu_project"/src/me/sixtyfive/mx5imu/core/*.java
jar cf "$imu_out/classes.jar" -C "$imu_out/classes" .
"$imu_tools/d8" --lib "$imu_android" --min-api 30 --output "$imu_out/dex" "$imu_out/classes.jar"
# the screen shows versionName; append the repository commit (MX5IMU_COMMIT, e.g. git rev-parse --short HEAD) so builds can be told apart.
# aapt2 --version-name only fills an ABSENT attribute, so the value is rewritten in a copy of the manifest.
imu_version=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$imu_project/AndroidManifest.xml")
imu_commit=${MX5IMU_COMMIT:-unknown}
sed "s|android:versionName=\"$imu_version\"|android:versionName=\"$imu_version+$imu_commit\"|" "$imu_project/AndroidManifest.xml" > "$imu_out/AndroidManifest.xml"
grep -q "android:versionName=\"$imu_version+$imu_commit\"" "$imu_out/AndroidManifest.xml"
"$imu_tools/aapt2" link -I "$imu_android" --manifest "$imu_out/AndroidManifest.xml" -o "$imu_out/base.apk"
python3 - "$imu_out" <<'PY'
from pathlib import Path
import sys,zipfile
out=Path(sys.argv[1])
with zipfile.ZipFile(out/'base.apk','a') as z:z.write(out/'dex/classes.dex','classes.dex')
PY
"$imu_tools/zipalign" -f 4 "$imu_out/base.apk" "$imu_out/aligned.apk"
if [ ! -f "$imu_out/sender-debug.p12" ]; then
    openssl rand -hex 24 > "$imu_out/signing.pass"
    keytool -genkeypair -keystore "$imu_out/sender-debug.p12" -storetype PKCS12 -storepass:file "$imu_out/signing.pass" -keypass:file "$imu_out/signing.pass" -alias sender -keyalg RSA -keysize 2048 -validity 3650 -dname 'CN=MX5DR IMU sender debug' -noprompt
fi
"$imu_tools/apksigner" sign --ks "$imu_out/sender-debug.p12" --ks-key-alias sender --ks-pass "file:$imu_out/signing.pass" --out "$imu_out/mx5imu-sender.apk" "$imu_out/aligned.apk"
"$imu_tools/apksigner" verify --verbose "$imu_out/mx5imu-sender.apk"
sha256sum "$imu_out/mx5imu-sender.apk"
