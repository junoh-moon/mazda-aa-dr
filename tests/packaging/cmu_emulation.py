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
import stat
import subprocess
import tarfile
import tempfile
import time

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
 } {print}' /proc/mounts > /proc/self/mounts.next || exit 93
/bin/mv /proc/self/mounts.next /proc/self/mounts
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


def require_mounts_link(root):
    mounts = root / 'proc/mounts'
    target = root / 'proc/self/mounts'
    require(mounts.is_symlink() and os.readlink(mounts) == 'self/mounts',
            'Emulated /proc/mounts lost the Linux self/mounts symlink')
    require(target.is_file() and not target.is_symlink(),
            'Emulated /proc/self/mounts is not a regular backing file')


def make_root(root, stock, bundle):
    root.chmod(0o755)  # The collector must traverse / after dropping privilege.
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
    # Preserve only names/IDs from the OEM factory backup, never credentials.
    # The update's passwdupdate payload confirms cmu=0 and service=1001.
    accounts = []
    for line in (stock / 'jci/integration/config-mfg-bak/passwd').read_text().splitlines():
        fields = line.split(':')
        if len(fields) == 7:
            accounts.append((fields[0], int(fields[2]), int(fields[3])))
    require(('cmu', 0, 0) in accounts and ('service', 1001, 1001) in accounts,
            'Unexpected stock account identities')
    put(root, '/config-mfg/passwd', ''.join(
        f'{name}:x:{uid}:{gid}:emulation:/tmp:/bin/sh\n' for name, uid, gid in accounts))
    (root / 'etc').mkdir(exist_ok=True)
    (root / 'etc/passwd').symlink_to('/config-mfg/passwd')
    put(root, '/etc/group', 'cmu:x:0:\nservice:x:1001:\nhmi:x:1002:\nbrowser:x:1003:\n')
    put(root, '/etc/nsswitch.conf', 'passwd: files\ngroup: files\n')
    put(root, '/proc/sys/kernel/random/boot_id', BOOT)
    # Linux exposes /proc/mounts as this kernel-owned symlink. The backing
    # contents remain authored: no host filesystem is mounted or remounted.
    put(root, '/proc/self/mounts', 'rootfs / rootfs rw 0 0\n/dev/root / relfs ro,relatime 0 0\n'
        'tmpfs /tmp tmpfs rw 0 0\n/dev/mtdblock8 /tmp/mnt/data_persist relfs rw 0 0\n'
        '/dev/sda1 /tmp/mnt/sda1 vfat ro,noexec 0 0\n')
    (root / 'proc/mounts').symlink_to('self/mounts')
    require_mounts_link(root)
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


def guest_environment():
    environment = dict(os.environ, PATH='/test-bin:/bin:/usr/bin:/sbin:/usr/sbin',
                       LC_ALL='C', LD_LIBRARY_PATH='/lib:/usr/lib')
    environment.pop('MX5DR_FIXTURE_ROOT', None)
    environment.pop('LD_PRELOAD', None)
    return environment


def execute_guest(root, script):
    require_mounts_link(root)
    result = subprocess.run(['/usr/sbin/chroot', str(root), '/bin/sh', '-c', script],
                            env=guest_environment(), capture_output=True, text=True, timeout=90)
    print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='')
    require_mounts_link(root)
    return result


def export_command_baselines(root):
    """Read actual stock command results without inventing a process/proc tree."""
    commands = {'uname': 'uname -a', 'processes': 'ps', 'kernel-log': 'dmesg',
                'usb-space': 'df -Pk /tmp/mnt/sda1',
                'persist-space': 'df -Pk /tmp/mnt/data_persist'}
    results = {}
    for key, command in commands.items():
        require_mounts_link(root)
        result = subprocess.run(['/usr/sbin/chroot', str(root), '/bin/sh', '-c', command],
                                env=guest_environment(), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=15)
        results[key] = (result.returncode, result.stdout)
        print('Stock diagnostic baseline: ' + json.dumps(
            dict(command=key, exit=result.returncode, bytes=len(result.stdout),
                 scope='authored_proc_and_host_kernel'), sort_keys=True), flush=True)
    for key in ('uname', 'usb-space', 'persist-space'):
        require(results[key][0] == 0 and results[key][1],
                key + ': stock diagnostic positive control failed')
    require_mounts_link(root)
    return results


def check_full_export(root, archive, command_baselines):
    """Check a quiescent authored guest; real live exports are not atomic."""
    base = root / 'tmp/mnt/data_persist/mx5-aa-dr'
    with tarfile.open(archive) as exported:
        members = {}
        for item in exported.getmembers():
            name = item.name.rstrip('/')
            require(not name.startswith('/') and '..' not in Path(name).parts and
                    name not in members, 'Ambiguous archive member: ' + name)
            members[name] = item

        def body(name):
            require(name in members and members[name].isfile(),
                    'Missing regular archive member: ' + name)
            return exported.extractfile(members[name]).read()

        def compare_source(path):
            name = path.relative_to(root).as_posix()
            require(name in members, 'Whole-source archive omitted ' + name)
            item, source = members[name], path.lstat()
            require((item.uid, item.gid, item.mode) ==
                    (source.st_uid, source.st_gid, stat.S_IMODE(source.st_mode)),
                    'Archive changed source UID/GID/mode: ' + name)
            if stat.S_ISREG(source.st_mode):
                require(body(name) == path.read_bytes(), 'Archive changed source bytes: ' + name)
            elif stat.S_ISDIR(source.st_mode):
                require(item.isdir(), 'Archive changed directory type: ' + name)
            elif stat.S_ISLNK(source.st_mode):
                require(item.issym() and item.linkname == os.readlink(path),
                        'Archive followed or changed source symlink: ' + name)
            else:
                raise RuntimeError('Unexpected authored source type: ' + name)

        # Preserve numeric ownership and permissions directly, including the
        # service-owned collector journal and root-owned guard/backups/tools.
        pending = [(base, 0)]
        while pending:
            path, depth = pending.pop()
            compare_source(path)
            if path.is_dir() and not path.is_symlink():
                require(depth < 8, 'Unexpected fixture installation depth')
                pending.extend((child, depth + 1) for child in path.iterdir())
        for name in ('usr/bin/autostart', 'jci/sm/sm.conf', 'jci/sm/sm_WCP.conf', 'jci/version.ini'):
            compare_source(root / name)

        reports = [name for name in members if Path(name).name == 'collection.txt' and
                   Path(name).parent.parent.as_posix() == 'tmp/mnt/sda1' and
                   Path(name).parent.name.startswith('mx5dr-diagnostics-')]
        require(len(reports) == 1, 'Expected one full-export diagnostic collection')
        stage = str(Path(reports[0]).parent)
        report = body(reports[0]).decode('utf-8')
        for line in ('export_schema=2', 'scope=current_recovery_boot',
                     'retained_scope=all_retained_boots', 'snapshot=sequential_nonatomic',
                     'installation=whole_tree'):
            require(line in report.splitlines(), 'Missing collection contract: ' + line)
        require(not (root / stage).exists(), 'Successful export left its USB staging directory')

        # The backing mount table is authored; /proc/mounts is still the actual
        # Linux-shaped alias. Export captures bytes while its USB is writable.
        writable_mounts = []
        for line in (root / 'proc/self/mounts').read_text().splitlines():
            fields = line.split()
            if fields[1] == '/tmp/mnt/sda1':
                fields[3] = ','.join('rw' if x == 'ro' else x for x in fields[3].split(','))
            writable_mounts.append(' '.join(fields) + '\n')
        require(body(stage + '/proc/mounts.txt') == ''.join(writable_mounts).encode(),
                'Export lost the proc alias target bytes or copied link text')
        for name in ('proc/sys/kernel/random/boot_id', 'proc/uptime'):
            require(body(stage + '/' + name + '.txt') == (root / name).read_bytes(),
                    'Export changed authored current proc data: ' + name)
        # Do not fabricate the absent proc data or interpret a ps header as
        # evidence of running OEM processes. dmesg can legitimately fail here.
        for name in ('proc/self/mountinfo', 'proc/version', 'proc/cmdline', 'proc/meminfo',
                     'proc/partitions', 'proc/1/status', 'proc/1/cmdline'):
            require(name + '=missing' in report.splitlines(),
                    'Absent authored proc source was reported as captured: ' + name)
        for key, (expected_exit, expected_output) in command_baselines.items():
            output = body(stage + '/' + key + '.txt')
            exit_text = body(stage + '/' + key + '.exit').decode('ascii').strip()
            require(exit_text == str(expected_exit), 'Command exit was lost: ' + key)
            require(len(output) <= 131072, 'Unbounded command capture: ' + key)
            require(not expected_output or output,
                    'Nonempty stock command output was lost: ' + key)
            if key == 'uname':
                require(output == expected_output, 'Stock uname output changed during export')
        print('PASS: whole BASE and current startup/SM bytes/UID/GID/mode, proc alias '
              'contents and stock command outputs/exits; synthetic proc and host kernel only', flush=True)


def account_regressions(stock, bundle):
    """Check installer ownership and the unmodified production collector together.

    Every case gets a fresh guest; only its synthetic NSS account identities
    vary. No fixture-root bypass, collector testing macro or host chown is used.
    """
    cases = (
        ('nonroot cmu', 500, 1001, 500, None),
        ('nonroot cmu without service', 500, None, 500, None),
        ('stock cmu/service', 0, 1001, 1001, None),
        ('missing service', 0, None, None, 'service account unavailable'),
        ('root service', 0, 0, None, 'service must have a nonzero UID'),
        ('missing cmu', None, 1001, None, 'cmu account unavailable'),
    )
    for label, cmu, service, expected_uid, error in cases:
        print('Account regression: ' + label, flush=True)
        with tempfile.TemporaryDirectory(prefix='mx5dr-account-') as tmp:
            root = Path(tmp)
            usb = make_root(root, stock, bundle)
            identities = [('jci', 0)]
            identities += [(name, uid) for name, uid in (('cmu', cmu), ('service', service))
                           if uid is not None]
            put(root, '/config-mfg/passwd', ''.join(
                f'{name}:x:{uid}:{uid}:account regression:/tmp:/bin/sh\n'
                for name, uid in identities))
            put(root, '/etc/group', ''.join(f'{name}:x:{uid}:\n' for name, uid in identities))
            baseline = {name: (root / name).read_bytes() for name in
                        ('jci/sm/sm.conf', 'jci/sm/sm_WCP.conf', 'usr/bin/autostart')}
            base = root / 'tmp/mnt/data_persist/mx5-aa-dr'
            installed = execute_guest(root, 'cd /tmp/mnt/sda1 && sh install.sh')
            if expected_uid is None:
                require(installed.returncode != 0 and error in installed.stderr,
                        label + ': installer did not reject the account')
                require(not base.exists() and not (root / 'mount.calls').exists(),
                        label + ': invalid account changed persistent storage or mounts')
                for name, before in baseline.items():
                    require((root / name).read_bytes() == before,
                            label + ': invalid account changed ' + name)
                # Run the real ARM collector independently of installer rejection.
                # Copy off the simulated noexec USB, without changing its bytes.
                collector = root / 'tmp/account-collector'
                shutil.copyfile(usb / 'mx5dr-collector', collector)
                collector.chmod(0o755)
                rejected = execute_guest(root, '/tmp/account-collector --session-seconds 1')
                require(rejected.returncode == 77,
                        label + ': collector account policy disagrees with installer')
                require(not base.exists(), label + ': rejected collector created logs')
            else:
                require(installed.returncode == 0, label + ': installation failed')
                logs = base / 'logs'
                require(logs.stat().st_uid == expected_uid,
                        label + ': installer assigned logs to the wrong UID')
                require(base.stat().st_uid == 0 and (base / 'guard').stat().st_uid == 0,
                        label + ': collector account owns privileged installation files')
                collected = execute_guest(root,
                    '/data_persist/mx5-aa-dr/mx5dr-collector --session-seconds 1')
                require(collected.returncode == 0, label + ': production collector failed')
                journal = logs / 'collector.0.jsonl'
                require(journal.is_file() and journal.stat().st_uid == expected_uid and
                        journal.stat().st_gid == expected_uid,
                        label + ': actual collector UID/GID disagrees with log ownership')
                rows = [json.loads(line) for line in journal.read_text().splitlines()]
                require(rows[0]['kind'] == 'collector_boot' and
                        rows[-1]['kind'] == 'collector_stop',
                        label + ': collector did not complete its real journal')
                require(not (logs / 'collector.pid').exists(),
                        label + ': collector did not clean its PID file')
                require(execute_guest(root, 'sh /data_persist/mx5-aa-dr/tools/uninstall.sh')
                        .returncode == 0, label + ': uninstall failed')
                for name, before in baseline.items():
                    require((root / name).read_bytes() == before,
                            label + ': uninstall damaged ' + name)
            print('PASS: ' + label, flush=True)
    recovery_account_regressions(stock, bundle)
    ownership_account_regressions(stock, bundle)
    print('PASS: 6 account variants, 2 account-loss recovery cases and collector '
          'ownership migration/active-lock/path-type regressions with stock ARM '
          'NSS/BusyBox, production installer ownership and collector UID/GID/rejection. '
          'Vehicle bus/SMDB unavailable; '
          'no OEM service execution.')


def recovery_account_regressions(stock, bundle):
    """Losing a collector account after installation cannot block removal."""
    for service_uid in (None, 0):
        label = 'uninstall with ' + ('missing service' if service_uid is None else 'root service')
        print('Account recovery regression: ' + label, flush=True)
        with tempfile.TemporaryDirectory(prefix='mx5dr-account-recovery-') as tmp:
            root = Path(tmp)
            make_root(root, stock, bundle)
            baseline = {name: (root / name).read_bytes() for name in
                        ('jci/sm/sm.conf', 'jci/sm/sm_WCP.conf', 'usr/bin/autostart')}
            base = root / 'tmp/mnt/data_persist/mx5-aa-dr'
            installed = execute_guest(root, 'cd /tmp/mnt/sda1 && sh install.sh')
            require(installed.returncode == 0 and (base / 'guard/arm').is_file(),
                    label + ': setup did not install and arm')
            collected = execute_guest(root,
                '/data_persist/mx5-aa-dr/mx5dr-collector --session-seconds 1')
            require(collected.returncode == 0, label + ': setup collector failed')
            journal = base / 'logs/collector.0.jsonl'
            original_log = journal.read_bytes()
            require(journal.stat().st_uid == 1001, label + ': wrong initial log owner')

            # Edit only the guest's credential-free NSS fixture after installation.
            passwd = root / 'config-mfg/passwd'
            changed = []
            for line in passwd.read_text().splitlines():
                fields = line.split(':')
                if fields[0] == 'service':
                    if service_uid is None:
                        continue
                    fields[2] = fields[3] = str(service_uid)
                changed.append(':'.join(fields))
            passwd.write_text('\n'.join(changed) + '\n')
            selection = execute_guest(root,
                'HERE=/tmp/mnt/sda1; . "$HERE/common.sh"; collector_user')
            require(selection.returncode != 0, label + ': account-loss setup was ineffective')

            removed = execute_guest(root, 'sh /data_persist/mx5-aa-dr/tools/uninstall.sh')
            require(removed.returncode == 0, label + ': account lookup blocked recovery')
            require(not (base / 'guard/arm').exists(), label + ': recovery left the trial armed')
            require('mode=OFF' in (base / 'mx5dr.conf').read_text().splitlines(),
                    label + ': recovery did not disable the runtime')
            for name, before in baseline.items():
                require((root / name).read_bytes() == before,
                        label + ': recovery did not restore ' + name)
            require(journal.read_bytes() == original_log and journal.stat().st_uid == 1001,
                    label + ': recovery changed existing collector evidence or ownership')
            require(not (base.parent / '.mx5dr-install-lock').exists(),
                    label + ': recovery left an installer lock')
            print('PASS: ' + label, flush=True)


def ownership_account_regressions(stock, bundle):
    """Migrate exactly the collector's files while respecting its real flock."""
    names = ('collector.lock', 'collector.pid', 'collector.0.jsonl', 'collector.1.jsonl')
    prepare = ('HERE=/tmp/mnt/sda1; . "$HERE/common.sh"; ALLOW_REMOUNT=1; '
               'prepare_collector_storage')
    collector = '/data_persist/mx5-aa-dr/mx5dr-collector --session-seconds '

    def accounts(root, cmu):
        put(root, '/config-mfg/passwd', 'jci:x:0:0:fixture:/tmp:/bin/sh\n'
            f'cmu:x:{cmu}:{cmu}:fixture:/tmp:/bin/sh\n'
            'service:x:1001:1001:fixture:/tmp:/bin/sh\n')
        put(root, '/etc/group', f'jci:x:0:\ncmu:x:{cmu}:\nservice:x:1001:\n')

    for active in (False, True):
        label = 'active old UID' if active else 'inactive old UID'
        print('Ownership regression: ' + label, flush=True)
        with tempfile.TemporaryDirectory(prefix='mx5dr-owner-') as tmp:
            root = Path(tmp)
            make_root(root, stock, bundle)
            accounts(root, 500)
            base = root / 'tmp/mnt/data_persist/mx5-aa-dr'
            logs = base / 'logs'
            require(execute_guest(root, 'cd /tmp/mnt/sda1 && sh install.sh').returncode == 0,
                    label + ': initial installation failed')
            # Produce both journal generations with the actual old-UID collector.
            for unused in range(2):
                require(execute_guest(root, collector + '1').returncode == 0,
                        label + ': old-UID collector failed')
            trace = logs / 'trace.0.jsonl'
            trace.write_text('unrelated AA evidence\n')
            require(execute_guest(root,
                'chown 500 /data_persist/mx5-aa-dr/logs/trace.0.jsonl').returncode == 0,
                label + ': AA ownership fixture failed')
            if active:
                process = subprocess.Popen(['/usr/sbin/chroot', str(root), '/bin/sh', '-c',
                    'exec ' + collector + '120'], env=guest_environment(),
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        try:
                            if (logs / 'collector.pid').exists() and 'collector_boot' in (
                                    logs / 'collector.0.jsonl').read_text():
                                break
                        except FileNotFoundError:
                            pass  # Normal startup rotation briefly replaces collector.0.
                        require(process.poll() is None, label + ': collector exited before lock check')
                        time.sleep(0.02)
                    require((logs / 'collector.pid').exists() and process.poll() is None,
                            label + ': collector did not start')
                    # Record the real BusyBox stat calls. An active collector
                    # may unlink its PID/rotate journals between any two probes.
                    # Busy-lock no-op must inspect only the stable lock and dir.
                    stat_probe = root / 'test-bin/stat'
                    put(root, '/test-bin/stat', '#!/bin/sh\n'
                        'printf \'%s\\n\' "$3" >> /tmp/ownership-stat-paths\n'
                        'exec /bin/busybox stat "$@"\n', 0o755)
                    try:
                        require(execute_guest(root, prepare).returncode == 0,
                                'An already-correct owner added a gate for the active collector')
                    finally:
                        stat_probe.unlink()
                    observed = set((root / 'tmp/ownership-stat-paths').read_text().splitlines())
                    stable = '/tmp/mnt/data_persist/mx5-aa-dr/logs'
                    require(observed == {stable, stable + '/collector.lock'},
                            'Busy-lock preparation inspected mutable collector paths')
                    require(process.poll() is None, 'No-op ownership preparation stopped the collector')
                    paths = [logs] + [logs / name for name in names]
                    before = [(path.stat().st_uid, path.stat().st_gid) for path in paths]
                    startup = (root / 'usr/bin/autostart').read_bytes()
                    arm = (base / 'guard/arm').read_bytes()
                    accounts(root, 0)
                    attempted = execute_guest(root, 'cd /tmp/mnt/sda1 && sh install.sh')
                    require(attempted.returncode != 0 and
                            'Collector is active; cannot transfer log ownership' in attempted.stderr,
                            'An active old-UID collector did not block ownership migration')
                    require(before == [(path.stat().st_uid, path.stat().st_gid) for path in paths],
                            'Busy ownership migration partially changed an owner')
                    require((root / 'usr/bin/autostart').read_bytes() == startup and
                            (base / 'guard/arm').read_bytes() == arm,
                            'Busy ownership migration changed startup or authorization')
                    require(process.poll() is None, 'Ownership migration stopped the active collector')
                finally:
                    # Cooperative completion only: no kill or PID-based signaling.
                    (logs / 'collector.stop').mkdir(exist_ok=True)
                    stdout, stderr = process.communicate(timeout=10)
                require(process.returncode == 0, stdout + stderr)
                print('PASS: same-UID active preparation and changed-UID busy rejection', flush=True)
            else:
                # A stopped/crashed collector can leave a 0600 diagnostic PID.
                (logs / 'collector.pid').write_text('stale fixture PID\n')
                require(execute_guest(root,
                    'chmod 0600 /data_persist/mx5-aa-dr/logs/collector.pid; '
                    'chown 500 /data_persist/mx5-aa-dr/logs/collector.pid').returncode == 0,
                    'Stale PID ownership fixture failed')
                before = {name: (logs / name).read_bytes() for name in names}
                require(all((logs / name).stat().st_uid == 500 for name in names),
                        'Migration fixture did not retain the previous UID')
                accounts(root, 0)
                require(execute_guest(root, 'cd /tmp/mnt/sda1 && sh install.sh').returncode == 0,
                        'Reinstall did not migrate old collector ownership')
                require(logs.stat().st_uid == 1001 and
                        all((logs / name).stat().st_uid == 1001 for name in names),
                        'Reinstall did not assign all collector files to the new UID')
                require(before == {name: (logs / name).read_bytes() for name in names},
                        'Ownership transfer truncated existing collector evidence')
                require(trace.stat().st_uid == 500 and trace.read_text() == 'unrelated AA evidence\n',
                        'Ownership transfer recursively changed AA evidence')
                require(execute_guest(root, collector + '1').returncode == 0,
                        'The new-UID production collector could not resume')
                require((logs / 'collector.1.jsonl').read_bytes() == before['collector.0.jsonl'],
                        'Collector restart lost the previous newest journal during normal rotation')
                rows = [json.loads(line) for line in (logs / 'collector.0.jsonl').read_text().splitlines()]
                require(rows[0]['kind'] == 'collector_boot' and rows[-1]['kind'] == 'collector_stop' and
                        (logs / 'collector.0.jsonl').stat().st_uid == 1001,
                        'The new-UID production collector did not complete its journal')
                print('PASS: four-file ownership transfer, preserved history and actual restart', flush=True)

                # Reject both dereference hazards and blocking special files,
                # including when the surrounding directory already has its UID.
                victim = root / 'tmp/account-victim'
                victim.write_text('outside collector ownership scope\n')
                require(execute_guest(root, 'chown 500 /tmp/account-victim').returncode == 0,
                        'Symlink victim fixture failed')
                for name in names:
                    path = logs / name
                    backup = logs / (name + '.fixture-backup')
                    original = path.exists()
                    if original:
                        path.rename(backup)
                    try:
                        for kind in ('symlink', 'fifo'):
                            if kind == 'symlink':
                                path.symlink_to('/tmp/account-victim')
                            else:
                                os.mkfifo(path, 0o600)
                            rejected = execute_guest(root, prepare)
                            require(rejected.returncode != 0 and
                                    'Not a regular non-symlink file' in rejected.stderr,
                                    name + ': unsafe ' + kind + ' was not rejected')
                            require(victim.stat().st_uid == 500 and
                                    victim.read_text() == 'outside collector ownership scope\n',
                                    name + ': ownership followed the symlink')
                            path.unlink()
                    finally:
                        if path.exists() or path.is_symlink():
                            path.unlink()
                        if original:
                            backup.rename(path)
                print('PASS: symlink/FIFO rejection at all four collector paths', flush=True)


def startup_menu_flow(stock, bundle):
    """Link menu 1→5→2 with real ARM guard and authored reboot/poll witnesses."""
    with tempfile.TemporaryDirectory(prefix='mx5dr-startup-menu-') as tmp:
        root = Path(tmp)
        usb = make_root(root, stock, bundle)
        reboot = root / 'sbin/reboot'
        require(reboot.is_symlink(), 'Expected stock BusyBox reboot applet link')
        reboot.unlink()
        new_boot = '71234567-1234-1234-1234-0123456789ab'
        put(root, '/sbin/reboot', '#!/bin/sh\n'
            f'printf "{new_boot}\\n" > /proc/sys/kernel/random/boot_id\n'
            'echo called > /reboot.witness\n', 0o755)

        def run(script, ok=True):
            result = execute_guest(root, script)
            require((result.returncode == 0) == ok,
                    f'Startup menu unexpected rc={result.returncode}: {script}')
            return result

        run("printf '1\\n0\\n' | sh /tmp/mnt/sda1/trial")
        base = root / 'tmp/mnt/data_persist/mx5-aa-dr'
        require((base / 'guard/arm').is_file(), 'Menu 1 did not arm actual ARM guard')
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf', ok=False)
        require((base / 'guard/arm').is_file() and
                not (base / 'guard/consumed').exists(),
                'Same-boot Service Manager retry consumed the next-boot arm')
        run("printf '5\\n' | sh /tmp/mnt/sda1/trial")
        require((root / 'reboot.witness').read_text() == 'called\n' and
                'boot_id=' + BOOT.strip() in (usb / 'reboot-request.txt').read_text(),
                'Menu 1→5 failed to preserve the pre-request boot ID')
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf')
        require((base / 'guard/last-boot').read_text() == new_boot + '\n' and
                not (base / 'guard/arm').exists(), 'Authored new boot did not consume arm')
        rows = [dict(kind='collector_boot', boot_id=new_boot, schema=1),
                dict(kind='poll', end_ns=99000000000, seq=0)]
        (base / 'logs/collector.0.jsonl').write_text(''.join(json.dumps(dict(
            stream='collector', collector_pid=123, observed_at_mono_ns=99000000000,
            producer_mono_ns=None, producer_time_status='unknown', **row),
            separators=(',', ':')) + '\n' for row in rows))
        status = run("printf '2\\n0\\n' | sh /tmp/mnt/sda1/trial", ok=False)
        for expected in ('reboot_check=new_boot_observed',
                         'startup_state=guard_committed_after_new_boot',
                         'one_boot=consumed_this_boot', 'config_mode=SHADOW',
                         'runtime_disable_next_start=absent',
                         'collector_poll_recent=observed'):
            require(expected in status.stdout, 'Menu 1→5→2 missing ' + expected)
        retained = next((int(token.split('=', 1)[1]) for token in status.stdout.split()
                         if token.startswith('retained_bytes=')), 0)
        require(retained > 0 and 'status_exit=1' in status.stdout,
                'Synthetic poll was not retained or missing AA runtime was overclaimed')
        require('reboot_check=new_boot_observed' in (usb / 'startup-result.txt').read_text(),
                'Menu 2 did not save the startup comparison to USB')
        config = base / 'mx5dr.conf'
        config_original = config.read_bytes()
        config.write_bytes(config_original + b'# changed after guard selection\n')
        changed = run("printf '2\\n0\\n' | sh /tmp/mnt/sda1/trial", ok=False)
        require('config_mode=SHADOW' in changed.stdout and
                'guard_config_binding=changed' in changed.stdout and
                'startup_state=guard_config_changed_since_selection' in changed.stdout,
                'Stock BusyBox borrowed a consumed guard after config changed')
        config.write_bytes(config_original)
        marker = base / 'guard/armed-boot'
        marker.write_bytes(BOOT.strip().encode() + b'\x00')
        damaged = run("printf '2\\n0\\n' | sh /tmp/mnt/sda1/trial", ok=False)
        require('guard_armed_boot=invalid' in damaged.stdout and
                'startup_state=guard_selected_reboot_unconfirmed' in damaged.stdout,
                'Stock BusyBox accepted a same-size NUL-corrupt arming marker')
        marker.write_text(BOOT)
        consumed = base / 'guard/consumed'
        consumed_original = consumed.read_bytes()
        consumed_damaged = bytearray(consumed_original)
        consumed_damaged[25] = 0
        consumed.write_bytes(consumed_damaged)
        damaged = run("printf '2\\n0\\n' | sh /tmp/mnt/sda1/trial", ok=False)
        require('guard_consumed=invalid' in damaged.stdout and
                'startup_state=guard_committed_after_new_boot' not in damaged.stdout,
                'Stock BusyBox accepted an internal NUL in the v3 consumed marker')
        consumed.write_bytes(consumed_original)
        config.write_bytes(config_original.replace(b'mode=', b'mo\x00e=', 1))
        damaged = run("printf '2\\n0\\n' | sh /tmp/mnt/sda1/trial", ok=False)
        require('config_mode=unconfirmed' in damaged.stdout and
                'startup_state=guard_committed_after_new_boot' not in damaged.stdout,
                'Stock BusyBox accepted an internal NUL in the trial config')
        config.write_bytes(config_original)
        put(root, '/test-bin/od', '#!/bin/sh\nexit 1\n', 0o755)
        missing_od = run("printf '2\\n0\\n' | sh /tmp/mnt/sda1/trial", ok=False)
        require('Invalid current boot ID' in missing_od.stdout + missing_od.stderr,
                'Failed od was hidden by a successful tr pipeline')
        print('PASS: stock ARM BusyBox menu1→5→2, same-boot retry refusal, '
              'real guard selection, authored reboot '
              'and collector witnesses, seven startup fields and corrupt-byte rejection; '
              'no PID 1/AA/VBS execution.',
              flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--account-regressions-only', action='store_true',
                        help='Run isolated account selection and account-loss recovery checks')
    parser.add_argument('--ownership-regressions-only', action='store_true',
                        help='Run collector ownership migration and active-lock checks')
    args = parser.parse_args()
    require(os.geteuid() == 0, 'Run inside an isolated Linux container as UID 0')
    if args.ownership_regressions_only:
        ownership_account_regressions(args.stock.resolve(), args.bundle.resolve())
        return
    if args.account_regressions_only:
        account_regressions(args.stock.resolve(), args.bundle.resolve())
        return
    default_mode = (args.bundle / 'bundle-default-mode').read_text().strip()
    require(default_mode in ('OBSERVE', 'SHADOW'), 'Unsupported bundle default mode')
    with tempfile.TemporaryDirectory(prefix='mx5dr-cmu-') as tmp, \
            tempfile.TemporaryDirectory(prefix='mx5dr-detached-usb-') as detached:
        root = Path(tmp)
        usb = make_root(root, args.stock.resolve(), args.bundle.resolve())
        def run(script, ok=True):
            result = execute_guest(root, script)
            require((result.returncode == 0) == ok, f'Unexpected rc={result.returncode}: {script}')
            return result

        # Ensure the intended guest is actually active (some host translators
        # cannot run PRoot correctly). This cannot accidentally use host tools.
        result = run('/bin/busybox 2>&1; test ! -e /src; test "$(id -u)" = 0; '
                     '! id root 2>/dev/null; ! command -v sha256sum; '
                     'test "$(id -u cmu)" = 0; test "$(id -u service)" = 1001')
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
        run("printf '1\\n' | sh /tmp/mnt/sda1/trial", ok=False)
        require(not base.exists(), 'Damaged bundle made installation changes')
        lib.write_bytes(saved)
        run("printf '1\\n' | sh /tmp/mnt/sda1/trial")
        require((base / 'guard/arm').is_file(), 'Actual ARM guard did not arm')
        require((base / 'guard/arm').stat().st_size == 538,
                'Unexpected real ARM guard manifest length')
        require((base / 'guard/armed-boot').read_text() == BOOT,
                'Installer did not retain the arming Linux boot identity')
        require('mode=' + default_mode in (base / 'mx5dr.conf').read_text().splitlines(),
                'Installed config differs from the bundle default mode')
        for name in ('jci/sm/sm.conf', 'jci/sm/sm_WCP.conf'):
            require((root / name).read_bytes() == baseline[name], 'Persistent SM config changed')
        require((root / 'mount.calls').read_text().splitlines() ==
                ['-o remount,rw /', '-o remount,ro /'], 'Incorrect containing mount selected/restored')
        require(not (base.parent / '.mx5dr-install-lock').exists(), 'Installer left a lock')
        # The installation USB is absent from the guest during loader/guard/
        # collector execution. Startup must use the installed persistent files.
        saved_mounts = (root / 'proc/mounts').read_text()
        usb.rename(Path(detached) / 'usb')
        (root / 'proc/self/mounts').write_text(''.join(
            line for line in saved_mounts.splitlines(keepends=True)
            if line.split()[1] != '/tmp/mnt/sda1'))
        require_mounts_link(root)
        require(not usb.exists(), 'Installation USB was not detached')
        # Exercise all three experimental DSOs with the stock dynamic loader/libc.
        # glibc returns success even when it ignores a missing LD_PRELOAD DSO.
        # Require positive initialization evidence, not just /bin/true's status.
        missing = run('LD_PRELOAD=/missing-mx5dr.so /bin/true')
        require('cannot be preloaded' in missing.stderr, 'Missing DSO negative control failed')
        for name in ('libmx5dr.so', 'libmx5dr-vimtap.so', 'libmx5dr-ldstap.so'):
            path = '/data_persist/mx5-aa-dr/' + name
            loaded = run('LD_DEBUG=libs LD_PRELOAD=' + path + ' /bin/true')
            require('cannot be preloaded' not in loaded.stderr and
                    'calling init: ' + path in loaded.stderr,
                    'Stock loader did not initialize ' + name)
        # Authored new kernel identity models the menu-5 reboot boundary. The
        # guest still does not boot OEM PID 1 or a physical CMU.
        put(root, '/proc/sys/kernel/random/boot_id', '11234567-1234-1234-1234-0123456789ab\n')
        result = run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf')
        trial = root / result.stdout.strip().lstrip('/')
        initial_trial = trial.read_text()
        require(TOUCH in initial_trial, 'Initial trial lost AA touch')
        require('/data_persist/mx5-aa-dr/libmx5dr.so' in initial_trial and
                '/tmp/mnt/sda1' not in initial_trial,
                'Trial preload depends on the installation USB')
        require(('/libmx5dr-vimtap.so' in initial_trial) == (default_mode == 'SHADOW'),
                'Initial trial VBS tap differs from the bundle default mode')
        require('/data_persist/mx5-aa-dr/libmx5dr-ldstap.so' in initial_trial,
                'Initial trial lacks LDS observation preload')
        consumed = (base / 'guard/consumed').read_text().splitlines()
        require(len(consumed) == 9 and consumed[0] == 'mx5dr-one-boot-v3' and
                consumed[8] == hashlib.sha256((base / 'libmx5dr-ldstap.so').read_bytes()).hexdigest(),
                'Consumed arm did not bind the exact installed LDS product')
        print('PASS: ' + default_mode + ' bundle default and initial trial preload selection', flush=True)
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf', ok=False)
        put(root, '/proc/sys/kernel/random/boot_id', '21234567-1234-1234-1234-0123456789ab\n')
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf', ok=False)
        # Later status fixtures describe SHADOW. Select that mode explicitly for
        # this simulated new boot, including a real arm/consume transition.
        run('sh /data_persist/mx5-aa-dr/tools/arm.sh --mode=SHADOW')
        require((base / 'guard/armed-boot.previous').read_text() == BOOT and
                (base / 'guard/armed-boot').read_text() ==
                '21234567-1234-1234-1234-0123456789ab\n',
                'Rearm did not separate old and new boot identities')
        put(root, '/proc/sys/kernel/random/boot_id', '31234567-1234-1234-1234-0123456789ab\n')
        result = run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf')
        shadow_trial = root / result.stdout.strip().lstrip('/')
        require('mode=SHADOW' in (base / 'mx5dr.conf').read_text().splitlines() and
                TOUCH in shadow_trial.read_text() and '/libmx5dr-vimtap.so' in shadow_trial.read_text() and
                '/libmx5dr-ldstap.so' in shadow_trial.read_text(),
                'Explicit SHADOW rearm did not prepare the status fixture mode')
        # Actual collector executes with guest NSS and drops UID to service. Missing
        # vehicle DBus/SMDB is expected here; it must still start/stop its journal.
        run('/data_persist/mx5-aa-dr/mx5dr-collector --session-seconds 1')
        journal = base / 'logs/collector.0.jsonl'
        require(journal.is_file() and journal.stat().st_uid == 1001, 'Collector failed service ownership')
        require('collector_boot' in journal.read_text(), 'Collector boot evidence missing')
        require('collector_stop' in journal.read_text(), 'Collector stop evidence missing')
        require(not usb.exists(), 'USB reappeared before persistent execution ended')
        (Path(detached) / 'usb').rename(usb)
        (root / 'proc/self/mounts').write_text(saved_mounts)
        require_mounts_link(root)
        print('PASS: absent installation USB during stock loader, actual guard selection '
              'and collector execution; no OEM service startup claimed', flush=True)
        # Run the parked helpers with explicitly synthetic sensor/health rows.
        # No OEM sensor callback or AA process is claimed by these records.
        current_boot = (root / 'proc/sys/kernel/random/boot_id').read_text().strip()
        original_collector = journal.read_bytes()
        trace = [dict(kind='boot', boot_id=current_boot, mono_ns=1000000000, mode=4),
                 dict(kind='shadow_boot', active=True, capture_active=True),
                 dict(kind='health', mono_ns=99000000000, hook_installed=True,
                      audit_fault=0, dropped=0, capture_active=True, computation_active=True),
                 dict(kind='position', mono_ns=99000000000, mode=1),
                 dict(kind='shadow', mono_ns=99000000000, domain='model', assist_ready=False,
                      model_valid=False, events=4, result='E_NO_SEED', pipeline='WAITING'),
                 dict(kind='motion_batch', schema=1, epoch=1, events=[
                     [sensor, sensor, 99000000000, 90000, 0, 0, 0, 0, 1, 0] for sensor in (1, 2, 3)]),
                 dict(kind='shadow', mono_ns=99000000000, domain='model', assist_ready=False,
                      model_valid=True, events=4, result='OK', pipeline='OK', drain_calls_total=40),
                 dict(kind='shadow_session', mono_ns=99500000000, reset=True)]
        (base / 'logs/trace.0.jsonl').write_text(''.join(json.dumps(row, separators=(',', ':')) + '\n' for row in trace))
        collector = [dict(kind='collector_boot', boot_id=current_boot, schema=1),
                     dict(kind='poll', end_ns=99000000000, seq=0)]
        journal.write_text(''.join(json.dumps(dict(stream='collector', collector_pid=123,
            observed_at_mono_ns=99000000000, producer_mono_ns=None,
            producer_time_status='unknown', **row), separators=(',', ':')) + '\n' for row in collector))
        status = run("printf '2\\n0\\n' | sh /tmp/mnt/sda1/trial")
        require('startup_state=guard_committed_after_new_boot' in status.stdout and
                'linux_reboot_after_arm=observed' in status.stdout,
                'Status did not prove a distinct authored Linux boot after arming')
        require('model_solution=not_observed' in status.stdout and
                'retained_model_diagnostic_records=2 model_valid_records=1 drain_attempt_records=1' in status.stdout,
                'AA reset lost retained MODEL rows or promoted them to current readiness')
        (base / 'logs/capture.done').write_text(current_boot + '\n')
        journal.write_bytes(original_collector)
        command_baselines = export_command_baselines(root)
        run("printf '3\\n' | sh /tmp/mnt/sda1/trial")
        report = (usb / 'trial-result.txt').read_text()
        require('finish_exit=0' in report and 'export_exit=0' in report,
                'USB menu did not preserve separate finish/export outcomes')
        archive = next(usb.glob('mx5dr-logs-*.tar'))
        require(archive.with_suffix('.tar.sha256').read_text().split()[0] ==
                hashlib.sha256(archive.read_bytes()).hexdigest(), 'Export checksum mismatch')
        check_full_export(root, archive, command_baselines)
        require((root / 'mount.calls').read_text().splitlines()[-2:] ==
                ['-o remount,rw /tmp/mnt/sda1', '-o remount,ro /tmp/mnt/sda1'], 'USB mount not restored')
        # Authored rebooted USB return: no menu2, reinstall or arm. Previous
        # receipt times exceed the new uptime and must never become current.
        raw = (base / 'logs/trace.0.jsonl').read_bytes()
        startup = (root / 'usr/bin/autostart').read_bytes()
        config = (base / 'mx5dr.conf').read_bytes()
        put(root, '/proc/sys/kernel/random/boot_id', '41234567-1234-1234-1234-0123456789ab\n')
        put(root, '/proc/uptime', '2.00 0.00\n')
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf', ok=False)
        for old_ack in (False, True):
            ack = base / 'logs/capture.done'
            if old_ack:
                ack.write_text(current_boot + '\n')
            else:
                ack.unlink(missing_ok=True)
            before = set(usb.glob('mx5dr-logs-*.tar'))
            run("printf '3\\n' | sh /tmp/mnt/sda1/trial", ok=False)
            report = (usb / 'trial-result.txt').read_text()
            for expected in ('status_scope=current_boot', 'finish_scope=current_boot',
                             'export_scope=all_retained_boots', 'status_exit=1',
                             'finish_exit=1', 'export_exit=0',
                             'runtime_current_boot=unavailable',
                             'retained_runtime_last_boot=previous_boot boot_id=' + current_boot,
                             'position_records=1 motion_batches=1',
                             'retained_model_diagnostic_records=2 model_valid_records=1 drain_attempt_records=1'):
                require(expected in report, 'Rebooted USB report missing: ' + expected)
            exports = set(usb.glob('mx5dr-logs-*.tar')) - before
            require(len(exports) == 1, 'Rebooted USB return did not create one archive')
            archive = exports.pop()
            require(archive.with_suffix('.tar.sha256').read_text().split()[0] ==
                    hashlib.sha256(archive.read_bytes()).hexdigest(), 'Previous-boot export checksum mismatch')
            check_full_export(root, archive, command_baselines)
            require((base / 'logs/trace.0.jsonl').read_bytes() == raw and
                    (base / 'mx5dr.conf').read_bytes() == config and
                    (root / 'usr/bin/autostart').read_bytes() == startup and
                    not (base / 'guard/arm').exists(), 'Retrieval changed raw files or startup state')
            print('PASS: rebooted single-port menu3 raw export with ' +
                  ('old acknowledgement rejected' if old_ack else 'missing acknowledgement unconfirmed'), flush=True)
        # Restore the authored boot for the pre-existing same-boot rearm test.
        put(root, '/proc/sys/kernel/random/boot_id', current_boot + '\n')
        put(root, '/proc/uptime', '100.00 0.00\n')
        # A later touch edit must never be paired with an old trial by raw arm.
        # Explicit rearm rebuilds the pair, while still refusing this same boot.
        updated_touch = TOUCH.replace('.so', '-updated.so')
        path = root / 'jci/sm/sm.conf'
        path.write_text(path.read_text().replace(TOUCH, updated_touch))
        baseline['jci/sm/sm.conf'] = path.read_bytes()
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard arm', ok=False)
        run('sh /data_persist/mx5-aa-dr/tools/arm.sh --mode=SHADOW')
        run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf', ok=False)
        put(root, '/proc/sys/kernel/random/boot_id', '51234567-1234-1234-1234-0123456789ab\n')
        result = run('/data_persist/mx5-aa-dr/guard/mx5dr-guard select /jci/sm/sm.conf')
        require(updated_touch in (root / result.stdout.strip().lstrip('/')).read_text(),
                'Explicit rearm lost the new touch setting')
        # Reject the final marker rename after a real ARM guard has armed.
        # A failed publication must revoke arm and preserve the previous ID.
        prior_armed = (base / 'guard/armed-boot').read_bytes()
        put(root, '/test-bin/mv', '#!/bin/sh\nfor last do :; done\n'
            'case "$last" in */guard/armed-boot) exit 93;; esac\n'
            'exec /bin/mv "$@"\n', 0o755)
        failed = run('sh /data_persist/mx5-aa-dr/tools/arm.sh --mode=SHADOW', ok=False)
        (root / 'test-bin/mv').unlink()
        require('trial arm revoked' in failed.stderr and
                not (base / 'guard/arm').exists() and
                not (base / 'guard/armed-boot').exists() and
                (base / 'guard/armed-boot.previous').read_bytes() == prior_armed,
                'Failed post-arm marker publication left a live arm or reused the old ID')
        # Removal and reinstallation use the same no-sha256sum environment.
        run("printf '4\\n' | sh /tmp/mnt/sda1/trial")
        for name, before in baseline.items():
            require((root / name).read_bytes() == before, 'Uninstall damaged baseline: ' + name)
        # Removal retains collected evidence. A parked USB return must retrieve
        # those exact bytes without reinstalling or arming a new experiment.
        before = set(usb.glob('mx5dr-logs-*.tar'))
        removed_config = (base / 'mx5dr.conf').read_bytes()
        run("printf '3\\n' | sh /tmp/mnt/sda1/trial", ok=False)
        report = (usb / 'trial-result.txt').read_text()
        require('finish_exit=1' in report and 'export_exit=0' in report,
                'Post-uninstall retrieval lost separate finish/export outcomes')
        exports = set(usb.glob('mx5dr-logs-*.tar')) - before
        require(len(exports) == 1, 'Post-uninstall retrieval did not create one archive')
        archive = exports.pop()
        require(archive.with_suffix('.tar.sha256').read_text().split()[0] ==
                hashlib.sha256(archive.read_bytes()).hexdigest(),
                'Post-uninstall export checksum mismatch')
        check_full_export(root, archive, command_baselines)
        require((base / 'logs/trace.0.jsonl').read_bytes() == raw and
                (base / 'mx5dr.conf').read_bytes() == removed_config and
                not (base / 'guard/arm').exists(), 'Retrieval changed removed state or raw files')
        for name, before in baseline.items():
            require((root / name).read_bytes() == before, 'Retrieval changed OEM baseline: ' + name)
        print('PASS: menu4 removal then menu3 retained raw export, unchanged OFF/no-arm '
              'state and Linux mounts symlink', flush=True)
        put(root, '/test-bin/mv', '#!/bin/sh\nfor last do :; done\n'
            'case "$last" in */guard/armed-boot) exit 93;; esac\n'
            'exec /bin/mv "$@"\n', 0o755)
        failed = run('cd /tmp/mnt/sda1 && sh install.sh', ok=False)
        (root / 'test-bin/mv').unlink()
        require('trial arm revoked' in failed.stderr and
                not (base / 'guard/arm').exists() and
                'ONE-BOOT BEGIN' in (root / 'usr/bin/autostart').read_text(),
                'Failed post-arm installer marker publication left an armed trial')
        run('cd /tmp/mnt/sda1 && sh uninstall.sh')
        run('cd /tmp/mnt/sda1 && sh install.sh')
        require((base / 'guard/arm').exists(), 'Reinstall did not arm')
        run('cd /tmp/mnt/sda1 && sh uninstall.sh')
        require(not (base / 'guard/arm').exists(), 'Uninstall left a trial armed')
        # Full parked recovery order after a fresh staged capture: export,
        # remove, request reboot, then inspect the authored new Linux identity.
        # Replace only the guest reboot applet with a witness; never request a
        # real container/host reboot or claim PID 1 shutdown was exercised.
        run('cd /tmp/mnt/sda1 && sh install.sh')
        recovery_boot = (root / 'proc/sys/kernel/random/boot_id').read_text()
        (base / 'logs/capture.done').write_text(recovery_boot)
        before = set(usb.glob('mx5dr-logs-*.tar'))
        run("printf '3\\n' | sh /tmp/mnt/sda1/trial")
        require((base / 'logs/capture.stop').is_dir() and
                len(set(usb.glob('mx5dr-logs-*.tar')) - before) == 1,
                'Recovery menu3 did not stop and export current capture')
        run("printf '4\\n' | sh /tmp/mnt/sda1/trial")
        require('mode=OFF' in (base / 'mx5dr.conf').read_text() and
                not (base / 'guard/arm').exists(), 'Recovery menu4 did not disarm and switch OFF')
        reboot = root / 'sbin/reboot'
        require(reboot.is_symlink(), 'Expected stock reboot applet link')
        reboot.unlink()
        put(root, '/sbin/reboot', '#!/bin/sh\n'
            'printf "61234567-1234-1234-1234-0123456789ab\\n" > /proc/sys/kernel/random/boot_id\n'
            'printf "2.00 0.00\\n" > /proc/uptime\n'
            'echo called > /reboot.witness\n', 0o755)
        run("printf '5\\n' | sh /tmp/mnt/sda1/trial")
        require((root / 'reboot.witness').read_text() == 'called\n',
                'Recovery menu5 did not hand off to reboot witness')
        recovery = run("printf '2\\n0\\n' | sh /tmp/mnt/sda1/trial", ok=False)
        require('reboot_check=new_boot_observed' in recovery.stdout and
                'config_mode=OFF' in recovery.stdout and
                'guard_arm=absent' in recovery.stdout and
                'runtime_disable_next_start=absent' in recovery.stdout and
                (base / 'logs/capture.stop').is_dir(),
                'Recovery menu2 lost the new boot, OFF, disarmed or stop evidence')
        print('PASS: stock ARM BusyBox recovery menu3→4→5→2, authored reboot witness, '
              'new boot ID, OFF, no arm and retained stop marker; no PID 1/vehicle reboot.',
              flush=True)
        print('PASS: stock ARM BusyBox/libc, damaged USB rejection, install, loader, one boot, '
              'collector UID/exit, numeric USB menu install/status/finish/export/report/remove '
              '(explicit SHADOW, synthetic rows), touch update/rearm, reinstall. Mount operations simulated; '
              'no OEM service or vehicle execution.')
    startup_menu_flow(args.stock.resolve(), args.bundle.resolve())


if __name__ == '__main__':
    main()
