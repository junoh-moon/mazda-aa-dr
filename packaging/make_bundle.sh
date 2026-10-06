#!/bin/sh
# Host-side bundle preparation. Does not execute the ARM payload.
set -eu
DEFAULT_MODE=OBSERVE
SHELL_ONLY=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --shell-only) SHELL_ONLY=1; shift;;
    --default-mode=OBSERVE|--default-mode=SHADOW|--default-mode=BETA) DEFAULT_MODE=${1#--default-mode=}; shift;;
    --*) echo 'Unknown bundle option' >&2; exit 2;;
    *) break;;
  esac
done
[ "$#" = 2 ] || { echo 'Usage: sh make_bundle.sh [--shell-only] [--default-mode=OBSERVE|SHADOW|BETA] built/libmx5dr.so new-output-directory' >&2; exit 2; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
lib=$1; dest=$2
build_dir=$(dirname -- "$lib")
[ -f "$build_dir/mx5dr-collector" ] || { echo "Missing sibling mx5dr-collector build" >&2; exit 1; }
[ -f "$build_dir/mx5dr-guard" ] || { echo "Missing sibling mx5dr-guard build" >&2; exit 1; }
[ -f "$build_dir/libmx5dr-vimtap.so" ] || { echo "Missing sibling libmx5dr-vimtap.so build" >&2; exit 1; }
[ -f "$build_dir/libmx5dr-ldstap.so" ] || { echo "Missing sibling libmx5dr-ldstap.so build" >&2; exit 1; }
[ -f "$build_dir/mx5dr-sha256" ] || { echo "Missing sibling static mx5dr-sha256 build" >&2; exit 1; }
[ -f "$lib" ] && [ ! -e "$dest" ] || { echo 'Input missing or output already exists' >&2; exit 1; }
mkdir -p "$dest"
printf '%s\n' "$DEFAULT_MODE" > "$dest/bundle-default-mode"
for file in trial install.sh uninstall.sh export_logs.sh common.sh edit_service.awk edit_autostart.awk arm.sh start_collector.sh stop_collector.sh finish_capture.sh trial_status.sh trial_status.awk startup_diagnostics.sh reboot_cmu.sh firmware.sha256 mx5dr.conf; do cp "$HERE/$file" "$dest/$file"; done
cp "$lib" "$dest/libmx5dr.so"
cp "$build_dir/libmx5dr-vimtap.so" "$dest/libmx5dr-vimtap.so"
cp "$build_dir/libmx5dr-ldstap.so" "$dest/libmx5dr-ldstap.so"
cp "$build_dir/mx5dr-guard" "$dest/mx5dr-guard"
cp "$build_dir/mx5dr-collector" "$dest/mx5dr-collector"
cp "$build_dir/mx5dr-sha256" "$dest/mx5dr-sha256"
if [ "$SHELL_ONLY" = 1 ]; then
  cp "$HERE/SHELL_START_KO.md" "$dest/INSTALL_KO.md"
else
  cp -R "$HERE/usb-entry/mp3" "$HERE/usb-entry/js" "$dest/"
  cp "$HERE/usb-entry/NOTICE.md" "$dest/USB_ENTRY_NOTICE.md"
  cp "$HERE/USB_START_KO.md" "$dest/INSTALL_KO.md"
fi
(cd "$dest" && sha256sum mx5dr-sha256 > mx5dr-sha256.sha256)
(cd "$dest" && sha256sum mx5dr-collector > mx5dr-collector.sha256)
(cd "$dest" && sha256sum mx5dr-guard > mx5dr-guard.sha256)
(cd "$dest" && sha256sum libmx5dr.so > libmx5dr.so.sha256)
(cd "$dest" && sha256sum libmx5dr-vimtap.so > libmx5dr-vimtap.so.sha256)
(cd "$dest" && sha256sum libmx5dr-ldstap.so > libmx5dr-ldstap.so.sha256)
policy='one guarded boot'
[ "$DEFAULT_MODE" != BETA ] || policy='persistent, every guarded boot'
echo "Bundle prepared: $dest (default=$DEFAULT_MODE; $policy; not vehicle-tested)"
