#!/usr/bin/env python3
"""G9 power-loss gate: cut the power inside menu 1 (install) and menu 4
(uninstall) at every mutating step, then prove the next boot is either stock
or a complete product and that digits alone recover (menu 4, then menu 1).

  python3 tests/gates/g9_powerloss.py --candidate CAND.zip [--libpatch DIR] \
      [--phases install,uninstall] [--stride N] [--recover-every K] [--max-points M]

Injection (stock-BusyBox replica, see emu.py): /test-bin is first on the
guest PATH. Every mutating command the shell scripts run by name (mv cp rm ln
mkdir rmdir chmod chown touch sync mktemp dd tar) goes through a counting
wrapper. A reference run without a cut numbers the steps. Cut points
(--select persistent, default) are the steps with a path argument on
persistent storage, or sync: a cut before a step that only touches tmpfs /tmp
(cleared at boot) leaves the same persistent state as a cut before the next
persistent step. --select all cuts before every step. For cut point k the
wrapper of step k writes /powercut and blocks before the command runs; the
driver then SIGKILLs the whole guest process group (PRoot, qemu, every shell),
which is what a power loss does to processes. The replica then gets a new
boot: /tmp is cleared, a new boot ID, the installed autostart block runs with
the real ARM guard.

Per cut point the gate asserts:
 - SM gets either the stock config or a trial with all three product preloads
   (never a missing or partial config);
 - /usr/bin/autostart passes the stock BusyBox `sh -n`;
 - every stock file is byte-identical (autostart: after removing complete
   owned blocks);
 - recovery with digits only: menu 4 (rc 0) leaves the stock autostart
   (trailing newline tolerated) and a stock boot; then menu 1 (rc 0), menu 5
   and the next boot select a complete product (every --recover-every point).
Not simulated: a cut inside one command (a half-written file from a shell
redirection or inside the ARM guard/hash helper), storage that loses recently
written data without fsync, and the runtime store/token writes of the C
worker (unit tests cover those). Exit 0 pass, 1 fail, 77 skip."""
import argparse
import os
import shutil
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu  # noqa: E402

WRAP = 'mv cp rm ln mkdir rmdir chmod chown touch sync mktemp dd tar'.split()
WRAPPER = '''#!/bin/sh
n=0
[ -f /powercut.count ] && read n < /powercut.count
n=$((n+1))
echo "$n" > /powercut.count
echo "$n @NAME@ $*" >> /powercut.log
if [ -f /powercut.at ]; then
  read at < /powercut.at
  if [ "$n" = "$at" ]; then echo "$n @NAME@ $*" > /powercut; exec /bin/sleep 600; fi
fi
exec @INNER@ "$@"
'''


def install_wrappers(r):
    tb = r.root / 'test-bin'
    inner = tb / '.inner'
    inner.mkdir(exist_ok=True)
    for name in WRAP:
        target = None
        if (tb / name).exists():
            (tb / name).rename(inner / name)
            target = f'/test-bin/.inner/{name}'
        else:
            for d in ('bin', 'usr/bin', 'sbin', 'usr/sbin'):
                if os.path.lexists(r.root / d / name):
                    target = f'/{d}/{name}'
                    break
        if target is None:
            continue
        (tb / name).write_text(WRAPPER.replace('@NAME@', name).replace('@INNER@', target))
        (tb / name).chmod(0o755)


PERSISTENT = ('/tmp/mnt/data_persist', '/data_persist', '/mnt/data_persist', '/jci', '/usr/bin', '/tmp/mnt/data/',
              '/data/', '/etc', '/config-mfg')


def persistent_step(line):
    """A step that may change storage surviving a power loss: a path argument
    on persistent storage (not tmpfs /tmp, not the read-only USB source), or
    sync. A cut before a tmpfs-only step leaves the same persistent state as
    a cut before the next persistent step, so only these are cut points."""
    parts = line.split()
    if len(parts) < 2:
        return False
    if parts[1] == 'sync':
        return True
    return any(p.startswith(PERSISTENT) for p in parts[2:])


def reset_counter(r, at=None):
    for name in ('powercut', 'powercut.count', 'powercut.log', 'powercut.at'):
        p = r.root / name
        if p.exists():
            p.unlink()
    if at is not None:
        (r.root / 'powercut.at').write_text(f'{at}\n')


def steps(r):
    p = r.root / 'powercut.log'
    return p.read_text().splitlines() if p.exists() else []


def classify_boot(r, tag):
    """Next boot after the cut: stock or complete product; autostart syntax; OEM identity."""
    rc, screen = r.sh('/bin/sh -n /usr/bin/autostart; echo "SYNTAX=$?"', f'{tag}: sh -n autostart')
    r.check(f'{tag}: autostart passes sh -n', 'SYNTAX=0' in screen)
    chosen = r.autostart_boot(f'{tag}: first boot after the power cut')
    kind = 'stock' if chosen == '/jci/sm/sm.conf' else 'product' if r.trial_complete(chosen) else 'BROKEN'
    r.check(f'{tag}: next boot is stock or a complete product ({kind})', kind != 'BROKEN', chosen)
    r.oem_identical(f'{tag}')
    return kind


def recover(r, tag, full):
    rc, screen = r.menu('4\n', f'{tag}: recovery menu 4')
    r.check(f'{tag}: recovery menu 4 rc 0', rc == 0, f'rc={rc}')
    r.autostart_restored(f'{tag}: after menu 4')
    chosen = r.autostart_boot(f'{tag}: boot after menu 4')
    r.check(f'{tag}: stock boot after menu 4', chosen == '/jci/sm/sm.conf', chosen)
    if not full:
        return
    rc, screen = r.menu('1\n0\n', f'{tag}: recovery menu 1')
    r.check(f'{tag}: recovery menu 1 rc 0 and persistent', rc == 0 and 'Installed BETA persistent' in screen,
            f'rc={rc}')
    r.menu('5\n', f'{tag}: menu 5 (witness)')
    chosen = r.autostart_boot(f'{tag}: product boot after recovery')
    r.check(f'{tag}: complete product after menu 4 + menu 1', r.trial_complete(chosen), chosen)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--candidate', default=os.environ.get('MX5DR_GATE_CANDIDATE'))
    ap.add_argument('--libpatch', default=os.environ.get('MX5DR_LIBPATCH_DIR'))
    ap.add_argument('--phases', default=os.environ.get('MX5DR_G9_PHASES', 'install,uninstall'))
    ap.add_argument('--select', default=os.environ.get('MX5DR_G9_SELECT', 'persistent'),
                    choices=('persistent', 'all'))
    ap.add_argument('--stride', type=int, default=int(os.environ.get('MX5DR_G9_STRIDE', '1')))
    ap.add_argument('--recover-every', type=int, default=int(os.environ.get('MX5DR_G9_RECOVER_EVERY', '1')))
    ap.add_argument('--max-points', type=int, default=int(os.environ.get('MX5DR_G9_MAX_POINTS', '0')))
    a = ap.parse_args()
    if not a.candidate:
        emu.skip('--candidate (or MX5DR_GATE_CANDIDATE) is required')
    stock, proot, qemu, work = emu.environment()
    cand = emu.unpack(a.candidate, work / 'g9-candidate')
    base = emu.Replica(work, 'g9-base', stock, proot, qemu)
    base.emit(f'G9 power loss: candidate={Path(a.candidate).name} phases={a.phases} stride={a.stride} '
              f'recover_every={a.recover_every} max_points={a.max_points or "all"}')
    base.build(cand, a.libpatch)
    install_wrappers(base)
    results = []

    def run_phase(phase, ready, keys, label):
        ref = ready.clone(f'g9-{phase}-ref')
        reset_counter(ref)
        rc, screen = ref.menu(keys, f'{phase}: reference run without a cut ({label})')
        base.check(f'{phase}: reference run rc 0', rc == 0, f'rc={rc}')
        listed = steps(ref)
        (work / f'g9-{phase}-steps.txt').write_text('\n'.join(listed) + '\n')
        base.emit(f'{phase}: {len(listed)} mutating steps in the reference run')
        if a.select == 'persistent':
            candidates = [i + 1 for i, line in enumerate(listed) if persistent_step(line)]
        else:
            candidates = list(range(1, len(listed) + 1))
        points = candidates[::max(1, a.stride)]
        base.emit(f'{phase}: {len(candidates)} candidate cut points ({a.select}), {len(points)} selected')
        if a.max_points:
            points = points[:a.max_points]
        for i, k in enumerate(points):
            t0 = time.monotonic()
            r = ready.clone(f'g9-{phase}-{k:03d}')
            reset_counter(r, at=k)
            rc, screen = r.menu(keys, f'{phase}: cut at step {k}: {listed[k - 1] if k <= len(listed) else "?"}',
                                watch=r.root / 'powercut')
            cut = rc is None
            r.check(f'{phase} k={k}: power cut happened inside the menu', cut, f'rc={rc}')
            reset_counter(r)
            kind = classify_boot(r, f'{phase} k={k}')
            recover(r, f'{phase} k={k}', full=(i % max(1, a.recover_every) == 0))
            fails = [n for n, ok, sk in r.findings if not ok and not sk]
            results.append((phase, k, listed[k - 1] if k <= len(listed) else '?', kind, not fails))
            base.emit(f'RESULT {phase} k={k:3d} next_boot={kind:7s} {"PASS" if not fails else "FAIL"} '
                      f'({time.monotonic() - t0:.0f}s) {listed[k - 1][:70] if k <= len(listed) else ""}')
            for n in fails:
                base.emit(f'  FAILED: {n}')
            r.log.close()
            if not fails:
                shutil.rmtree(r.root, ignore_errors=True)  # keep failing roots only
        return ref

    phases = [p for p in a.phases.split(',') if p]
    if 'install' in phases:
        run_phase('install', base, '1\n0\n', 'menu 1 on a stock replica')
    if 'uninstall' in phases:
        inst = base.clone('g9-installed')
        reset_counter(inst)
        rc, screen = inst.menu('1\n0\n', 'prepare: menu 1')
        inst.menu('5\n', 'prepare: menu 5')
        chosen = inst.autostart_boot('prepare: product boot')
        base.check('uninstall phase prepared: product running', rc == 0 and inst.trial_complete(chosen), chosen)
        reset_counter(inst)
        run_phase('uninstall', inst, '4\n', 'menu 4 on a running product')
    total = len(results)
    ok = sum(1 for x in results if x[4])
    kinds = {}
    for x in results:
        kinds[(x[0], x[3])] = kinds.get((x[0], x[3]), 0) + 1
    base.emit(f'\nG9 cut points: {ok}/{total} passed; next-boot kinds: ' +
              ', '.join(f'{p}:{k}={n}' for (p, k), n in sorted(kinds.items())))
    base.check('every cut point passed', ok == total and total > 0, f'{ok}/{total}')
    return base.summary('G9')


if __name__ == '__main__':
    sys.exit(main())
