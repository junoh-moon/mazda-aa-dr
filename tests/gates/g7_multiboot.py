#!/usr/bin/env python3
"""G7 persistent multi-boot simulation on a stock-BusyBox replica (see emu.py).

Tracked port of the beta.17 release harness (persist_sim). Boots are authored
boot_id changes with /tmp cleared; SM resets are authored /data/*.out
rewrites; the installed ARM guard runs through the installed owned autostart
block (confirm after 3 s instead of 90 s, collector replaced by a missing
path). No OEM PID 1, SM, AA or VBS process is executed.

  python3 tests/gates/g7_multiboot.py --bundle ZIP|DIR

Scenarios: install (menu 1) and the fenced installing boot; confirmed product
boots; same-boot double selection refused; two consecutive SM resets trip to
stock; menu 3 export keeps the tripped state, menu 1 keeps the evidence and
re-enables; probation trip when SM never runs the trial; three unconfirmed
short boots trip; a binding edited by another tool is visible and the next
boot is stock with the reason; a hanging guard cannot block SM (stock
`timeout -t 15 -s KILL`); menu 4 restores the autostart byte for byte.
Exit 0 pass, 1 fail, 77 skip."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu  # noqa: E402


def reset_reports(r, tag):
    data = r.root / 'tmp/mnt/data'
    for name in ('thread_info.out', 'ps_info.out', 'top_info.out', 'meminfo.out',
                 'free_info.out', 'df_info.out', 'dmesg.out'):
        (data / name).write_text(f'SM reset report {name} written during {tag}\n')
    r.emit(f'(authored SM reset: /data/*.out rewritten during {tag})')


def width_ok(r, tag, block):
    r.check(f'{tag}: verdict lines <= 40 chars', block and max(len(x) for x in block) <= 40,
            f'widest={max((len(x) for x in block), default=0)}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bundle', default=os.environ.get('MX5DR_GATE_CANDIDATE'))
    ap.add_argument('--libpatch', default=os.environ.get('MX5DR_LIBPATCH_DIR'))
    a = ap.parse_args()
    if not a.bundle:
        emu.skip('--bundle (or MX5DR_GATE_CANDIDATE) is required')
    stock, proot, qemu, work = emu.environment()
    bundle = emu.unpack(a.bundle, work / 'g7mb-bundle')
    r = emu.Replica(work, 'g7-multiboot', stock, proot, qemu)
    r.build(bundle, a.libpatch)
    emu.h.put(r.root, '/hang-guard', '#!/bin/sh\nexec /bin/sleep 100\n', 0o755)
    original_autostart = r.original['usr/bin/autostart']
    B = r.boot_id

    r.emit('\n#### 1. Install persistent BETA (menu 1)')
    rc, screen = r.menu('0\n', 'first screen')
    r.check('menu shows the persistent BETA install line', '1 Install or re-enable BETA, every boot' in screen)
    rc, screen = r.menu('1\n0\n', 'menu 1 install')
    install_boot = r.boot_number
    r.check('install rc 0', rc == 0, f'rc={rc}')
    r.check('installer says persistent', 'Installed BETA persistent: the guard starts it on every CMU boot.' in screen)
    persist = r.base / 'guard/persist'
    r.check('real ARM guard wrote persist', persist.is_file() and persist.stat().st_size > 0,
            f'{persist.stat().st_size if persist.is_file() else 0} bytes')
    st = r.persist_state()
    r.check('state fences the installing boot, probation', st.get('enabled_boot') == B(install_boot)
            and st.get('probation') == 'yes', str(st))
    for name in ('jci/sm/sm.conf', 'jci/sm/sm_WCP.conf'):
        r.check(f'{name} carries no package preload', 'mx5-aa-dr' not in (r.root / name).read_text())
    auto = (r.root / 'usr/bin/autostart').read_text()
    r.check('installed autostart has the timeout wrapper',
            '/usr/bin/timeout -t 15 -s KILL /data_persist/mx5-aa-dr/guard/mx5dr-guard select' in auto)
    r.check('installed autostart backgrounds the confirm',
            "( trap '' HUP; /bin/sleep 90; exec /data_persist/mx5-aa-dr/guard/mx5dr-guard confirm ) </dev/null >/dev/null 2>&1 &"
            in auto)
    screen, block = r.status('install boot')
    width_ok(r, 'install boot', block)
    r.check('install boot: NO BOOT installed; choose 5', 'NO   BOOT  installed; choose 5' in block)
    chosen = r.autostart_boot('SM restart in the installing boot', new_boot=False)
    r.check('installing boot: SM gets stock config', chosen == '/jci/sm/sm.conf', chosen)
    rc, screen = r.menu('5\n', 'menu 5 reboot (witness)')
    r.check('reboot witness', (r.root / 'reboot.witness').exists())

    r.emit('\n#### 2. Product boots, confirmed by the background confirm')
    for _ in (1, 2):
        chosen = r.autostart_boot('product')
        n = r.boot_number
        r.check(f'boot {n}: SM gets a complete /tmp trial', r.trial_complete(chosen), chosen)
        r.check(f'boot {n}: confirmed by the real ARM guard', r.persist_state().get('confirmed_boot') == B(n),
                str(r.persist_state()))
        screen, block = r.status(f'boot {n}')
        width_ok(r, f'boot {n}', block)
        r.check(f'boot {n}: PERSIST enabled', 'PERSIST enabled (every boot)' in block)
        r.check(f'boot {n}: ok BOOT/CONF', 'ok   BOOT  product this boot' in block and 'ok   CONF  confirmed' in block)
    r.check('probation ended after the first confirmed boot', r.persist_state().get('probation') == 'no')
    chosen = r.autostart_boot('same Linux boot again (SM restart)', new_boot=False)
    r.check('same boot again: stock (same-boot double selection refused)', chosen == '/jci/sm/sm.conf', chosen)

    r.emit('\n#### 3. Two consecutive SM resets trip to stock')
    reset_reports(r, f'boot {r.boot_number}')
    chosen = r.autostart_boot('after one reset')
    r.check('after one reset: still product', r.selected(chosen), chosen)
    st = r.persist_state()
    r.check('after one reset: fail_count 1, previous failed_reset',
            (st.get('fail_count'), st.get('previous')) == ('1', 'failed_reset'), str(st))
    reset_reports(r, f'boot {r.boot_number}')
    chosen = r.autostart_boot('after a second consecutive reset')
    r.check('after two resets: stock config (tripped)', chosen == '/jci/sm/sm.conf', chosen)
    r.check('tripped=reset_reports', r.persist_state().get('tripped') == 'reset_reports', str(r.persist_state()))
    screen, block = r.status('tripped')
    width_ok(r, 'tripped', block)
    r.check('tripped: PERSIST tripped line', 'PERSIST tripped (reset_reports)' in block)
    r.check('tripped: stock this boot with the reason', 'NO   BOOT  stock this boot' in block
            and ' (tripped:reset_reports)' in block)
    r.check('tripped: explicit NO-GO trip lines', block[-2:] == ['NO-GO: tripped, stock runs, BETA off.',
                                                                'Find the cause, then run menu 1 again.'], str(block[-2:]))
    chosen = r.autostart_boot('stays tripped')
    r.check('next boot: still stock', chosen == '/jci/sm/sm.conf', chosen)

    r.emit('\n#### 4. Export (menu 3), then re-enable (menu 1) keeps the evidence')
    rc, screen = r.menu('3\n', 'menu 3 export')
    result = r.root / 'tmp/mnt/sda1/trial-result.txt'
    r.check('export saved the tripped state', result.exists() and
            'guard.persist_state.line_14=tripped=reset_reports' in result.read_text())
    archives = sorted((r.root / 'tmp/mnt/sda1').glob('mx5dr-logs-*.tar'))
    members = subprocess.run(['tar', 'tf', str(archives[-1])], capture_output=True, text=True).stdout if archives else ''
    r.check('export archive carries the SM reset reports', '/sm-reports/dmesg.out.txt' in members)
    rc, screen = r.menu('1\n0\n', 'menu 1 re-enable')
    r.check('menu 1 kept the previous state', 'Kept the previous BETA state in backups/persist-evidence/1 (tripped: reset_reports).' in screen)
    r.check('menu 1 advises export first', 'BETA was tripped (reset_reports). Run menu 3 export first' in screen)
    ev = r.base / 'backups/persist-evidence/1/persist-state'
    r.check('evidence copy present', ev.exists() and 'tripped=reset_reports' in ev.read_text())
    st = r.persist_state()
    r.check('re-enable: tripped no, probation yes, fenced', (st.get('tripped'), st.get('probation'), st.get('enabled_boot'))
            == ('no', 'yes', B(r.boot_number)), str(st))

    r.emit('\n#### 5. Probation: SM rejects the trial (no report, no confirm) -> one product boot only')
    chosen = r.autostart_boot('SM never runs with the trial', sm_runs=False)
    r.check('probation: product selected (first attempt)', r.selected(chosen), chosen)
    r.check('probation: not confirmed', r.persist_state().get('confirmed_boot') == 'none')
    chosen = r.autostart_boot('after an unconfirmed probation boot')
    r.check('probation trip: stock', chosen == '/jci/sm/sm.conf', chosen)
    r.check('tripped=probation', r.persist_state().get('tripped') == 'probation', str(r.persist_state()))

    r.emit('\n#### 6. Short boots without confirmation: 3 in a row trip')
    r.menu('1\n0\n', 'menu 1 re-enable')
    chosen = r.autostart_boot('confirmed first boot')
    r.check('confirmed first boot: product and confirmed', r.selected(chosen) and
            r.persist_state().get('confirmed_boot') == B(r.boot_number))
    picks = [r.selected(r.autostart_boot('ignition off before the confirmation', sm_runs=False)) for _ in range(3)]
    r.check('three unconfirmed boots: product each time', picks == [True, True, True], str(picks))
    chosen = r.autostart_boot('after three unconfirmed boots')
    r.check('then stock, tripped=unconfirmed', chosen == '/jci/sm/sm.conf' and
            r.persist_state().get('tripped') == 'unconfirmed', str(r.persist_state()))
    screen, block = r.status('tripped unconfirmed')
    r.check('PERSIST tripped (unconfirmed)', 'PERSIST tripped (unconfirmed)' in block)

    r.emit('\n#### 7. Bindings changed by another tool are visible; next boot says why')
    r.menu('1\n0\n', 'menu 1 re-enable')
    chosen = r.autostart_boot('product')
    r.check('product again', r.selected(chosen), chosen)
    sm = r.root / 'jci/sm/sm.conf'
    saved = sm.read_bytes()
    sm.write_bytes(saved + b'<!-- another tool -->\n')
    screen, block = r.status('sm.conf edited')
    r.check('PERSIST enabled but bindings changed', 'PERSIST enabled but bindings changed' in block)
    r.check('message names sm.conf', 'sm.conf edited by another tool' in ' '.join(x.strip() for x in block))
    r.check('NO PERS bindings changed', 'NO   PERS  bindings changed' in block)
    chosen = r.autostart_boot('after the edit')
    r.check('after the edit: stock', chosen == '/jci/sm/sm.conf', chosen)
    screen, block = r.status('stock after edit')
    r.check('reason shown', ' (baseline_edited:sm.conf)' in block, str(block[:12]))
    sm.write_bytes(saved)

    r.emit('\n#### 8. A hanging guard cannot block the SM (stock BusyBox timeout -t 15 -s KILL)')
    import time
    t0 = time.monotonic()
    chosen = r.autostart_boot('guard replaced by a hanging stub', guard_stub='/hang-guard')
    elapsed = time.monotonic() - t0
    r.check('hanging guard: SM gets stock after the bound', chosen == '/jci/sm/sm.conf', chosen)
    r.check('hanging guard: bounded (< 60 s wall under proot+qemu)', elapsed < 60, f'{elapsed:.1f}s')

    r.emit('\n#### 9. Uninstall (menu 4) returns to stock')
    r.menu('4\n', 'menu 4 uninstall')
    r.check('uninstall removed persist', not persist.exists())
    r.check('autostart restored byte for byte', (r.root / 'usr/bin/autostart').read_bytes() == original_autostart)
    chosen = r.autostart_boot('after uninstall')
    r.check('after uninstall: stock', chosen == '/jci/sm/sm.conf', chosen)
    r.oem_identical('after uninstall', allow_autostart_blocks=False)
    return r.summary('G7 multiboot')


if __name__ == '__main__':
    sys.exit(main())
