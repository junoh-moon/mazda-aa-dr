#!/usr/bin/env python3
"""Write the touch route's USB entry into a bundle directory (host only).

The CMU update scanner starts a stick's entry when the stick is inserted: a
zip archive whose file name is a small shell expression. The scanner evaluates
that name, so the expression runs touch.sh from the mounted stick. A file name
cannot contain a slash, so the expression builds its paths from
${HOME%root}, which is "/" on the CMU. Only the one stick that carries
MX5DR-TOUCH.ID is started; otherwise the expression does nothing.

Reference for the mechanism and the archive layout: TouchTune 1.2.1
(https://github.com/Miatafy/TouchTune, GPL-3.0-or-later). This file is written
for this project; no TouchTune code or values are copied. The name and the
archive layout are unverified on this CMU (validation/TOUCH_INSTALL_UX_2026-10-31.md).
"""
from pathlib import Path
import sys
import zipfile

ENTRY_NAME = ('$(S=${HOME%root};B=$MX5DR_ROOT;N=0;for D in sda1 sdb1 sdc1 sdd1;do P=$B${S}tmp${S}mnt${S}$D;'
              'M=$P${S}MX5DR-TOUCH.ID;[ -f $M ]&&{ N=$((N+1));I=$P${S}touch.sh;};done;'
              '[ $N = 1 ]&&[ -f $I ]&&sh $I).up')
MEMBERS = ('main_instructions.ini', 'versions.ini')
LAUNCHER = Path(__file__).resolve().parent / 'launcher'
# Fixed timestamp: the same sources always produce the same bytes.
ZIP_TIME = (2026, 10, 31, 0, 0, 0)


def main():
    if len(sys.argv) != 2:
        raise SystemExit('Usage: make_entry.py BUNDLE_DIR')
    dest = Path(sys.argv[1])
    if '/' in ENTRY_NAME or len(ENTRY_NAME.encode('ascii')) > 200:
        raise SystemExit('ERROR: entry name is not a plain file name')
    if not dest.is_dir():
        raise SystemExit('ERROR: bundle directory is missing: %s' % dest)
    target = dest / ENTRY_NAME
    if target.exists():
        raise SystemExit('ERROR: entry already exists: %s' % target.name)
    with zipfile.ZipFile(target, 'x', zipfile.ZIP_STORED) as archive:
        for name in MEMBERS:
            info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0o644 << 16
            archive.writestr(info, (LAUNCHER / name).read_bytes())


if __name__ == '__main__':
    main()
