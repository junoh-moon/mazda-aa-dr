#!/bin/sh
# Host-side bundle preparation. Does not execute the ARM payload.
set -eu
[ "$#" = 2 ] || { echo 'Usage: sh make_bundle.sh built/libmx5dr.so new-output-directory' >&2; exit 2; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
lib=$1; dest=$2
build_dir=$(dirname -- "$lib")
[ -f "$build_dir/mx5dr-collector" ] || { echo "Missing sibling mx5dr-collector build" >&2; exit 1; }
[ -f "$build_dir/mx5dr-guard" ] || { echo "Missing sibling mx5dr-guard build" >&2; exit 1; }
[ -f "$lib" ] && [ ! -e "$dest" ] || { echo 'Input missing or output already exists' >&2; exit 1; }
mkdir -p "$dest"
for file in install.sh uninstall.sh export_logs.sh common.sh edit_service.awk edit_autostart.awk arm.sh start_collector.sh stop_collector.sh firmware.sha256 mx5dr.conf; do cp "$HERE/$file" "$dest/$file"; done
cp "$lib" "$dest/libmx5dr.so"
cp "$build_dir/mx5dr-guard" "$dest/mx5dr-guard"
cp "$build_dir/mx5dr-collector" "$dest/mx5dr-collector"
(cd "$dest" && sha256sum mx5dr-collector > mx5dr-collector.sha256)
(cd "$dest" && sha256sum mx5dr-guard > mx5dr-guard.sha256)
(cd "$dest" && sha256sum libmx5dr.so > libmx5dr.so.sha256)
echo "Bundle prepared: $dest (not vehicle-tested)"
