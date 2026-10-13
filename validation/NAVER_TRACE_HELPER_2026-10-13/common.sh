# Shared settings for the Naver trace helper (POSIX sh). Run these scripts on the owner's Mac with adb in PATH.
PKG=com.nhn.android.nmap
DIR=/sdcard/Android/data/$PKG/files
CONF=$DIR/navi.config
need_adb() {
    command -v adb >/dev/null 2>&1 || { echo "adb not found in PATH"; exit 1; }
    n=$(adb devices | awk 'NR>1 && $2=="device"{c++} END{print c+0}')
    [ "$n" = 1 ] || { echo "expected exactly one authorised device, found $n (adb devices)"; exit 1; }
}
