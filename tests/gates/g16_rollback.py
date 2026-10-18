#!/usr/bin/env python3
"""G16 rollback proof on a stock-BusyBox replica (see emu.py).

  python3 tests/gates/g16_rollback.py --candidate CAND.zip --previous PREV.zip \
      [--previous-sha256 HEX] [--libpatch DIR]

 1. candidate: menu 1, menu 5, product boot, menu 2 GO;
 2. rollback with digits only: menu 4, menu 5, next boot: SM gets the stock
    sm.conf (normal and WCP branch), menu 2 shows 'stock this boot', the
    autostart is byte-identical to the pre-install file (the only tolerated
    difference is a trailing newline) and every other stock file is
    byte-identical; no owned block, no persist file;
 3. the previous ZIP is reinstallable over the rolled-back state: menu 1,
    menu 5, product boot, menu 2 GO; then menu 4 again returns to stock.
Exit 0 pass, 1 fail, 77 skip."""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu  # noqa: E402


def install_and_boot(r, tag):
    rc, screen = r.menu('1\n0\n', f'{tag}: menu 1 install')
    r.check(f'{tag}: install rc 0 and persistent', rc == 0 and 'Installed BETA persistent' in screen, f'rc={rc}')
    r.menu('5\n', f'{tag}: menu 5 reboot (witness)')
    chosen = r.autostart_boot(f'{tag} product boot')
    r.check(f'{tag}: product boot with all preloads', r.trial_complete(chosen), chosen)
    screen, block = r.status(f'{tag} product boot')
    r.check(f'{tag}: menu 2 product this boot, no NO row, GO or WAIT (runtime rows)',
            'ok   BOOT  product this boot' in block and r.install_side_go(block)[0], r.install_side_go(block)[1])


def rollback(r, tag):
    rc, screen = r.menu('4\n', f'{tag}: menu 4 uninstall')
    r.check(f'{tag}: menu 4 rc 0', rc == 0, f'rc={rc}')
    r.menu('5\n', f'{tag}: menu 5 reboot (witness)')
    chosen = r.autostart_boot(f'{tag}: first boot after menu 4')
    r.check(f'{tag}: SM gets the stock sm.conf', chosen == '/jci/sm/sm.conf', chosen)
    chosen = r.autostart_boot(f'{tag}: WCP board after menu 4', var='SMCFG_WCPMODE')
    r.check(f'{tag}: SM gets the stock sm_WCP.conf', chosen == '/jci/sm/sm_WCP.conf', chosen)
    screen, block = r.status(f'{tag} after rollback')
    want = os.environ.get('MX5DR_GATE_STOCK_TEXT', 'stock this boot')
    r.check(f'{tag}: menu 2 says {want!r}', any(want in x for x in block), str(block[:12]))
    r.autostart_restored(tag)
    auto = (r.root / 'usr/bin/autostart').read_text(errors='replace')
    r.check(f'{tag}: no owned autostart block left', emu.BEGIN not in auto)
    r.check(f'{tag}: no persist/arm file left', not (r.base / 'guard/persist').exists()
            and not (r.base / 'guard/arm').exists())
    r.oem_identical(f'{tag}', allow_autostart_blocks=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--candidate', default=os.environ.get('MX5DR_GATE_CANDIDATE'))
    ap.add_argument('--previous', default=os.environ.get('MX5DR_GATE_PREVIOUS'))
    ap.add_argument('--previous-sha256', default=os.environ.get('MX5DR_GATE_PREVIOUS_SHA256'))
    ap.add_argument('--libpatch', default=os.environ.get('MX5DR_LIBPATCH_DIR'))
    a = ap.parse_args()
    if not a.previous or not a.candidate:
        emu.skip('--previous and --candidate (or MX5DR_GATE_PREVIOUS/MX5DR_GATE_CANDIDATE) are required')
    stock, proot, qemu, work = emu.environment()
    if a.previous_sha256 and Path(a.previous).is_file():
        emu.verify_zip_sha256(a.previous, a.previous_sha256)
    cand = emu.unpack(a.candidate, work / 'g16-candidate')
    prev = emu.unpack(a.previous, work / 'g16-previous')
    r = emu.Replica(work, 'g16', stock, proot, qemu)
    r.emit(f'G16 rollback: candidate={Path(a.candidate).name} previous={Path(a.previous).name}')
    r.build(cand, a.libpatch)
    r.emit('\n#### 1. Candidate installed and running')
    install_and_boot(r, 'candidate')
    r.emit('\n#### 2. Rollback with digits only: menu 4, menu 5, menu 2')
    rollback(r, 'rollback')
    r.emit('\n#### 3. Previous ZIP reinstallable, then removed again')
    r.swap_usb(prev)
    install_and_boot(r, 'previous')
    rollback(r, 'previous removed')
    return r.summary('G16')


if __name__ == '__main__':
    sys.exit(main())
