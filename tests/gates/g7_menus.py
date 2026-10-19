#!/usr/bin/env python3
"""G7 owner-switch menus (7 to 12) under the stock BusyBox (see emu.py).

  python3 tests/gates/g7_menus.py --bundle ZIP|DIR [--toggles '7:vimchan-off 8:g3-off ...']

For each listed menu digit and marker file name, on an installed replica:
 - before installation the menu changes nothing and says so;
 - the menu with a wrong confirmation digit cancels and changes nothing;
 - the menu with its own digit twice flips the marker (file present <-> absent)
   with no staging file (*.new.*) left behind; a second time flips it back;
 - menu 2 runs without a shell/awk error while the marker is set;
 - a marker that is a directory is reported and left alone.
The default list is the v1.0 menu set (menu 9 removed); the beta.17 list
(with 9:diag) is given with --toggles or MX5DR_GATE_TOGGLES. The marker semantics (which state means on) are not
asserted here; G8 asserts the installer's marker rules.
Exit 0 pass, 1 fail, 77 skip."""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu  # noqa: E402

DEFAULT = '7:vimchan-off 8:g3-off 10:acc-cap-off 11:bt-off 12:g3-garage-off'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bundle', default=os.environ.get('MX5DR_GATE_CANDIDATE'))
    ap.add_argument('--libpatch', default=os.environ.get('MX5DR_LIBPATCH_DIR'))
    ap.add_argument('--toggles', default=os.environ.get('MX5DR_GATE_TOGGLES', DEFAULT))
    a = ap.parse_args()
    if not a.bundle:
        emu.skip('--bundle (or MX5DR_GATE_CANDIDATE) is required')
    stock, proot, qemu, work = emu.environment()
    bundle = emu.unpack(a.bundle, work / 'g7menus-bundle')
    toggles = [tuple(x.split(':', 1)) for x in a.toggles.split()]
    r = emu.Replica(work, 'g7-menus', stock, proot, qemu)
    r.build(bundle, a.libpatch)
    menu_text = (bundle / 'trial').read_text()

    r.emit('\n#### 0. Not installed: every switch changes nothing')
    for digit, name in toggles:
        rc, screen = r.menu(f'{digit}\n{digit}\n0\n', f'menu {digit} before installation')
        r.check(f'menu {digit} before installation: nothing changed', 'nothing changed' in screen
                and not r.base.exists(), f'rc={rc}')
    rc, screen = r.menu('1\n0\n', 'menu 1 install')
    r.check('install rc 0', rc == 0, f'rc={rc}')
    for digit, name in toggles:
        if f'{digit}) if ! ' not in menu_text:
            r.skipped(f'menu {digit} ({name})', 'this bundle has no such menu')
            continue
        marker = r.base / name
        r.emit(f'\n#### menu {digit}: {name}')
        start = marker.exists()
        if name == 'bt-off' and start:
            # Turning the phone Bluetooth channel ON needs a prepared config copy,
            # which the replica CMU does not have: the refusal is the contract.
            rc, screen = r.menu(f'{digit}\n{digit}\n0\n', f'menu {digit} ON without a prepared copy')
            r.check(f'menu {digit}: ON is refused without a prepared Bluetooth copy and the marker stays',
                    'Not prepared' in screen and marker.exists(), f'rc={rc}')
            continue
        rc, screen = r.menu(f'{digit}\n5\n0\n', f'menu {digit} cancelled with a wrong digit')
        r.check(f'menu {digit}: wrong digit cancels', 'Cancelled; nothing changed.' in screen and marker.exists() == start)
        rc, screen = r.menu(f'{digit}\n{digit}\n0\n', f'menu {digit} confirmed')
        r.check(f'menu {digit}: confirmed flips {name}', rc == 0 and marker.exists() != start, f'rc={rc}')
        r.check(f'menu {digit}: no staging file left', not list(r.base.glob(f'{name}.new.*')))
        screen, block = r.status(f'menu {digit} switched')
        r.check(f'menu {digit}: menu 2 still prints a verdict', bool(block))
        rc, screen = r.menu(f'{digit}\n{digit}\n0\n', f'menu {digit} confirmed again')
        r.check(f'menu {digit}: second confirmation flips back', rc == 0 and marker.exists() == start, f'rc={rc}')
        if marker.exists():
            marker.unlink()
        marker.mkdir()
        rc, screen = r.menu(f'{digit}\n{digit}\n0\n', f'menu {digit} with a directory marker')
        r.check(f'menu {digit}: directory marker reported and kept', marker.is_dir() and 'menu 6 removes it' in screen)
        marker.rmdir()
        if start:
            marker.write_text('off\n')
    r.menu('4\n', 'menu 4 uninstall')
    r.autostart_restored('after menu 4')
    return r.summary('G7 menus')


if __name__ == '__main__':
    sys.exit(main())
