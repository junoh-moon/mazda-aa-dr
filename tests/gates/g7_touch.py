#!/usr/bin/env python3
"""G7 touch-mod (third-party libpatch) compatibility on a vehicle-file replica.

Tracked port of the beta.17 release harness (touch_sim). The replica is the
stock-BusyBox root of emu.py with /jci/sm/sm.conf, /jci/sm/sm_WCP.conf,
/usr/bin/autostart and /jci/version.ini replaced by the owner's exported
vehicle bytes, plus the oem-aa-mod folder (published libpatch DSOs and the
vehicle's libpatch.conf). Private inputs are only referenced by path and are
never copied into the repository or the transcript summary.

  MX5DR_VEHICLE_EXPORT=DIR MX5DR_LIBPATCH_DIR=DIR \
    python3 tests/gates/g7_touch.py --bundle ZIP|DIR --scenario A|B

Scenario A: vehicle files without any mx5dr state (the oldest backup's
autostart.before). Scenario B: upgrade from the vehicle's exported shadow.5
one-boot state (current autostart and the exported mx5-aa-dr tree).
Checks: preload chains per service (ours first, libpatch kept, one
LD_PRELOAD per service), trial minus our three tokens equals the vehicle
file, autostart minus the owned blocks equals the vehicle file, normal and WCP
branches, a touch-mod edit of sm.conf/sm_WCP.conf falls back to stock with the
reason and menu 1 adopts it, trip, menu 4 restores the vehicle autostart.
Exit 0 pass, 1 fail, 77 skip (no vehicle export or libpatch)."""
import argparse
import difflib
import json
import os
from pathlib import Path
import re
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu  # noqa: E402

AAPA = '/data_persist/mx5-aa-dr/libmx5dr.so:/data_persist/oem-aa-mod/libpatch-blmjciaapa.so'


def udiff(a, b, an, bn):
    return ''.join(difflib.unified_diff(a.decode().splitlines(True), b.decode().splitlines(True), an, bn, n=0))


def preload_lines(b):
    out, svc = {}, None
    for line in b.decode().splitlines():
        m = re.search(r'<service [^>]*name="([^"]+)"', line)
        if m:
            svc = m.group(1)
        m = re.search(r'env_name="LD_PRELOAD" env_value="([^"]*)"', line)
        if m:
            out.setdefault(svc, []).append(m.group(1))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bundle', default=os.environ.get('MX5DR_GATE_CANDIDATE'))
    ap.add_argument('--scenario', choices=('A', 'B'), default='A')
    ap.add_argument('--vehicle', default=os.environ.get('MX5DR_VEHICLE_EXPORT'))
    ap.add_argument('--libpatch', default=os.environ.get('MX5DR_LIBPATCH_DIR'))
    a = ap.parse_args()
    if not a.bundle:
        emu.skip('--bundle (or MX5DR_GATE_CANDIDATE) is required')
    if not a.vehicle or not Path(a.vehicle, 'jci/sm/sm.conf').is_file():
        emu.skip('MX5DR_VEHICLE_EXPORT unset or not a vehicle export tree (private)')
    if not a.libpatch or not Path(a.libpatch, 'libpatch-blmjciaapa.so').is_file():
        emu.skip('MX5DR_LIBPATCH_DIR unset or without libpatch-blmjciaapa.so')
    stock, proot, qemu, work = emu.environment()
    bundle = emu.unpack(a.bundle, work / 'g7touch-bundle')
    veh = Path(a.vehicle)
    vbase = veh / 'tmp/mnt/data_persist/mx5-aa-dr'
    backups = sorted(vbase.glob('backups/*/autostart.before'))
    conf = sorted(veh.glob('tmp/mnt/sda1/mx5dr-diagnostics-*/oem-aa-mod/libpatch.conf.txt'))
    s5 = sorted(veh.glob('tmp/mx5dr-trial-*/sm.conf'))
    if not backups or not conf:
        emu.skip('vehicle export lacks backups/*/autostart.before or the libpatch.conf copy')
    v_sm, v_wcp = (veh / 'jci/sm/sm.conf').read_bytes(), (veh / 'jci/sm/sm_WCP.conf').read_bytes()
    v_auto_clean, v_auto_now = backups[0].read_bytes(), (veh / 'usr/bin/autostart').read_bytes()
    v_version = (veh / 'jci/version.ini').read_bytes()
    v_s5 = s5[0].read_bytes() if s5 else None
    upgrade = a.scenario == 'B'

    r = emu.Replica(work, 'g7-touch-' + a.scenario, stock, proot, qemu)
    r.build(bundle, a.libpatch)
    oem = r.root / 'tmp/mnt/data_persist/oem-aa-mod'
    shutil.copyfile(conf[0], oem / 'libpatch.conf')
    oem_src = {p.name: p.read_bytes() for p in oem.iterdir()}
    r.emit(f'stock version.ini == vehicle version.ini: {(r.root / "jci/version.ini").read_bytes() == v_version}')
    (r.root / 'jci/sm/sm.conf').write_bytes(v_sm)
    (r.root / 'jci/sm/sm_WCP.conf').write_bytes(v_wcp)
    (r.root / 'jci/version.ini').write_bytes(v_version)
    (r.root / 'usr/bin/autostart').write_bytes(v_auto_now if upgrade else v_auto_clean)
    if upgrade:
        shutil.copytree(vbase, r.base, symlinks=True)
    r.original.update({'jci/sm/sm.conf': v_sm, 'jci/sm/sm_WCP.conf': v_wcp, 'jci/version.ini': v_version,
                       'usr/bin/autostart': v_auto_clean})
    B = r.boot_id

    def oem_files_ok(tag):
        r.check(f'{tag}: sm.conf byte-identical to vehicle', (r.root / 'jci/sm/sm.conf').read_bytes() == v_sm)
        r.check(f'{tag}: sm_WCP.conf byte-identical to vehicle', (r.root / 'jci/sm/sm_WCP.conf').read_bytes() == v_wcp)
        r.check(f'{tag}: oem-aa-mod folder untouched',
                all((oem / n).read_bytes() == b for n, b in oem_src.items()))

    def analyse(trial, base, tag, s5=None):
        d = udiff(base, trial, 'vehicle', 'trial')
        (r.out / f'diff-{tag}.txt').write_text(d)
        pl = preload_lines(trial)
        r.emit(f'--- LD_PRELOAD per service [{tag}] --- ' + json.dumps(pl))
        r.check(f'{tag}: jciAAPA preload = libmx5dr.so:libpatch-blmjciaapa.so', pl.get('jciAAPA') == [AAPA])
        r.check(f'{tag}: jciLDS one preload = ldstap', pl.get('jciLDS') == ['/data_persist/mx5-aa-dr/libmx5dr-ldstap.so'])
        r.check(f'{tag}: jciVBS one preload = vimtap', pl.get('jciVBS') == ['/data_persist/mx5-aa-dr/libmx5dr-vimtap.so'])
        base_pl = preload_lines(base)
        for svc in ('aap_service', 'jcinavi'):
            r.check(f'{tag}: {svc} preload untouched', pl.get(svc) == base_pl.get(svc), str(pl.get(svc)))
        r.check(f'{tag}: no service has two LD_PRELOAD', all(len(v) == 1 for v in pl.values()))
        t = trial.decode().replace('/data_persist/mx5-aa-dr/libmx5dr.so:', '')
        t = re.sub(r'            <environ_var env_name="LD_PRELOAD" env_value="/data_persist/mx5-aa-dr/libmx5dr-(vimtap|ldstap)\.so"/>\n', '', t)
        r.check(f'{tag}: trial minus our 3 tokens == vehicle file', t.encode() == base)
        added = [x for x in d.splitlines() if x.startswith('+') and not x.startswith('+++')]
        removed = [x for x in d.splitlines() if x.startswith('-') and not x.startswith('---')]
        r.check(f'{tag}: diff is 3 added / 1 changed line only', len(added) == 3 and len(removed) == 1,
                f'+{len(added)} -{len(removed)}')
        if s5 is not None:
            r.check(f'{tag}: trial byte-identical to the shadow.5 trial the vehicle generated', trial == s5)

    if upgrade:
        r.emit('\n#### B0. Vehicle shadow.5 state, new boot, before upgrading')
        r.status('before upgrade')
        chosen = r.autostart_boot('shadow.5 blocks, consumed arm (stock expected)')
        r.check('B0: shadow.5 consumed state gives stock', chosen == '/jci/sm/sm.conf', chosen)
    r.emit('\n#### 1. Install persistent BETA (menu 1)')
    rc, screen = r.menu('1\n0\n', 'menu 1 install')
    r.check('install rc 0', rc == 0, f'rc={rc}')
    r.check('installer says persistent', 'Installed BETA persistent' in screen)
    oem_files_ok('after install')
    auto = (r.root / 'usr/bin/autostart').read_bytes()
    d = udiff(v_auto_clean, auto, 'vehicle autostart (pre-mx5dr)', 'installed')
    removed = [x for x in d.splitlines() if x.startswith('-') and not x.startswith('---')]
    r.check('autostart diff vs pre-mx5dr removes nothing', not removed, str(removed[:3]))
    r.check('installed autostart minus owned blocks == vehicle pre-mx5dr autostart', emu.strip_blocks(auto) == v_auto_clean)
    r.check('exactly two owned blocks', auto.decode().count(emu.BEGIN) == 2)
    r.check('normal.source.sha256 = vehicle sm.conf hash',
            (r.base / 'guard/normal.source.sha256').read_text().strip() == emu.sha(v_sm))
    r.check('wcp.source.sha256 = vehicle sm_WCP.conf hash',
            (r.base / 'guard/wcp.source.sha256').read_text().strip() == emu.sha(v_wcp))
    r.status('install boot')

    r.emit('\n#### 2. Product boot, normal branch')
    chosen = r.autostart_boot('product, normal board')
    r.check('normal branch gets a /tmp trial', r.selected(chosen), chosen)
    first = None
    if r.selected(chosen):
        first = (r.root / chosen.lstrip('/')).read_bytes()
        analyse(first, v_sm, 'normal', v_s5)
    r.check('confirmed', r.persist_state().get('confirmed_boot') == B(r.boot_number), str(r.persist_state()))
    oem_files_ok('normal boot')
    screen, block = r.status('normal boot')
    r.check('PERSIST enabled', 'PERSIST enabled (every boot)' in block)

    r.emit('\n#### 3. Product boot, WCP branch')
    chosen = r.autostart_boot('product, WCP board', var='SMCFG_WCPMODE')
    r.check('WCP branch gets a /tmp trial', r.selected(chosen), chosen)
    if r.selected(chosen):
        analyse((r.root / chosen.lstrip('/')).read_bytes(), v_wcp, 'wcp')
    oem_files_ok('WCP boot')

    r.emit('\n#### 4. More product boots give the same trial')
    for _ in range(3):
        chosen = r.autostart_boot('product')
        r.check(f'boot {r.boot_number}: identical trial', r.selected(chosen) and first is not None and
                (r.root / chosen.lstrip('/')).read_bytes() == first, chosen)

    r.emit('\n#### 5. Touch mod rewrites sm.conf with identical bytes')
    (r.root / 'jci/sm/sm.conf').write_bytes(v_sm)
    chosen = r.autostart_boot('identical rewrite')
    r.check('identical rewrite keeps product', r.selected(chosen), chosen)

    r.emit('\n#### 6. Touch mod edits sm.conf later (adds a libpatch preload)')
    old = b'<environ_var env_name="LD_PRELOAD" env_value="/data_persist/oem-aa-mod/libpatch-blmjciaapa.so"/>'
    edited = v_sm.replace(old, old.replace(b'blmjciaapa.so"', b'blmjciaapa.so:/data_persist/oem-aa-mod/libpatch-extra.so"'))
    if edited == v_sm:
        r.skipped('touch-mod edit scenario', 'vehicle sm.conf has no libpatch-blmjciaapa preload line')
    else:
        (r.root / 'jci/sm/sm.conf').write_bytes(edited)
        screen, block = r.status('sm.conf edited by touch mod')
        r.check('menu 2 shows bindings changed', 'PERSIST enabled but bindings changed' in block)
        chosen = r.autostart_boot('after touch-mod edit')
        r.check('stock config after the edit', chosen == '/jci/sm/sm.conf', chosen)
        r.check('SM sees the touch edit unchanged', (r.root / 'jci/sm/sm.conf').read_bytes() == edited)
        rc, screen = r.menu('1\n0\n', 'menu 1 re-enable after touch edit')
        r.check('re-enable rc 0', rc == 0, f'rc={rc}')
        chosen = r.autostart_boot('product after re-enable')
        r.check('product again', r.selected(chosen), chosen)
        if r.selected(chosen):
            pl = preload_lines((r.root / chosen.lstrip('/')).read_bytes())
            r.check('jciAAPA = ours first + edited libpatch chain',
                    pl.get('jciAAPA') == [AAPA + ':/data_persist/oem-aa-mod/libpatch-extra.so'], str(pl.get('jciAAPA')))
        (r.root / 'jci/sm/sm.conf').write_bytes(v_sm)
        r.menu('1\n0\n', 'menu 1 re-enable with the original sm.conf')
    r.emit('\n#### 6c. Touch mod edits only sm_WCP.conf')
    chosen = r.autostart_boot('product')
    r.check('product', r.selected(chosen), chosen)
    (r.root / 'jci/sm/sm_WCP.conf').write_bytes(v_wcp + b'<!-- touch mod edit -->\n')
    screen, block = r.status('sm_WCP.conf edited')
    r.check('menu 2 names sm_WCP.conf', 'sm_WCP.conf edited by another tool' in ' '.join(x.strip() for x in block))
    chosen = r.autostart_boot('after WCP-only edit, normal board')
    r.check('normal board falls to stock after a WCP-only edit', chosen == '/jci/sm/sm.conf', chosen)
    (r.root / 'jci/sm/sm_WCP.conf').write_bytes(v_wcp)
    r.menu('1\n0\n', 'menu 1 re-enable with the original sm_WCP.conf')
    chosen = r.autostart_boot('product')
    r.check('product', r.selected(chosen), chosen)
    oem_files_ok('after re-enable')

    r.emit('\n#### 7. Trip: two consecutive SM resets')
    for n in range(2):
        for name in ('thread_info.out', 'ps_info.out', 'dmesg.out'):
            (r.root / 'tmp/mnt/data' / name).write_text(f'SM reset report {name} {n}\n')
        chosen = r.autostart_boot(f'after reset {n + 1}')
    r.check('tripped -> stock sm.conf', chosen == '/jci/sm/sm.conf' and
            r.persist_state().get('tripped') == 'reset_reports', chosen + ' ' + str(r.persist_state()))
    chosen = r.autostart_boot('tripped WCP branch', var='SMCFG_WCPMODE')
    r.check('tripped WCP -> stock sm_WCP.conf', chosen == '/jci/sm/sm_WCP.conf', chosen)
    oem_files_ok('tripped')

    r.emit('\n#### 8. Uninstall (menu 4)')
    rc, screen = r.menu('4\n', 'menu 4 uninstall')
    r.check('uninstall rc 0', rc == 0, f'rc={rc}')
    oem_files_ok('after uninstall')
    r.check('autostart after uninstall == vehicle pre-mx5dr autostart',
            (r.root / 'usr/bin/autostart').read_bytes() == v_auto_clean)
    chosen = r.autostart_boot('after uninstall')
    r.check('stock after uninstall', chosen == '/jci/sm/sm.conf', chosen)
    return r.summary('G7 touch ' + a.scenario)


if __name__ == '__main__':
    sys.exit(main())
