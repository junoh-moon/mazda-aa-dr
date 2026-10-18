#!/bin/sh
# Release stage 2 (G5, G6): self-contained installation ZIP (mp3/, js/,
# USB_ENTRY_NOTICE.md; never --shell-only) with --default-mode ${REL_MODE:-BETA},
# unpacked copy in WORK/unpacked, and the ZIP-level checker.
#   sh tools/release/zip.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin zip
set -e
cd "$SRC"
[ ! -e "$WORK/dist/$NAME.zip" ] || { echo 'ZIP exists: never overwrite a built ZIP'; exit 1; }
nice -n "$REL_NICE" python3 tools/make_usb_zip.py --build-dir "$WORK/arm" --default-mode "${REL_MODE:-BETA}" \
    --output "$WORK/dist/$NAME.zip"
unzip -tq "$WORK/dist/$NAME.zip"
rm -rf "$WORK/unpacked"; mkdir "$WORK/unpacked"; unzip -q "$WORK/dist/$NAME.zip" -d "$WORK/unpacked"
(cd "$WORK/unpacked" && sha256sum -c --quiet SHA256SUMS)
cat "$WORK/dist/$NAME.zip.sha256"
python3 "$GATES"/g5g6_zipcheck.py --zip "$WORK/dist/$NAME.zip" --commit "$COMMIT" --mode "${REL_MODE:-BETA}" \
    --readelf "$SRC/tools/m3-toolchain/bin/arm-cortexa9_neon-linux-gnueabi-readelf" > "$WORK/evidence/zipcheck.txt" 2>&1 ||
    { tail -20 "$WORK/evidence/zipcheck.txt"; exit 1; }
tail -1 "$WORK/evidence/zipcheck.txt"
for f in libmx5dr.so libmx5dr-vimtap.so libmx5dr-ldstap.so mx5dr-collector mx5dr-guard mx5dr-sha256; do
    file "$WORK/arm/$f"
    "$SRC/tools/m3-toolchain/bin/arm-cortexa9_neon-linux-gnueabi-readelf" -h -A -d -V "$WORK/arm/$f"
done > "$WORK/evidence/elf.txt"
evidence zipcheck.txt; evidence elf.txt
