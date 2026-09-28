#!/bin/sh
# Host-side bundle preparation. Does not execute the ARM payload.
set -eu
[ "$#" = 2 ] || { echo 'Usage: sh make_bundle.sh built/libmx5dr.so new-output-directory' >&2; exit 2; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
lib=$1; dest=$2
collector=$(dirname -- "$lib")/mx5dr-collector
[ -f "$lib" ] && [ -f "$collector" ] && [ ! -e "$dest" ] || { echo 'Library/collector missing or output already exists' >&2; exit 1; }
mkdir -p "$dest"
for file in install.sh uninstall.sh export_logs.sh start_collector.sh stop_collector.sh common.sh edit_service.awk firmware.sha256 mx5dr.conf; do cp "$HERE/$file" "$dest/$file"; done
cp "$lib" "$dest/libmx5dr.so"
cp "$collector" "$dest/mx5dr-collector"
(cd "$dest" && sha256sum libmx5dr.so > libmx5dr.so.sha256)
(cd "$dest" && sha256sum mx5dr-collector > mx5dr-collector.sha256)
echo "Bundle prepared: $dest (not vehicle-tested)"
