#!/usr/bin/env python3
"""Shared stock-BusyBox replica driver for the release gates (G7, G8, G9, G16).

The replica root is built by tests/packaging/cmu_emulation.make_root (stock
ARM BusyBox 1.19.2, libc, the firmware-hashed files, sm.conf, sm_WCP.conf,
version.ini and autostart copied READ-ONLY from the private stock tree) and
the unpacked USB bundle is copied to /tmp/mnt/sda1. Guest commands run under
PRoot (fake UID 0) with qemu-arm. The installed ARM guard runs through the
owned autostart block of the installed /usr/bin/autostart; boots are authored
boot_id changes and /tmp clears; SM is an authored /proc entry. No OEM PID 1,
SM, AA, VBS or LDS process is executed.

Environment (defaults in parentheses):
  MX5DR_STOCK_ROOT   private stock firmware tree, read only (required)
  MX5DR_PROOT        proot executable (proot on PATH)
  QEMU_ARM           qemu-arm user emulator (qemu-arm, then qemu-arm-static on PATH)
  MX5DR_GATE_WORK    scratch directory for replica roots and transcripts (a new mktemp dir)
  MX5DR_GATE_NICE    nice level of guest commands (19)
Nothing private is written into the repository; transcripts stay in the work dir.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / 'tests/packaging'))
import cmu_emulation as h  # noqa: E402

SKIP = 77
# SIGTERM/SIGHUP unwind through the finally blocks, so no guest process group
# (PRoot, qemu, shells) outlives a killed gate.
for _sig in (signal.SIGTERM, signal.SIGHUP):
    signal.signal(_sig, lambda n, f: sys.exit(128 + n))
BEGIN = '# MX5DR ONE-BOOT BEGIN '
END = '# MX5DR ONE-BOOT END '
STOCK_SM_PROCESS_MAPS = ('00008000-00030000 r-xp 00000000 1f:02 1201 /jci/sm/sm\n'
                         'b6e00000-b6f20000 r-xp 00000000 1f:02 88 /lib/libc-2.11.1.so\n')
# Harness-authored paths, not CMU filesystem content.
HARNESS = {'proc', 'test-bin', 'mount.calls', 'drv.sh', 'reboot.witness', 'powercut', 'powercut.at',
           'powercut.count', 'powercut.log'}


def skip(reason):
    print(f'SKIP: {reason}', flush=True)
    sys.exit(SKIP)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def tool(env, *names):
    value = os.environ.get(env)
    if value:
        return value if Path(value).is_file() and os.access(value, os.X_OK) else None
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def environment():
    """Resolve the documented environment or exit 77 (skip) with the reason."""
    stock = os.environ.get('MX5DR_STOCK_ROOT')
    if not stock or not Path(stock, 'bin/busybox').exists():
        skip('MX5DR_STOCK_ROOT unset or not a stock firmware tree')
    proot = tool('MX5DR_PROOT', 'proot')
    qemu = tool('QEMU_ARM', 'qemu-arm', 'qemu-arm-static')
    if not proot:
        skip('proot not found (set MX5DR_PROOT)')
    if not qemu:
        skip('qemu-arm not found (set QEMU_ARM)')
    work = os.environ.get('MX5DR_GATE_WORK')
    work = Path(work) if work else Path(tempfile.mkdtemp(prefix='mx5dr-gate-'))
    work.mkdir(parents=True, exist_ok=True)
    return Path(stock).resolve(), proot, qemu, work.resolve()


def unpack(zip_or_dir, into):
    """A bundle directory as is, or a ZIP unpacked into `into` (fresh)."""
    path = Path(zip_or_dir)
    if path.is_dir():
        return path.resolve()
    if into.exists():
        shutil.rmtree(into)
    into.mkdir(parents=True)
    subprocess.run(['unzip', '-q', str(path), '-d', str(into)], check=True)
    return into.resolve()


def verify_zip_sha256(path, expected):
    got = sha(Path(path).read_bytes())
    if got != expected:
        raise SystemExit(f'FAIL: {path} sha256 {got} != expected {expected}')
    return got


class Replica:
    """One replica root plus its transcript."""

    def __init__(self, work, tag, stock, proot, qemu):
        self.work, self.tag, self.stock, self.proot, self.qemu = Path(work), tag, stock, proot, qemu
        self.out = self.work / ('out-' + tag)
        self.root = self.work / ('root-' + tag)
        for path in (self.out, self.root):
            if path.exists():
                shutil.rmtree(path)
        self.out.mkdir(parents=True)
        self.log = open(self.out / 'session.txt', 'w')
        self.step = 0
        self.findings = []
        self.base = self.root / 'tmp/mnt/data_persist/mx5-aa-dr'
        self.boot_number = 0
        self.boot_prefix = 0x70000000 + (sum(tag.encode()) % 4096) * 0x10000
        self.nice = os.environ.get('MX5DR_GATE_NICE', '19')

    # ---- transcript -------------------------------------------------------
    def emit(self, text=''):
        print(text, flush=True)
        self.log.write(text + '\n')
        self.log.flush()

    def check(self, name, ok, detail=''):
        self.findings.append((name, bool(ok), False))
        self.emit(f'CHECK {"PASS" if ok else "FAIL"}: {name}' + (f' -- {detail}' if detail else ''))
        return bool(ok)

    def skipped(self, name, why):
        self.findings.append((name, True, True))
        self.emit(f'CHECK SKIP: {name} -- {why}')

    def summary(self, label=None):
        real = [(n, ok) for n, ok, sk in self.findings if not sk]
        passed = sum(ok for _, ok in real)
        skipped = sum(1 for _, _, sk in self.findings if sk)
        self.emit(f'\nSUMMARY {label or self.tag}: {passed}/{len(real)} checks passed, {skipped} skipped')
        for name, ok in real:
            if not ok:
                self.emit('FAILED: ' + name)
        return 0 if passed == len(real) else 1

    # ---- replica ------------------------------------------------------------
    def build(self, bundle, libpatch_dir=None):
        """Stock replica with `bundle` on the USB. firmware.sha256 comes from
        the bundle itself (the version under test), not this checkout."""
        fake = self.work / ('fw-' + self.tag)
        (fake / 'packaging').mkdir(parents=True, exist_ok=True)
        shutil.copy2(Path(bundle) / 'firmware.sha256', fake / 'packaging/firmware.sha256')
        real_repo, real_mknod = h.REPO, os.mknod
        h.REPO = fake
        if os.geteuid() != 0:
            # proot binds the host /dev/null over this placeholder (-b /dev/null).
            os.mknod = lambda path, *a, **k: Path(path).write_text('')
        try:
            self.root.mkdir()
            h.make_root(self.root, self.stock, Path(bundle))
        finally:
            h.REPO, os.mknod = real_repo, real_mknod
        (self.root / 'sbin/reboot').unlink()
        h.put(self.root, '/sbin/reboot', '#!/bin/sh\necho called > /reboot.witness\n', 0o755)
        oem = self.root / 'tmp/mnt/data_persist/oem-aa-mod'
        if libpatch_dir:
            oem.mkdir(parents=True)
            for name in ('libpatch-aap_service.so', 'libpatch-blmjciaapa.so', 'libpatch-svcjcinavi.so'):
                if (Path(libpatch_dir) / name).exists():
                    shutil.copy2(Path(libpatch_dir) / name, oem / name)
            (oem / 'libpatch.conf').write_text('# authored stand-in\n')
        (self.root / 'tmp/mnt/data/dmesg.out').write_text('[ 1451.0] older reset before installation\n')
        self.original = {name: (self.root / name).read_bytes() for name in self.oem_files()}
        self.reboot('replica power-on')

    def oem_files(self):
        names = ['usr/bin/autostart', 'jci/sm/sm.conf', 'jci/sm/sm_WCP.conf', 'jci/version.ini']
        for line in (self.root / 'tmp/mnt/sda1/firmware.sha256').read_text().splitlines():
            if line.split():
                names.append(line.split()[1].lstrip('/'))
        return sorted(set(names))

    def swap_usb(self, bundle):
        usb = self.root / 'tmp/mnt/sda1'
        for path in list(usb.iterdir()):
            shutil.rmtree(path) if path.is_dir() and not path.is_symlink() else path.unlink()
        for path in Path(bundle).iterdir():
            if path.is_dir():
                shutil.copytree(path, usb / path.name)
            else:
                shutil.copy2(path, usb / path.name)
        for file in usb.rglob('*'):
            if file.is_file():
                file.chmod(0o644)
        self.emit(f'(USB stick swapped to {Path(bundle).name})')

    def clone(self, tag):
        """A copy of this replica's root (symlinks kept) as a new Replica."""
        other = Replica(self.work, tag, self.stock, self.proot, self.qemu)
        shutil.rmtree(other.root, ignore_errors=True)
        shutil.copytree(self.root, other.root, symlinks=True)
        other.original = dict(self.original)
        other.boot_number = self.boot_number
        other.boot_prefix = self.boot_prefix
        return other

    # ---- guest execution ----------------------------------------------------
    def command(self, argv):
        return ['nice', '-n', self.nice, self.proot, '-0', '-q', self.qemu, '-b', '/dev/null', '-w', '/',
                '-r', str(self.root)] + argv

    def guest(self, argv, keys=None, label='', timeout=1800, watch=None):
        """Run argv in the guest. watch: a path; when it appears the whole
        guest process group is killed (power cut) and rc is None."""
        self.step += 1
        env = h.guest_environment()
        env.pop('TMPDIR', None)
        start = time.monotonic()
        with tempfile.TemporaryFile() as out:
            p = subprocess.Popen(self.command(argv), env=env, stdin=subprocess.PIPE, stdout=out,
                                 stderr=subprocess.STDOUT, start_new_session=True)
            try:
                p.stdin.write((keys or '').encode())
                p.stdin.close()
            except BrokenPipeError:
                pass
            cut = False
            try:
                while p.poll() is None:
                    if watch is not None and watch.exists():
                        os.killpg(p.pid, signal.SIGKILL)
                        cut = True
                        break
                    if time.monotonic() - start > timeout:
                        os.killpg(p.pid, signal.SIGKILL)
                        break
                    time.sleep(0.05)
            finally:
                try:
                    p.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    pass
                try:
                    os.killpg(p.pid, signal.SIGKILL)  # no orphan guest process survives
                except ProcessLookupError:
                    pass
            out.seek(0)
            screen = out.read().decode('utf-8', 'replace')
        rc = None if cut else p.returncode
        (self.out / f'{self.step:03d}.screen.txt').write_text(screen)
        self.emit('=' * 78)
        self.emit(f'[{self.step:03d}] {label}')
        self.emit('guest command: ' + ' '.join(argv) + ('' if keys is None else '   keys: ' + json.dumps(keys)))
        self.emit(f'exit code: {"POWER CUT" if cut else rc}   (elapsed {time.monotonic() - start:.1f}s under proot+qemu)')
        self.emit('--- screen (last 30 lines) ---')
        self.emit('\n'.join(screen.rstrip('\n').splitlines()[-30:]))
        return rc, screen

    def menu(self, keys, label, **kw):
        return self.guest(['/bin/sh', '/tmp/mnt/sda1/trial'], keys, label, **kw)

    def sh(self, script, label, **kw):
        path = self.root / 'drv.sh'
        path.write_text(script + '\n')
        try:
            return self.guest(['/bin/sh', '/drv.sh'], None, label, **kw)
        finally:
            path.unlink()

    # ---- authored kernel view -------------------------------------------------
    def boot_id(self, n):
        return '%08x-1234-4234-8234-0123456789ab' % (self.boot_prefix + n)

    def stock_sm(self, config):
        for entry in (self.root / 'proc').iterdir():
            if entry.name.isdigit():
                shutil.rmtree(entry)
        proc = self.root / 'proc/266'
        (proc / 'fd').mkdir(parents=True)
        (proc / 'cmdline').write_bytes(f'/jci/sm/sm\0-f\0{config}\0-e\0/tmp/smevents.txt\0'.encode())
        (proc / 'maps').write_text(STOCK_SM_PROCESS_MAPS)
        for number, target in {'0': '/dev/null', '1': '/dev/null', '3': '/tmp/smevents.txt'}.items():
            (proc / 'fd' / number).symlink_to(target)

    def reboot(self, label):
        """New Linux boot: tmpfs /tmp loses everything except the mounts below
        /tmp/mnt, the kernel gets a new boot ID, fresh processes."""
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
        self.stock_sm('/jci/sm/sm.conf')
        self.emit(f'(simulated CMU reboot {self.boot_number}: {label}; /tmp cleared, new boot ID)')

    def autostart_boot(self, label, var='SMCFG_NORMALMODE', sm_runs=True, guard_stub=None, new_boot=True):
        """Run the installed owned block (if any) of this board branch with the
        real ARM guard and stock timeout, then author the SM /proc entry it
        would start. Returns the config path SM gets."""
        if new_boot:
            self.reboot(label)
        text = (self.root / 'usr/bin/autostart').read_text(errors='replace')
        begin = BEGIN + var
        block = text[text.index(begin):text.index(END + var)] if begin in text and END + var in text else ''
        block = block.replace('/data_persist/mx5-aa-dr/tools/start_collector.sh', '/nonexistent-collector')
        block = block.replace('/bin/sleep 90;', '/bin/sleep 3;')
        if guard_stub:
            block = block.replace('/data_persist/mx5-aa-dr/guard/mx5dr-guard select', guard_stub + ' select')
        default = '/jci/sm/sm.conf' if var == 'SMCFG_NORMALMODE' else '/jci/sm/sm_WCP.conf'
        launch = ''
        if sm_runs:
            launch = ("rm -rf /proc/266; mkdir -p /proc/266\n"
                      "printf '/jci/sm/sm\\000-f\\000%s\\000-e\\000/tmp/smevents.txt\\000' \"$" + var +
                      "\" > /proc/266/cmdline\n")
        script = (f"{var}='{default}'\nstart=$(cut -d' ' -f1 /proc/uptime)\n" + block +
                  f'echo "SM_CONFIG=${var}"\n' + launch + 'wait\n')
        rc, screen = self.sh(script, f'boot {self.boot_number}: autostart {var} ({label})')
        match = re.search(r'SM_CONFIG=(\S+)', screen)
        return match.group(1) if match else ''

    @staticmethod
    def selected(chosen):
        return re.fullmatch(r'/tmp/mx5dr-trial-\w{6}/sm(_WCP)?\.conf', chosen or '') is not None

    def trial_complete(self, chosen):
        """A selected trial config exists and carries all three product preloads."""
        if not self.selected(chosen):
            return False
        path = self.root / chosen.lstrip('/')
        if not path.is_file():
            return False
        text = path.read_text(errors='replace')
        return all(t in text for t in ('/data_persist/mx5-aa-dr/libmx5dr.so', 'libmx5dr-vimtap.so',
                                       'libmx5dr-ldstap.so'))

    # ---- status -----------------------------------------------------------------
    @staticmethod
    def verdict_block(screen):
        lines = screen.replace('Number, then Enter: ', '').splitlines()
        for i, line in enumerate(lines):
            if line == '---- GO / NO-GO ----':
                block = []
                for x in lines[i:]:
                    if x.startswith('0 Exit'):
                        break
                    block.append(x)
                return [x for x in block if x.strip()]
        return []

    def status(self, tag):
        rc, screen = self.menu('2\n0\n', f'menu 2 status [{tag}]')
        block = self.verdict_block(screen)
        self.emit(f'--- verdict block [{tag}] ---')
        for x in block:
            self.emit(f'{len(x):2d}|{x}')
        bad = [x for x in screen.splitlines() if re.search(r'awk: |syntax error|not found|unexpected|Usage: awk', x)]
        self.check(f'{tag}: no awk/shell portability error text', not bad, '; '.join(bad[:3]))
        return screen, block

    @staticmethod
    def install_side_go(block):
        """No NO row and the verdict is GO or WAIT. In this replica the
        runtime never runs (no AA, VBS, LDS, collector), so the rows that need
        live runtime evidence (POLL, HLTH, services, BETA, HOOK, FENCE) can
        only be 'wait'; a real GO needs the vehicle (or authored rows)."""
        no = [x for x in block if x.startswith('NO ') or x.startswith('NO-GO')]
        last = block[-1] if block else ''
        return (not no and last in ('GO', 'WAIT 60 s, then run 2 again'),
                f'last={last!r} no_rows={no[:3]}')

    def persist_state(self):
        path = self.base / 'guard/persist-state'
        if not path.is_file():
            return {}
        return dict(line.split('=', 1) for line in path.read_text().splitlines()[1:] if '=' in line)

    # ---- OEM file identity ---------------------------------------------------------
    def oem_identical(self, tag, allow_autostart_blocks=True):
        """Every stock file the package may touch equals its pre-install bytes;
        the autostart equals them after removing the owned blocks."""
        bad = []
        for name, data in self.original.items():
            now = (self.root / name).read_bytes() if (self.root / name).is_file() else None
            if name == 'usr/bin/autostart' and allow_autostart_blocks and now is not None:
                now = strip_blocks(now)
            if now != data:
                bad.append(name)
        return self.check(f'{tag}: stock files byte-identical ({len(self.original)} files)', not bad, ', '.join(bad))

    def autostart_restored(self, tag):
        now = (self.root / 'usr/bin/autostart').read_bytes()
        before = self.original['usr/bin/autostart']
        exact = now == before
        # Documented exception: the stock file has no trailing newline and the
        # editor may leave one (validation/STOCK_BUSYBOX_MENU_2026-10-04.md).
        newline = not exact and now.rstrip(b'\n') == before.rstrip(b'\n')
        self.check(f'{tag}: autostart byte-identical to the pre-install file'
                   + (' (except the trailing newline)' if newline else ''), exact or newline,
                   f'{sha(now)[:16]} vs {sha(before)[:16]}')
        return exact or newline

    # ---- tree snapshot (G9) ----------------------------------------------------------
    def snapshot(self, include_usb=False):
        found = {}
        for top, dirs, files in os.walk(self.root):
            rel_top = os.path.relpath(top, self.root)
            keep = []
            for d in sorted(dirs):
                rel = os.path.normpath(os.path.join(rel_top, d))
                if not self.excluded(rel, include_usb):
                    keep.append(d)
            dirs[:] = keep
            for name in sorted(dirs + files):
                rel = os.path.normpath(os.path.join(rel_top, name))
                if self.excluded(rel, include_usb):
                    continue
                path = Path(top) / name
                info = path.lstat()
                mode = stat.S_IMODE(info.st_mode)
                if stat.S_ISLNK(info.st_mode):
                    found[rel] = ('link', mode, os.readlink(path))
                elif stat.S_ISDIR(info.st_mode):
                    found[rel] = ('dir', mode, '')
                else:
                    found[rel] = ('file', mode, sha(path.read_bytes()))
        return found

    @staticmethod
    def excluded(rel, include_usb=False):
        parts = rel.split(os.sep)
        if parts[0] in HARNESS:
            return True
        if parts[0] == 'tmp':
            if len(parts) == 1 or parts[1] != 'mnt':
                return True  # tmpfs: cleared at every boot
            if len(parts) > 2 and parts[2] == 'sda1' and not include_usb:
                return True
            if len(parts) > 2 and parts[2] == 'data':
                return True  # SM reset reports (authored)
        return False


def strip_blocks(data):
    text = data.decode('utf-8', 'replace')
    text = re.sub(r'# MX5DR ONE-BOOT BEGIN (SMCFG_\w+)\n.*?# MX5DR ONE-BOOT END \1\n', '', text, flags=re.S)
    return text.encode()


def diff_snapshots(a, b, limit=12):
    changed = sorted(set(a) ^ set(b) | {k for k in set(a) & set(b) if a[k] != b[k]})
    return changed[:limit], len(changed)
