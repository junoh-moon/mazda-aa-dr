#!/usr/bin/env python3
"""Run the production installer in chroot with the supplied CMU ARM BusyBox.

Requires Linux UID 0, chroot and working ARM binfmt/QEMU (e.g. OrbStack).
Never runs the OEM autostart, Service Manager, AA or VBS executables. Mounts
are modelled explicitly; the test never remounts the host. Stock ownership and
physical flash/power-loss behavior are not emulated. No firmware is distributed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
BOOT = '01234567-1234-1234-1234-0123456789ab\n'
TOUCH = '/data_persist/oem-aa-mod/libpatch-blmjciaapa.so'

MOUNT = '''#!/bin/sh
[ "$1" = -o ] || exit 91
case "$2" in remount,rw) mode=rw;; remount,ro) mode=ro;; *) exit 92;; esac
echo "$*" >> /mount.calls
awk -v mp="$3" -v mode="$mode" '
 $2==mp && $1!="rootfs" {
  n=split($4,a,","); $4=""
  for(i=1;i<=n;i++){if(a[i]=="ro" || a[i]=="rw")a[i]=mode; $4=$4 (i>1?",":"") a[i]}
 } {print}' /proc/mounts > /proc/mounts.next || exit 93
/bin/mv /proc/mounts.next /proc/mounts
'''
# Catch the original bug even though the test host's chroot storage is writable.
# cp is always the first staging operation on each OEM file. Other operations
# are checked by comparing the mount call sequence and unchanged baseline files.
COPY = '''#!/bin/sh
for dest in "$@"; do :; done
case "$dest" in
 /jci/*|/usr/bin/*)
  awk '$2=="/" && $1!="rootfs" {opts=$4} END{exit !(opts ~ /(^|,)rw(,|$)/)}' /proc/mounts || {
   echo "emulated EROFS: $dest" >&2; exit 30;
  };;
esac
exec /bin/cp "$@"
'''


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def put(root, name, text, mode=0o644):
    dest = root / name.lstrip('/')
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(text)
    dest.chmod(mode)


def copy_file(stock, root, name):
    src = stock / name.lstrip('/')
    dest = root / name.lstrip('/')
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest, follow_symlinks=False)


def make_root(root, stock, bundle):
    root.chmod(0o755)  # The collector must traverse / after dropping to cmu.
    # Use the exact shipped shell/applets and dynamic loader/libc, not host tools.
    for pattern in ('ld-*', 'libc-*', 'libc.so*', 'libdl*', 'libm-*', 'libm.so*',
                    'libpthread*', 'librt*', 'libnss_files*', 'libgcc_s*'):
        for src in (stock / 'lib').glob(pattern):
            copy_file(stock, root, '/lib/' + src.name)
    copy_file(stock, root, '/bin/busybox')
    for directory in ('bin', 'usr/bin', 'usr/sbin', 'sbin'):
        for src in (stock / directory).iterdir():
            if src.is_symlink() and 'busybox' in os.readlink(src):
                copy_file(stock, root, '/' + directory + '/' + src.name)
    for src in (stock / 'usr/lib').glob('libdbus-1.so*'):
        copy_file(stock, root, '/usr/lib/' + src.name)
    for src in (stock / 'usr/lib').glob('libexpat.so*'):
        copy_file(stock, root, '/usr/lib/' + src.name)
    paths = [line.split()[1] for line in (REPO / 'packaging/firmware.sha256').read_text().splitlines()]
    paths += ['jci/version.ini', 'jci/sm/sm.conf', 'jci/sm/sm_WCP.conf', 'usr/bin/autostart']
    for name in paths:
        copy_file(stock, root, '/' + name)
    for name in ('jci', 'jci/sm', 'usr/bin'):
        (root / name).chmod((stock / name).stat().st_mode & 0o777)
    (root / 'tmp/mnt/data_persist').mkdir(parents=True)
    (root / 'tmp').chmod(0o1777)
    (root / 'data_persist').symlink_to('/mnt/data_persist')
    (root / 'mnt').symlink_to('/tmp/mnt')
    # The writable factory partition isn't in the firmware tar. Model the
    # reported UID-0-without-root account, plus the production cmu service user.
    put(root, '/config-mfg/passwd', 'jci:x:0:0:JCI:/tmp:/bin/sh\ncmu:x:500:500:CMU:/tmp:/bin/sh\n')
    (root / 'etc').mkdir(exist_ok=True)
    (root / 'etc/passwd').symlink_to('/config-mfg/passwd')
    put(root, '/etc/group', 'jci:x:0:\ncmu:x:500:\n')
    put(root, '/etc/nsswitch.conf', 'passwd: files\ngroup: files\n')
    put(root, '/proc/sys/kernel/random/boot_id', BOOT)
    put(root, '/proc/mounts', 'rootfs / rootfs rw 0 0\n/dev/root / relfs ro,relatime 0 0\n'
        'tmpfs /tmp tmpfs rw 0 0\n/dev/mtdblock8 /tmp/mnt/data_persist relfs rw 0 0\n'
        '/dev/sda1 /tmp/mnt/sda1 vfat ro,noexec 0 0\n')
    put(root, '/proc/uptime', '100.00 0.00\n')
    put(root, '/test-bin/mount', MOUNT, 0o755)
    put(root, '/test-bin/cp', COPY, 0o755)
    (root / 'dev').mkdir()
    os.mknod(root / 'dev/null', 0o20666, os.makedev(1, 3))
    usb = root / 'tmp/mnt/sda1'
    shutil.copytree(bundle, usb)
    for file in usb.rglob('*'):
        if file.is_file():
            file.chmod(0o644)  # FAT32 execute bits cannot be relied upon.
    return usb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, required=True)
    args = parser.parse_args()
    require(os.geteuid() == 0, 'Run inside an isolated Linux container as UID 0')
    with tempfile.TemporaryDirectory(prefix='mx5dr-cmu-') as tmp:
        root = Path(tmp)
        usb = make_root(root, args.stock.resolve(), args.bundle.resolve())
        environment = dict(os.environ, PATH='/test-bin:/bin:/usr/bin:/sbin:/usr/sbin',
                           LC_ALL='C', LD_LIBRARY_PATH='/lib:/usr/lib')
        environment.pop('MX5DR_FIXTURE_ROOT', None)
        environment.pop('LD_PRELOAD', None)

        def run(script, ok=True):
            result = subprocess.run(['/usr/sbin/chroot', str(root), '/bin/sh', '-c', script],
                                    env=environment, capture_output=True, text=True, timeout=90)
            print(result.stdout, end='')
            if result.stderr:
                print(result.stderr, end='')
            require((result.returncode == 0) == ok, f'Unexpected rc={result.returncode}: {script}')
            return result

        # Ensure the intended guest is actually active (some host translators
        # cannot run PRoot correctly). This cannot accidentally use host tools.
        result = run('/bin/busybox 2>&1; test ! -e /src; test "$(id -u)" = 0; '
                     '! id root 2>/dev/null; ! command -v sha256sum; test "$(id -u cmu)" = 500')
        require('BusyBox v1.19.2' in result.stdout, 'Wrong BusyBox under emulation')
        # Reproduce the successful AA installer placement, immediately after
        # the service opening tag, while retaining all other stock content.
        for name in ('sm.conf', 'sm_WCP.conf'):
            path = root / 'jci/sm' / name
            text = path.read_text()
            lines = text.splitlines(keepends=True)
            for i, line in enumerate(lines):
                if 'name="jciAAPA"' in line:
                    lines.insert(i + 1, '            <environ_var env_name="LD_PRELOAD" env_value="' + TOUCH + '"/>\n')
                    break
            path.write_text(''.join(lines))
        baseline = {name: (root / name).read_bytes() for name in ('jci/sm/sm.conf', 'jci/sm/sm_WCP.conf', 'usr/bin/autostart')}
        base = root / 'tmp/mnt/data_persist/mx5-aa-dr'
        # A damaged USB must stop before any persistent install directory exists.
        lib = usb / 'libmx5dr.so'
        saved = lib.read_bytes()
        lib.write_bytes(saved + b'damage')
        run('cd /tmp/mnt/sda1 && sh install.sh', ok=False)
        require(not base.exists(), 'Damaged bundle made installation changes')
        lib.write_bytes(saved)
        run('cd /tmp/mnt/sda1 && sh install.sh')
        require((base / 'guard/arm').is_file(), 'Actual ARM guard did not arm')
        for name in ('jci/sm/sm.conf', 'jci/sm/sm_WCP.conf'):
            require((root / name).read_bytes() == baseline[name], 'Persistent SM config changed')
        require((root / 'mount.calls').read_text().splitlines() ==
                ['-o remount,rw /', '-o remount,ro /'], 'Incorrect containing mount selected/restored')
        require(not (base.parent / '.mx5dr-install-lock').exists(), 'Installer left a lock')
        # Exercise both experimental DSOs with the stock dynamic loader/libc.
        # glibc returns success even when it ignores a missing LD_PRELOAD DSO.
        # Require positive initialization evidence, not just /bin/true's status.
        missing = run('LD_PRELOAD=/missing-mx5dr.so /bin/true')
        require('cannot be preloaded' in missing.stderr, 'Missing DSO negative control failed')
        for name in ('libmx5dr.so', 'libmx5dr-vimtap.so'):
            path = '/data_persist/mx5-aa-dr/' + name
            loaded = run('LD_DEBUG=libs LD_PRELOAD=' + path + ' /bin/true')
            require('cannot be preloaded' not in loaded.stderr and
                    'calling init: ' + path in loaded.stderr,
                    'Stock loader did not initialize ' + name)
        result = run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf')
        trial = root / result.stdout.strip().lstrip('/')
        require(TOUCH in trial.read_text() and '/libmx5dr-vimtap.so' in trial.read_text(), 'Trial lost AA touch or VBS tap')
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf', ok=False)
        put(root, '/proc/sys/kernel/random/boot_id', '11234567-1234-1234-1234-0123456789ab\n')
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf', ok=False)
        # Actual collector executes with guest NSS and drops UID to cmu. Missing
        # vehicle DBus/SMDB is expected here; it must still start/stop its journal.
        run('/data_persist/mx5-aa-dr/mx5dr-collector --session-seconds 1')
        journal = base / 'logs/collector.0.jsonl'
        require(journal.is_file() and journal.stat().st_uid == 500, 'Collector failed cmu ownership')
        require('collector_boot' in journal.read_text(), 'Collector boot evidence missing')
        require('collector_stop' in journal.read_text(), 'Collector stop evidence missing')
        # Run the parked helpers with explicitly synthetic sensor/health rows.
        # No OEM sensor callback or AA process is claimed by these records.
        current_boot = (root / 'proc/sys/kernel/random/boot_id').read_text().strip()
        original_collector = journal.read_bytes()
        put(root, '/tmp/mnt/data_persist/mx5-aa-dr/guard/last-boot', current_boot + '\n', 0o600)
        trace = [dict(kind='boot', boot_id=current_boot, mono_ns=1000000000, mode=4),
                 dict(kind='shadow_boot', active=True, capture_active=True),
                 dict(kind='health', mono_ns=99000000000, hook_installed=True,
                      audit_fault=0, dropped=0, capture_active=True),
                 dict(kind='motion_batch', schema=1, epoch=1, events=[
                     [sensor, sensor, 99000000000, 90000, 0, 0, 0, 0, 1, 0] for sensor in (1, 2, 3)])]
        (base / 'logs/trace.0.jsonl').write_text(''.join(json.dumps(row, separators=(',', ':')) + '\n' for row in trace))
        collector = [dict(kind='collector_boot', boot_id=current_boot, schema=1),
                     dict(kind='poll', end_ns=99000000000, seq=0)]
        journal.write_text(''.join(json.dumps(dict(stream='collector', collector_pid=123,
            observed_at_mono_ns=99000000000, producer_mono_ns=None,
            producer_time_status='unknown', **row), separators=(',', ':')) + '\n' for row in collector))
        run('sh /data_persist/mx5-aa-dr/tools/trial_status.sh')
        (base / 'logs/capture.done').write_text(current_boot + '\n')
        run('sh /data_persist/mx5-aa-dr/tools/finish_capture.sh')
        journal.write_bytes(original_collector)
        run('sh /data_persist/mx5-aa-dr/tools/export_logs.sh /tmp/mnt/sda1')
        archive = next(usb.glob('mx5dr-logs-*.tar'))
        require(archive.with_suffix('.tar.sha256').read_text().split()[0] ==
                hashlib.sha256(archive.read_bytes()).hexdigest(), 'Export checksum mismatch')
        require((root / 'mount.calls').read_text().splitlines()[-2:] ==
                ['-o remount,rw /tmp/mnt/sda1', '-o remount,ro /tmp/mnt/sda1'], 'USB mount not restored')
        # A later touch edit must never be paired with an old trial by raw arm.
        # Explicit rearm rebuilds the pair, while still refusing this same boot.
        updated_touch = TOUCH.replace('.so', '-updated.so')
        path = root / 'jci/sm/sm.conf'
        path.write_text(path.read_text().replace(TOUCH, updated_touch))
        baseline['jci/sm/sm.conf'] = path.read_bytes()
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard arm', ok=False)
        run('sh /data_persist/mx5-aa-dr/tools/arm.sh --mode=SHADOW')
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf', ok=False)
        put(root, '/proc/sys/kernel/random/boot_id', '21234567-1234-1234-1234-0123456789ab\n')
        result = run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf')
        require(updated_touch in (root / result.stdout.strip().lstrip('/')).read_text(),
                'Explicit rearm lost the new touch setting')
        # Removal and reinstallation use the same no-sha256sum environment.
        run('sh /data_persist/mx5-aa-dr/tools/uninstall.sh')
        for name, before in baseline.items():
            require((root / name).read_bytes() == before, 'Uninstall damaged baseline: ' + name)
        run('cd /tmp/mnt/sda1 && sh install.sh')
        require((base / 'guard/arm').exists(), 'Reinstall did not arm')
        run('cd /tmp/mnt/sda1 && sh uninstall.sh')
        require(not (base / 'guard/arm').exists(), 'Uninstall left a trial armed')
        print('PASS: stock ARM BusyBox/libc, damaged USB rejection, install, loader, one boot, '
              'collector UID/exit, status/finish (synthetic rows), USB export, touch update/rearm, uninstall, reinstall. Mount operations simulated; '
              'no OEM service or vehicle execution.')


if __name__ == '__main__':
    main()
