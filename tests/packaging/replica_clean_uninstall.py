#!/usr/bin/env python3
"""Clean-uninstall FILE TREE equality on a stock BusyBox 1.19.2 replica root.

Not part of `make test`: it needs the private stock firmware tree, a published
USB bundle (unpacked), proot and qemu-arm, and optionally the owner's private
vehicle export. Nothing private is copied into the repository or its output.

The replica is built by cmu_emulation.make_root (stock BusyBox, libc and
firmware-hashed files) with the vehicle's sm.conf, sm_WCP.conf, version.ini and
pre-mx5dr autostart written over the stock copies, plus an oem-aa-mod folder.
The bundle's packaging scripts are replaced by this checkout's (the ARM
payloads are the bundle's) and its SHA256SUMS is regenerated for the test.

  python3 tests/packaging/replica_clean_uninstall.py --stock DIR --bundle DIR \
      --work DIR --proot PROOT --qemu QEMU_ARM [--vehicle DIR --libpatch DIR]

Scenario A: clean vehicle state -> menu 1 (persistent BETA) -> product boots
(normal + WCP, real ARM guard through the owned autostart blocks) -> menu 3 ->
refusals -> menu 4 -> simulated reboot -> refusals -> menu 6 -> tree == before.
Scenario D: upgrade from the exported v0.3.12-shadow.5 tree (one-boot leftovers,
five backup sets incl. an interrupted one) -> same flow -> tree == clean tree.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import cmu_emulation as h  # noqa: E402

REPO = HERE.parents[1]
HELPERS = ('trial install.sh uninstall.sh purge.sh export_logs.sh common.sh edit_service.awk '
           'edit_autostart.awk arm.sh start_collector.sh stop_collector.sh finish_capture.sh '
           'trial_status.sh trial_status.awk startup_diagnostics.sh reboot_cmu.sh '
           'firmware.sha256 mx5dr.conf').split()
# Harness-authored paths, not CMU filesystem content (see validation record).
HARNESS = {'proc', 'test-bin', 'mount.calls', 'drv.sh', 'reboot.witness'}
STOCK_SM_PROCESS = {
    'maps': '00008000-00030000 r-xp 00000000 1f:02 1201 /jci/sm/sm\n'
            'b6e00000-b6f20000 r-xp 00000000 1f:02 88 /lib/libc-2.11.1.so\n',
    'fd': {'0': '/dev/null', '1': '/dev/null', '3': '/tmp/smevents.txt'},
}


class Run:
    def __init__(self, args, tag):
        self.args = args
        self.tag = tag
        self.out = Path(args.work) / ('out' + tag)
        self.root = Path(args.work) / ('root' + tag)
        for path in (self.out, self.root):
            if path.exists():
                shutil.rmtree(path)
        self.out.mkdir(parents=True)
        self.log = open(self.out / 'session.txt', 'w')
        self.step = 0
        self.findings = []
        self.base = self.root / 'tmp/mnt/data_persist/mx5-aa-dr'
        self.boot_number = 0

    def emit(self, text=''):
        print(text, flush=True)
        self.log.write(text + '\n')
        self.log.flush()

    def check(self, name, ok, detail=''):
        self.findings.append((name, bool(ok)))
        self.emit(f'CHECK {"PASS" if ok else "FAIL"}: {name}' + (f' -- {detail}' if detail else ''))

    def guest(self, argv, keys=None, label=''):
        self.step += 1
        env = h.guest_environment()
        env.pop('TMPDIR', None)
        start = time.monotonic()
        command = ['nice', '-n', '19', self.args.proot, '-0', '-q', self.args.qemu,
                   '-b', '/dev/null', '-w', '/', '-r', str(self.root)] + argv
        result = subprocess.run(command, env=env, input=keys, capture_output=True, text=True,
                                timeout=1800)
        screen = result.stdout + result.stderr
        (self.out / f'{self.step:02d}.screen.txt').write_text(screen)
        self.emit('=' * 78)
        self.emit(f'[{self.step:02d}] {label}')
        self.emit('guest command: ' + ' '.join(argv) + ('' if keys is None else '   keys: ' + json.dumps(keys)))
        self.emit(f'exit code: {result.returncode}   (elapsed {time.monotonic() - start:.1f}s under proot+qemu)')
        self.emit('--- screen (last 40 lines) ---')
        self.emit('\n'.join(screen.rstrip('\n').splitlines()[-40:]))
        return result.returncode, screen

    def menu(self, keys, label):
        return self.guest(['/bin/sh', '/tmp/mnt/sda1/trial'], keys, label)

    def sh(self, script, label):
        path = self.root / 'drv.sh'
        path.write_text(script + '\n')
        try:
            return self.guest(['/bin/sh', '/drv.sh'], None, label)
        finally:
            path.unlink()

    # --- authored kernel view ------------------------------------------------
    def boot_id(self, n):
        return '%08x-1234-4234-8234-0123456789ab' % (0x62000000 + n)

    def process(self, pid, cmdline, maps, fds):
        proc = self.root / 'proc' / str(pid)
        if proc.exists():
            shutil.rmtree(proc)
        (proc / 'fd').mkdir(parents=True)
        (proc / 'cmdline').write_bytes(cmdline.encode())
        (proc / 'maps').write_text(maps)
        for number, target in fds.items():
            (proc / 'fd' / number).symlink_to(target)

    def stock_processes(self, config):
        for entry in (self.root / 'proc').iterdir():
            if entry.name.isdigit():
                shutil.rmtree(entry)
        self.process(266, f'/jci/sm/sm\0-f\0{config}\0-e\0/tmp/smevents.txt\0',
                     STOCK_SM_PROCESS['maps'], STOCK_SM_PROCESS['fd'])

    def reboot(self, label):
        """New Linux boot: tmpfs /tmp loses everything except the mounts below
        /tmp/mnt, the kernel gets a new boot ID and fresh processes."""
        self.boot_number += 1
        for entry in (self.root / 'tmp').iterdir():
            if entry.name == 'mnt':
                continue
            if entry.is_dir() and not entry.is_symlink():
                shutil.rmtree(entry)
            else:
                entry.unlink()
        (self.root / 'tmp/smevents.txt').write_text('')
        h.put(self.root, '/proc/sys/kernel/random/boot_id', self.boot_id(self.boot_number) + '\n')
        h.put(self.root, '/proc/uptime', '300.00 0.00\n')
        self.stock_processes('/jci/sm/sm.conf')
        self.emit(f'(simulated CMU reboot {self.boot_number}: {label}; /tmp cleared, new boot ID)')

    def autostart_boot(self, label, var='SMCFG_NORMALMODE'):
        """Run the installed owned block (if any) for this board branch with
        the real ARM guard, then author the SM /proc entry it would start."""
        self.reboot(label)
        text = (self.root / 'usr/bin/autostart').read_text()
        begin = '# MX5DR ONE-BOOT BEGIN ' + var
        block = text[text.index(begin):text.index('# MX5DR ONE-BOOT END ' + var)] if begin in text else ''
        block = block.replace('/data_persist/mx5-aa-dr/tools/start_collector.sh', '/nonexistent-collector')
        block = block.replace('/bin/sleep 90;', '/bin/sleep 3;')
        default = '/jci/sm/sm.conf' if var == 'SMCFG_NORMALMODE' else '/jci/sm/sm_WCP.conf'
        launch = ("mkdir -p /proc/266\n"
                  "printf '/jci/sm/sm\\000-f\\000%s\\000-e\\000/tmp/smevents.txt\\000' \"$" + var + "\" > /proc/266/cmdline\n")
        script = f"{var}='{default}'\n" + block + f'echo "SM_CONFIG=${var}"\n' + launch + 'wait\n'
        rc, screen = self.sh(script, f'boot {self.boot_number}: autostart {var} ({label})')
        match = re.search(r'SM_CONFIG=(\S+)', screen)
        return match.group(1) if match else ''

    # --- replica ---------------------------------------------------------------
    def build(self, upgrade):
        args = self.args
        self.root.mkdir()
        real_mknod = os.mknod
        if os.geteuid() != 0:
            # proot binds the host /dev/null over this placeholder (-b /dev/null).
            os.mknod = lambda path, *a, **k: Path(path).write_text('')
        try:
            h.make_root(self.root, Path(args.stock), Path(args.bundle_dir))
        finally:
            os.mknod = real_mknod
        (self.root / 'sbin/reboot').unlink()
        h.put(self.root, '/sbin/reboot', '#!/bin/sh\necho called > /reboot.witness\n', 0o755)
        if args.vehicle:
            veh = Path(args.vehicle)
            vbase = veh / 'tmp/mnt/data_persist/mx5-aa-dr'
            (self.root / 'jci/sm/sm.conf').write_bytes((veh / 'jci/sm/sm.conf').read_bytes())
            (self.root / 'jci/sm/sm_WCP.conf').write_bytes((veh / 'jci/sm/sm_WCP.conf').read_bytes())
            (self.root / 'jci/version.ini').write_bytes((veh / 'jci/version.ini').read_bytes())
            clean = (vbase / 'backups/19700101T000140-5752/autostart.before').read_bytes()
            current = (veh / 'usr/bin/autostart').read_bytes()
            (self.root / 'usr/bin/autostart').write_bytes(current if upgrade else clean)
            if upgrade:
                shutil.copytree(vbase, self.base, symlinks=True)
        elif upgrade:
            raise SystemExit('Scenario D needs --vehicle')
        oem = self.root / 'tmp/mnt/data_persist/oem-aa-mod'
        oem.mkdir(parents=True)
        if args.libpatch:
            for name in ('libpatch-aap_service.so', 'libpatch-blmjciaapa.so', 'libpatch-svcjcinavi.so'):
                shutil.copy2(Path(args.libpatch) / name, oem / name)
        if args.libpatch_conf:
            shutil.copy2(args.libpatch_conf, oem / 'libpatch.conf')
        else:
            (oem / 'libpatch.conf').write_text('# authored stand-in\n')
        (self.root / 'tmp/mnt/data/dmesg.out').write_text('[ 1451.0] older reset before installation\n')
        self.reboot('replica power-on')

    def snapshot(self):
        """path -> (kind, mode, uid, gid, sha256 | link target | ''). Directory
        and file times are recorded separately (mtimes) and not compared."""
        found, mtimes = {}, {}
        for top, dirs, files in os.walk(self.root):
            rel_top = os.path.relpath(top, self.root)
            keep = []
            for d in sorted(dirs):
                rel = os.path.normpath(os.path.join(rel_top, d))
                if self.excluded(rel):
                    continue
                keep.append(d)
            dirs[:] = keep
            for name in sorted(dirs + files):
                rel = os.path.normpath(os.path.join(rel_top, name))
                if self.excluded(rel):
                    continue
                path = Path(top) / name
                info = path.lstat()
                mode = stat.S_IMODE(info.st_mode)
                if stat.S_ISLNK(info.st_mode):
                    found[rel] = ('link', mode, info.st_uid, info.st_gid, os.readlink(path))
                elif stat.S_ISDIR(info.st_mode):
                    found[rel] = ('dir', mode, info.st_uid, info.st_gid, '')
                else:
                    found[rel] = ('file', mode, info.st_uid, info.st_gid,
                                  hashlib.sha256(path.read_bytes()).hexdigest())
                mtimes[rel] = int(info.st_mtime)
        return found, mtimes

    @staticmethod
    def excluded(rel):
        top = rel.split(os.sep)[0]
        if top in HARNESS:
            return True
        if top == 'tmp':
            # Only the persistent mounts below /tmp/mnt are CMU storage; the rest
            # is tmpfs (listed separately) and /tmp/mnt/sda1 is the USB.
            return not (rel in ('tmp', 'tmp/mnt') or rel.startswith('tmp/mnt/data_persist')
                        or rel.startswith('tmp/mnt/data'))
        return False

    def tmpfs_leftovers(self):
        return sorted(p.name for p in (self.root / 'tmp').iterdir()
                      if p.name != 'mnt' and ('mx5' in p.name))

    def compare(self, label, before, after, before_m, after_m):
        added = sorted(set(after) - set(before))
        removed = sorted(set(before) - set(after))
        changed = sorted(k for k in set(before) & set(after) if before[k] != after[k])
        lines = [f'+ {k} {after[k]}' for k in added] + [f'- {k} {before[k]}' for k in removed] + \
                [f'~ {k} {before[k]} -> {after[k]}' for k in changed]
        (self.out / f'tree-diff-{label}.txt').write_text('\n'.join(lines) + '\n')
        self.emit(f'--- tree comparison [{label}]: {len(before)} entries before, {len(after)} after ---')
        for line in lines[:60]:
            self.emit(line)
        moved = sorted(k for k in set(before_m) & set(after_m)
                       if before_m[k] != after_m[k])
        files = [k for k in moved if after[k][0] == 'file']
        dirs = [k for k in moved if after[k][0] == 'dir']
        self.emit(f'(not compared) mtime differs: {len(dirs)} directories {dirs}')
        self.emit(f'(not compared) mtime differs: {len(files)} regular files {files}')
        self.check(f'{label}: tree identical (paths, types, modes, owners, sha256, link targets)',
                   not lines, f'{len(added)} added, {len(removed)} removed, {len(changed)} changed')
        return files, dirs

    def summary(self):
        passed = sum(ok for _, ok in self.findings)
        self.emit(f'\nSUMMARY {self.tag}: {passed}/{len(self.findings)} checks passed')
        for name, ok in self.findings:
            if not ok:
                self.emit('FAILED: ' + name)
        return passed == len(self.findings)


def product(chosen):
    return re.fullmatch(r'/tmp/mx5dr-trial-\w{6}/sm\.conf', chosen) is not None


def bundle_with_checkout(args):
    """Published bundle payloads + this checkout's packaging scripts."""
    dest = Path(args.work) / 'bundle'
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(args.bundle, dest)
    for name in HELPERS:
        shutil.copy2(REPO / 'packaging' / name, dest / name)
    shutil.copy2(REPO / 'packaging/USB_START_KO.md', dest / 'INSTALL_KO.md')
    files = sorted(p for p in dest.rglob('*') if p.is_file() and p.name != 'SHA256SUMS')
    (dest / 'SHA256SUMS').write_text(''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.relative_to(dest).as_posix() + '\n'
        for p in files))
    args.bundle_dir = str(dest)


def refusal(run, code, reason, label):
    before, _ = run.snapshot()
    tmp_before = run.tmpfs_leftovers()
    rc, screen = run.menu('6\n', label)
    after, _ = run.snapshot()
    run.check(f'{label}: exit {code}', rc == code, f'rc={rc}')
    run.check(f'{label}: reason shown', 'Refused, nothing deleted: ' + reason in screen, reason)
    run.check(f'{label}: tree unchanged', before == after)
    run.check(f'{label}: no USB report written', not (run.root / 'tmp/mnt/sda1/purge-result.txt').exists())
    run.check(f'{label}: tmpfs mx5 entries unchanged (mount lock may be created)',
              set(run.tmpfs_leftovers()) - {'.mx5dr-mount.lock'} == set(tmp_before) - {'.mx5dr-mount.lock'})


def lifecycle(run, reference, upgrade):
    mounts0 = (run.root / 'proc/self/mounts').read_text()
    if upgrade:
        chosen = run.autostart_boot('shadow.5 state, consumed arm')
        run.check('D0: shadow.5 consumed state starts stock', chosen == '/jci/sm/sm.conf', chosen)
    run.emit('\n#### menu 1: install persistent BETA')
    rc, screen = run.menu('1\n0\n', 'menu 1 install')
    run.check('install rc 0', rc == 0, f'rc={rc}')
    run.check('installed persistent', 'Installed BETA persistent' in screen)
    run.check('guard/persist present after install', (run.base / 'guard/persist').is_file())
    rc, screen = run.menu('0\n', 'first screen')
    run.check('first screen lists menu 6',
              '6 Delete everything this package left on the CMU (run 4, then 5, first)' in screen)
    refusal(run, 3, 'guard/persist present', 'menu 6 while installed')
    for n, var in enumerate(('SMCFG_NORMALMODE', 'SMCFG_WCPMODE', 'SMCFG_NORMALMODE', 'SMCFG_NORMALMODE')):
        chosen = run.autostart_boot('product', var)
        run.check(f'boot {run.boot_number}: product trial selected ({var})', product(chosen), chosen)
    rc, screen = run.menu('2\n0\n', 'menu 2 status')
    rc, screen = run.menu('3\n', 'menu 3 finish and export')
    run.check('menu 3 export_exit=0', 'export_exit=0' in screen)
    run.emit('\n#### menu 4 in the product boot')
    rc, screen = run.menu('4\n', 'menu 4 uninstall')
    run.check('uninstall rc 0', rc == 0, f'rc={rc}')
    run.check('autostart restored to the vehicle pre-mx5dr bytes',
              (run.root / 'usr/bin/autostart').read_bytes() == reference['usr/bin/autostart.bytes'])
    # The SM of this boot still runs the /tmp trial; jciAAPA maps the library.
    run.process(201, '/jci/sm/sm\0', 'b6a00000-b6a80000 r-xp 00000000 1f:05 4242 '
                '/tmp/mnt/data_persist/mx5-aa-dr/libmx5dr.so\n', {'0': '/dev/null'})
    refusal(run, 4, 'process 201 uses a package file (maps)', 'menu 6 before reboot (mapped library)')
    shutil.rmtree(run.root / 'proc/201')
    refusal(run, 4, 'process 266 uses a package file (cmdline)', 'menu 6 before reboot (SM on trial)')
    run.emit('\n#### reboot (menu 5 stand-in): stock boot, nothing mapped')
    chosen = run.autostart_boot('after uninstall')
    run.check('after uninstall the autostart starts stock', chosen == '/jci/sm/sm.conf', chosen)
    run.stock_processes('/jci/sm/sm.conf')
    lock = run.base / 'logs/collector.lock'
    run.check('collector lock file exists (installer created it)', lock.is_file())
    with open(lock, 'rb') as held:
        fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
        refusal(run, 5, 'collector is running', 'menu 6 while collector holds its lock')
    mount_lock = run.root / 'tmp/.mx5dr-mount.lock'
    mount_lock.touch()
    with open(mount_lock, 'rb') as held:
        fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
        before, _ = run.snapshot()
        rc, screen = run.menu('6\n', 'menu 6 while another installer holds the mount lock')
        run.check('live installer: refused', rc != 0 and 'Another installer or USB export is active' in screen,
                  f'rc={rc}')
        run.check('live installer: tree unchanged', before == run.snapshot()[0])
    # Stale leftovers of an interrupted run in this boot.
    persist = run.root / 'tmp/mnt/data_persist'
    (persist / '.mx5dr-install-lock').mkdir()
    (persist / '.mx5dr-install-lock/pid').write_text('4242\n')
    (run.root / 'tmp/mx5dr-hash.Zx9Qw1').mkdir()
    (run.root / 'tmp/mx5dr-hash.Zx9Qw1/hash').write_bytes(b'\x7fELF stale helper copy')
    (run.base / 'guard/.persist-state.4242').write_text('torn write\n')
    run.emit('\n#### menu 6')
    deleted_bytes = sum(p.lstat().st_size for p in run.base.rglob('*') if p.is_file() and not p.is_symlink())
    deleted_files = sum(1 for p in run.base.rglob('*') if p.is_file() and not p.is_symlink())
    deleted_files += 1  # the stale /tmp hash helper copy
    deleted_bytes += (run.root / 'tmp/mx5dr-hash.Zx9Qw1/hash').stat().st_size
    rc, screen = run.menu('6\n', 'menu 6 delete everything')
    run.check('menu 6 rc 0', rc == 0, f'rc={rc}')
    run.check('screen: package directory absent', 'Package directory absent' in screen)
    run.check('screen: deleted totals', f'Deleted {deleted_files} files, {deleted_bytes} bytes' in screen,
              f'expected {deleted_files} files, {deleted_bytes} bytes')
    run.check('screen: OEM files identical to pre-install', 'OEM files: identical to pre-install' in screen)
    block = screen[screen.find('---- DELETE RESULT ----'):]
    run.check('screen: result block lines <= 40 chars', all(len(x) <= 40 for x in block.splitlines()))
    report = (run.root / 'tmp/mnt/sda1/purge-result.txt').read_text()
    (run.out / 'purge-result.txt').write_text(report)
    run.check('report: finished', 'status=finished' in report)
    run.check('report: verdict identical', 'verdict: identical to the pre-install state' in report)
    run.check('report: package directory absent', 'after_package_directory=absent' in report)
    run.check('report: stale install lock reclaimed', 'stale_install_lock_reclaimed=yes' in report)
    run.check('report: stale /tmp hash copy listed', 'dir 0 /tmp/mx5dr-hash.Zx9Qw1' in report)
    if upgrade:
        run.check('report: basis is the oldest vehicle record',
                  'compare_basis=19700101T000140-5752' in report)
        run.check('report: interrupted vehicle record not used',
                  '19700101T002334-12846' in report.split('compare_incomplete_or_not_original=')[1].splitlines()[0])
        run.check('report: records agree', 'compare_records_agree=yes' in report)
        for name in ('guard/consumed', 'guard/armed-boot.previous', 'guard/last-boot', 'logs/trace.1.jsonl'):
            run.check(f'report lists shadow.5 leftover {name}', '/data_persist/mx5-aa-dr/' + name in report)
    run.check('package directory absent on disk', not run.base.exists())
    run.check('install lock absent', not (persist / '.mx5dr-install-lock').exists())
    run.check('mount table restored to the initial state', (run.root / 'proc/self/mounts').read_text() == mounts0)
    left = run.tmpfs_leftovers()
    run.check('tmpfs: only the mount lock inode remains (by design)', left == ['.mx5dr-mount.lock'], str(left))
    after, after_m = run.snapshot()
    files, dirs = run.compare('after menu 6', reference['tree'], after, reference['mtimes'], after_m)
    rc, screen = run.menu('6\n', 'menu 6 second run')
    run.check('second run rc 0 and nothing to delete', rc == 0 and 'Nothing to delete' in screen, f'rc={rc}')
    again, _ = run.snapshot()
    run.check('second run changes nothing', again == after)
    run.check('second run kept the report', (run.root / 'tmp/mnt/sda1/purge-result.txt').read_text() == report)
    chosen = run.autostart_boot('after menu 6')
    run.check('boot after menu 6 starts stock', chosen == '/jci/sm/sm.conf', chosen)
    return files, dirs


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--stock', required=True)
    parser.add_argument('--bundle', required=True, help='unpacked published USB bundle (not modified)')
    parser.add_argument('--work', required=True)
    parser.add_argument('--proot', required=True)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--vehicle', help='private vehicle export root (jci/, usr/, tmp/mnt/data_persist/)')
    parser.add_argument('--libpatch', help='oem-aa-mod DSO folder')
    parser.add_argument('--libpatch-conf')
    parser.add_argument('--scenario', choices=('A', 'D', 'both'), default='both')
    args = parser.parse_args()
    Path(args.work).mkdir(parents=True, exist_ok=True)
    bundle_with_checkout(args)
    ok = True
    for tag in ('A', 'D') if args.scenario == 'both' else (args.scenario,):
        ref = Run(args, tag + 'ref')
        ref.build(upgrade=False)
        tree, mtimes = ref.snapshot()
        reference = {'tree': tree, 'mtimes': mtimes,
                     'usr/bin/autostart.bytes': (ref.root / 'usr/bin/autostart').read_bytes()}
        ref.emit(f'reference tree: {len(tree)} entries')
        if tag == 'A':
            run = ref
        else:
            run = Run(args, tag)
            run.build(upgrade=True)
        run.emit(f'\n######## Scenario {tag}')
        lifecycle(run, reference, tag == 'D')
        ok = run.summary() and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
