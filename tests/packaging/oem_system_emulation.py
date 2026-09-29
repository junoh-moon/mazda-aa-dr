#!/usr/bin/env python3
"""Build a private OEM initramfs and record bounded, isolated ARM QEMU runs.

The initramfs and console logs contain private OEM material: do not publish them.
This tool does not declare a run successful. A timeout, live process, or clean
QEMU exit is not proof of service initialization or vehicle operation.
"""
import argparse
import datetime
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time

GUEST_INIT = Path(__file__).with_name('oem_guest_init.sh')
CMU_ENTRY = 0x10010000
CMU_MACHINE_ID = 3837


def digest(path):
    value = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def regular_input(path, label):
    require(path.is_file(), label + ' must be an existing file')
    return path.resolve()


def unused_outputs(*paths):
    for path in paths:
        require(not os.path.lexists(path), 'Output already exists; choose a new name')
        path.parent.mkdir(parents=True, exist_ok=True)


def write_json(path, value):
    created = False
    try:
        with path.open('x', encoding='utf-8') as stream:
            created = True
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write('\n')
    except BaseException:
        if created:
            path.unlink(missing_ok=True)
        raise


def guest_path(root, name):
    """Resolve absolute guest symlinks inside the extracted tree, never the host."""
    parts = name.lstrip('/').split('/')
    resolved = []
    links = 0
    while parts:
        part = parts.pop(0)
        if part in ('', '.'):
            continue
        if part == '..':
            require(bool(resolved), 'Guest path escapes the extracted root')
            resolved.pop()
            continue
        candidate = root.joinpath(*resolved, part)
        if candidate.is_symlink():
            links += 1
            require(links <= 40, 'Too many guest symlinks')
            target = os.readlink(candidate)
            if target.startswith('/'):
                resolved = []
            parts = target.split('/') + parts
        else:
            resolved.append(part)
    return root.joinpath(*resolved)


def put_guest(root, name, contents, mode=0o644):
    destination = guest_path(root, name)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(contents, encoding='utf-8')
    destination.chmod(mode)
    os.chown(destination, 0, 0)


def sanitize_accounts(root):
    # Retain names and numeric IDs only; never print or record credential fields.
    backup = guest_path(root, '/jci/integration/config-mfg-bak/passwd')
    accounts = []
    for line in backup.read_text().splitlines():
        fields = line.split(':')
        require(len(fields) == 7 and re.fullmatch(r'[A-Za-z0-9_.-]+', fields[0])
                and fields[2].isdigit() and fields[3].isdigit(),
                'Unexpected OEM account record format')
        accounts.append((fields[0], int(fields[2]), int(fields[3])))
    require(accounts and len({row[0] for row in accounts}) == len(accounts),
            'Missing or duplicate OEM account names')
    require(any(uid == 0 for _, uid, _ in accounts), 'OEM UID 0 account missing')
    put_guest(root, '/config-mfg/passwd', ''.join(
        f'{name}:x:{uid}:{gid}::/tmp:/bin/sh\n' for name, uid, gid in accounts))
    passwd_link = root / 'etc/passwd'
    if passwd_link.is_symlink() or passwd_link.is_file():
        passwd_link.unlink()
    require(not passwd_link.exists(), 'Unexpected guest passwd object')
    passwd_link.symlink_to('/config-mfg/passwd')
    groups = []
    group_path = guest_path(root, '/etc/group')
    if group_path.exists():
        for line in group_path.read_text().splitlines():
            fields = line.split(':')
            require(len(fields) == 4 and re.fullmatch(r'[A-Za-z0-9_.-]+', fields[0])
                    and fields[2].isdigit(), 'Unexpected OEM group record format')
            groups.append((fields[0], int(fields[2])))
    for name, _, gid in accounts:
        if not any(existing_gid == gid for _, existing_gid in groups):
            require(not any(existing_name == name for existing_name, _ in groups),
                    'Cannot derive a unique guest primary group')
            groups.append((name, gid))
    put_guest(root, '/etc/group', ''.join(f'{name}:x:{gid}:\n' for name, gid in groups))
    put_guest(root, '/etc/nsswitch.conf', 'passwd: files\ngroup: files\nhosts: files dns\n')


def bundle_identities(bundle):
    require(bundle.is_dir() and (bundle / 'install.sh').is_file(),
            'Bundle must be an extracted USB bundle directory')
    files = {}
    for path in sorted(bundle.rglob('*')):
        require(not path.is_symlink(), 'Bundle symlinks are not supported')
        if path.is_file():
            files[path.relative_to(bundle).as_posix()] = digest(path)
        else:
            require(path.is_dir(), 'Bundle contains a special file')
    require(bool(files), 'Empty bundle')
    tree_hash = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    return {'tree_sha256': tree_hash, 'files_sha256': files}


def archive_names(root):
    # Walk without following firmware links such as /mnt -> /tmp/mnt. cpio's
    # default also archives symlinks themselves, including dangling aliases.
    names = ['.']
    for directory, subdirs, files in os.walk(root, followlinks=False):
        subdirs.sort()
        for name in sorted(subdirs + files):
            names.append('./' + (Path(directory) / name).relative_to(root).as_posix())
    return b'\0'.join(os.fsencode(name) for name in names) + b'\0'


def build(args):
    require(platform.system() == 'Linux' and os.geteuid() == 0,
            'build requires UID 0 inside an isolated Linux container')
    require(shutil.which('tar') and shutil.which('cpio'), 'GNU tar and cpio are required')
    source = regular_input(args.rootfs_tar, 'Rootfs tar')
    init = regular_input(GUEST_INIT, 'Diagnostic guest init')
    touch = regular_input(args.touch, 'Touch library') if args.touch else None
    bundle = args.bundle.resolve()
    output = args.output.absolute()
    metadata_path = Path(str(output) + '.json')
    unused_outputs(output, metadata_path)
    identities = bundle_identities(bundle)
    metadata = {
        'schema': 1, 'operation': 'build', 'scope': 'Private OEM userspace initramfs',
        'publishable': False,
        'inputs': {'rootfs_tar_sha256': digest(source), 'bundle': identities,
                   'diagnostic_init_sha256': digest(init),
                   'touch_sha256': digest(touch) if touch else None},
        'account_model': 'OEM names and UID/GID only; credential fields replaced with x',
        'modifications': ['/init diagnostic harness', '/config-mfg/passwd sanitized identities',
                          '/etc/passwd link', '/etc/group identities', '/etc/nsswitch.conf',
                          '/validation/usb', '/validation/touch.so if supplied',
                          '/sbin/init_target byte-identical probe entry', '/dev/console', '/dev/null'],
        'oem_executed_during_build': False,
    }
    created = False
    try:
        # /tmp is container-local: do not extract device nodes or firmware
        # symlinks onto a host bind mount or require a host mount operation.
        with tempfile.TemporaryDirectory(prefix='mx5dr-oem-build-', dir='/tmp') as tmp:
            work = Path(tmp)
            root = work / 'root'
            root.mkdir(mode=0o755)
            subprocess.run(['tar', '--same-owner', '-xf', str(source), '-C', str(root)], check=True)
            require(digest(source) == metadata['inputs']['rootfs_tar_sha256'],
                    'Rootfs archive changed during extraction')
            require((root / 'jci/sm/sm').is_file(), 'Rootfs tar must contain the root filesystem directly')
            sanitize_accounts(root)
            original_init = guest_path(root, '/sbin/init')
            require(original_init.is_file(), 'Original OEM init is missing')
            metadata['inputs']['oem_init_sha256'] = digest(original_init)
            probe = root / 'sbin/init_target'
            if os.path.lexists(probe):
                require(digest(guest_path(root, '/sbin/init_target')) == digest(original_init),
                        'Existing init probe differs from the original OEM init')
            else:
                shutil.copy2(original_init, probe)
                info = original_init.stat()
                os.chown(probe, info.st_uid, info.st_gid)
            init_destination = root / 'init'
            require(not os.path.lexists(init_destination), 'Rootfs already contains /init')
            shutil.copy2(init, init_destination)
            init_destination.chmod(0o755)
            require(digest(init_destination) == metadata['inputs']['diagnostic_init_sha256'],
                    'Diagnostic init changed while building')
            validation = root / 'validation'
            require(not os.path.lexists(validation), 'Rootfs already contains /validation')
            validation.mkdir(mode=0o755)
            shutil.copytree(bundle, validation / 'usb')
            require(bundle_identities(validation / 'usb') == identities,
                    'Bundle changed while copying')
            if touch:
                shutil.copy2(touch, validation / 'touch.so')
                require(digest(validation / 'touch.so') == metadata['inputs']['touch_sha256'],
                        'Touch library changed while copying')
            for name, major, minor, mode in (('console', 5, 1, 0o600), ('null', 1, 3, 0o666)):
                device = guest_path(root, '/dev/' + name)
                device.parent.mkdir(parents=True, exist_ok=True)
                if os.path.lexists(device):
                    device.unlink()
                os.mknod(device, stat.S_IFCHR | mode, os.makedev(major, minor))
            # Avoid the stdin/stdout pipe deadlock of Popen(cpio)->gzip. cpio
            # drains communicate(input=names) straight into a disk file first.
            archive = work / 'root.cpio'
            with archive.open('wb') as stream:
                subprocess.run(['cpio', '--null', '--create', '--format=newc',
                                '--reproducible', '--quiet'], cwd=root,
                               input=archive_names(root), stdout=stream, check=True)
            with output.open('xb') as destination:
                created = True
                with gzip.GzipFile(filename='', mode='wb', fileobj=destination, mtime=0) as compressed:
                    with archive.open('rb') as stream:
                        shutil.copyfileobj(stream, compressed, 1024 * 1024)
            metadata['initrd_sha256'] = digest(output)
            metadata['initrd_bytes'] = output.stat().st_size
            write_json(metadata_path, metadata)
    except BaseException:
        if created:
            output.unlink(missing_ok=True)
        raise
    print('Created private initramfs and provenance sidecar; no OEM code was executed.')
    print('initrd_sha256=' + metadata['initrd_sha256'])
    return 0


class RemoteGDB:
    """Minimal acknowledged GDB remote protocol over a private Unix socket."""
    def __init__(self, stream, deadline):
        self.stream = stream
        self.deadline = deadline

    def byte(self):
        self.stream.settimeout(max(0.001, self.deadline - time.monotonic()))
        value = self.stream.recv(1)
        require(bool(value), 'QEMU GDB connection closed unexpectedly')
        return value

    def request(self, command):
        payload = command.encode('ascii')
        self.stream.sendall(b'$' + payload + b'#' + f'{sum(payload) & 255:02x}'.encode())
        while True:
            token = self.byte()
            require(token != b'-', 'QEMU rejected the GDB request checksum')
            if token != b'$':
                continue  # acknowledgements, or the stop response after continue
            data = bytearray()
            while True:
                token = self.byte()
                if token == b'#':
                    break
                data.extend(token)
            checksum = int(self.byte() + self.byte(), 16)
            require(sum(data) & 255 == checksum, 'Invalid QEMU GDB response checksum')
            self.stream.sendall(b'+')
            # The checksum covers the encoded packet; decode escaping and GDB
            # run-length encoding only after verifying it (registers use RLE).
            decoded = bytearray()
            index = 0
            while index < len(data):
                value = data[index]
                if value in (ord('}'), ord('*')):
                    index += 1
                    require(index < len(data), 'Truncated QEMU GDB packet encoding')
                    if value == ord('}'):
                        decoded.append(data[index] ^ 0x20)
                    else:
                        require(bool(decoded) and data[index] >= 29, 'Invalid GDB run length')
                        decoded.extend(bytes([decoded[-1]]) * (data[index] - 29))
                else:
                    decoded.append(value)
                index += 1
            text = decoded.decode('ascii')
            # Console-output packets can precede a stop response.
            if text.startswith('O') and text != 'OK':
                continue
            return text

    def register_packet(self):
        response = self.request('g')
        require(len(response) >= 128 and re.fullmatch(r'[0-9a-fA-F]+', response),
                'Unexpected ARM GDB register layout')
        return response

    @staticmethod
    def core_registers(response):
        return [int.from_bytes(bytes.fromhex(response[i * 8:(i + 1) * 8]), 'little')
                for i in range(16)]

    def registers(self):
        return self.core_registers(self.register_packet())


def select_cmu_machine(socket_path, process, deadline, record):
    stream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        while True:
            require(process.poll() is None, 'QEMU exited before the CMU entry breakpoint')
            require(time.monotonic() < deadline, 'Timed out connecting to QEMU GDB')
            try:
                stream.connect(str(socket_path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.05)
        remote = RemoteGDB(stream, deadline)
        require(remote.request('?').startswith(('S', 'T')), 'QEMU is not initially stopped')
        breakpoint = f'1,{CMU_ENTRY:x},4'  # hardware breakpoint: do not patch kernel bytes
        require(remote.request('Z' + breakpoint) == 'OK', 'QEMU rejected hardware breakpoint')
        require(remote.request('c').startswith(('S', 'T')), 'CMU entry breakpoint was not reached')
        before_packet = remote.register_packet()
        before = remote.core_registers(before_packet)
        require(before[15] == CMU_ENTRY, 'QEMU stopped outside the original zImage entry')
        record['before'] = before[1]
        value = CMU_MACHINE_ID.to_bytes(4, 'little').hex()
        # QEMU may reject P writes before XML target-feature negotiation. Keep
        # the complete g packet byte-for-byte and change only r1's four bytes,
        # then verify the entire packet after G (including non-core registers).
        changed_packet = before_packet[:8] + value + before_packet[16:]
        record['register_write_method'] = 'GDB G packet with only r1 bytes changed'
        require(remote.request('G' + changed_packet) == 'OK', 'QEMU rejected the r1 register update')
        record['r1_write_acknowledged'] = True
        after_packet = remote.register_packet()
        require(after_packet.lower() == changed_packet.lower(),
                'QEMU register packet changed beyond the requested r1 bytes')
        after = remote.core_registers(after_packet)
        record['after'] = after[1]
        require(after[1] == CMU_MACHINE_ID and all(before[i] == after[i] for i in range(16) if i != 1),
                'Unexpected core register change at CMU entry')
        require(remote.request('z' + breakpoint) == 'OK', 'Cannot remove CMU entry breakpoint')
        require(remote.request('D') == 'OK', 'Cannot detach and resume QEMU')
        record.update(completed=True, other_r0_to_r15_unchanged=True,
                      all_other_register_packet_bytes_unchanged=True)
    finally:
        stream.close()


def console_markers(path):
    markers = []
    with path.open(encoding='utf-8', errors='replace') as stream:
        for number, line in enumerate(stream, 1):
            line = line.rstrip('\r\n')
            if line.startswith(('VM_', 'GUARD_SELECT_', 'BUS_PIDS=', 'SM_PID=',
                                'VBS_LAUNCHER_PID=', 'AA_LAUNCHER_PID=', 'INIT_PROBE_PID=',
                                'TRACE_WRAPPER_RC=')):
                markers.append({'line': number, 'text': line})
    return markers


def initrd_provenance(initrd, actual_digest):
    sidecar = Path(str(initrd) + '.json')
    if not sidecar.is_file():
        return None
    with sidecar.open(encoding='utf-8') as stream:
        value = json.load(stream)
    if not isinstance(value, dict) or value.get('operation') != 'build':
        return None
    require(value.get('initrd_sha256') == actual_digest,
            'Initramfs does not match its build provenance')
    require(isinstance(value.get('inputs'), dict)
            and value['inputs'].get('diagnostic_init_sha256')
            and value['inputs'].get('oem_init_sha256'), 'Incomplete initramfs build provenance')
    # Copy identities only, never arbitrary paths or account data from a sidecar.
    return {'sidecar_sha256': digest(sidecar),
            'diagnostic_init_sha256': value['inputs']['diagnostic_init_sha256'],
            'oem_init_sha256': value['inputs']['oem_init_sha256']}


def run(args):
    kernel = regular_input(args.kernel, 'Kernel')
    initrd = regular_input(args.initrd, 'Initramfs')
    require(shutil.which('qemu-system-arm'), 'qemu-system-arm is required on PATH')
    prefix = args.output.absolute()
    log_path = Path(str(prefix) + '.log')
    metadata_path = Path(str(prefix) + '.json')
    unused_outputs(log_path, metadata_path)
    version = subprocess.check_output(['qemu-system-arm', '--version'], text=True).splitlines()[0]
    metadata = {
        'schema': 1, 'operation': 'run', 'publishable': False,
        'scope': 'OEM userspace on an isolated emulated board; no physical CMU devices',
        'verdict': 'observation_only', 'timeout_is_success': False,
        'clean_qemu_exit_is_success': False, 'qemu_version': version,
        'board': args.board, 'mode': args.mode, 'phase': args.phase,
        'original_init_requested': args.original_init,
        'mode_phase_arguments_passed': not args.original_init,
        'original_init_execution_verified': False,
        'extra_kernel_arguments': args.kernel_arg,
        'seconds_limit': args.seconds, 'timed_out': False, 'exit_code': None,
        'host_network': False, 'host_device_passthrough': False, 'host_shared_directories': False,
        'inputs': {'kernel_sha256': digest(kernel), 'initrd_sha256': digest(initrd)},
        'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'machine_id_adjustment': None,
    }
    provenance = initrd_provenance(initrd, metadata['inputs']['initrd_sha256'])
    metadata['initrd_build_provenance'] = provenance
    # A built image always contains our diagnostic /init. Omitting rdinit for
    # that image would silently run diagnostics when OEM PID 1 was requested.
    # External stock CMU initramfs images retain their kernel-default /init;
    # their contents and actual PID 1 execution are not inferred from the flag.
    if args.original_init:
        selected_init = '/sbin/init' if provenance or args.board == 'virt' else None
        metadata['init_selection'] = {
            'rdinit': selected_init,
            'expected_path': selected_init or '/init (kernel default)',
            'origin': 'OEM init in verified build' if provenance else 'external image, unverified',
            'sha256': provenance['oem_init_sha256'] if provenance else None,
        }
    else:
        selected_init = '/init'
        metadata['init_selection'] = {
            'rdinit': selected_init, 'expected_path': selected_init,
            'origin': 'diagnostic init in verified build' if provenance else 'external image, unverified',
            'sha256': provenance['diagnostic_init_sha256'] if provenance else None,
        }
    process = None
    error = None
    # Temporary command arguments conceal private input paths in provenance.
    # Only copied kernel/initrd files are provided; no filesystem is shared.
    with tempfile.TemporaryDirectory(prefix='mx5dr-oem-run-', dir='/tmp') as tmp:
        work = Path(tmp)
        for original, name in ((kernel, 'kernel'), (initrd, 'initrd')):
            shutil.copyfile(original, work / name)
            require(digest(work / name) == metadata['inputs'][name + '_sha256'],
                    'An input changed while being staged')
        command = ['qemu-system-arm', '-machine', 'virt' if args.board == 'virt' else 'sabrelite',
                   '-smp', '2', '-m', '1536' if args.board == 'virt' else '1024',
                   '-display', 'none', '-monitor', 'none', '-nic', 'none', '-no-reboot']
        if args.board == 'virt':
            command += ['-cpu', 'cortex-a15', '-serial', 'stdio']
            cmdline = 'console=ttyAMA0 loglevel=7 panic=0'
        else:
            command += ['-serial', 'null', '-serial', 'stdio', '-S',
                        '-gdb', 'unix:' + str(work / 'gdb.sock') + ',server=on,wait=off']
            cmdline = ('console=ttymxc1,115200 lpj=8495104 ldb=sin0 loglevel=8 initcall_debug '
                       'usbcore.authorized_default=0 panic=0 mxc_vpu_mempool.max_instances=3')
        if selected_init:
            cmdline += ' rdinit=' + selected_init
        if not args.original_init:
            cmdline += f' mx5mode={args.mode} mx5phase={args.phase}'
        if args.kernel_arg:
            # Explicit diagnostic overrides follow the defaults. Pass the
            # complete string directly to QEMU; never evaluate it in a shell.
            cmdline += ' ' + ' '.join(args.kernel_arg)
        command += ['-kernel', str(work / 'kernel'), '-initrd', str(work / 'initrd'), '-append', cmdline]
        metadata['command'] = command
        start = time.monotonic()
        deadline = start + args.seconds
        try:
            with log_path.open('xb') as log:
                process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log,
                                           stderr=subprocess.STDOUT, start_new_session=True)
                if args.board == 'cmu':
                    metadata['machine_id_adjustment'] = {
                        'entry_pc': f'0x{CMU_ENTRY:08x}', 'breakpoint': 'GDB hardware breakpoint',
                        'register': 'r1', 'requested_value': CMU_MACHINE_ID,
                        'kernel_bytes_modified': False, 'completed': False,
                    }
                    select_cmu_machine(work / 'gdb.sock', process, min(deadline, start + 30),
                                       metadata['machine_id_adjustment'])
                try:
                    process.wait(timeout=max(0.001, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    metadata['timed_out'] = True
        except (OSError, RuntimeError, ValueError) as exc:
            # Do not embed an exception's private input pathname in metadata.
            error = exc.__class__.__name__
            metadata['harness_error'] = {'type': error}
            if isinstance(exc, RuntimeError):
                metadata['harness_error']['message'] = str(exc)
        finally:
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                if process.stdin:
                    process.stdin.close()
                metadata['exit_code'] = process.returncode
            metadata['elapsed_seconds'] = round(time.monotonic() - start, 3)
            metadata['console_markers'] = console_markers(log_path) if log_path.exists() else []
            metadata['diagnostic_init_observed'] = any(
                row['text'].startswith('VM_SCOPE=') for row in metadata['console_markers'])
            metadata['unexpected_diagnostic_init'] = (
                args.original_init and metadata['diagnostic_init_observed'])
            metadata['console_sha256'] = digest(log_path) if log_path.exists() else None
            write_json(metadata_path, metadata)
    print('Recorded OEM emulation observation; this is not a PASS verdict.')
    print('timed_out=' + str(metadata['timed_out']).lower()
          + ' qemu_exit_code=' + str(metadata['exit_code']))
    return 2 if error else (124 if metadata['timed_out'] else (0 if metadata['exit_code'] == 0 else 1))


def positive_seconds(value):
    result = int(value)
    if result <= 0:
        raise argparse.ArgumentTypeError('seconds must be positive')
    return result


def single_kernel_argument(value):
    if not value or '\x00' in value or any(character.isspace() for character in value):
        raise argparse.ArgumentTypeError('kernel argument must be nonempty and contain no whitespace or NUL')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    build_parser = commands.add_parser('build', help='Build a private initramfs in a Linux UID 0 container')
    build_parser.add_argument('--rootfs-tar', type=Path, required=True)
    build_parser.add_argument('--bundle', type=Path, required=True, help='Extracted flat USB bundle directory')
    build_parser.add_argument('--output', type=Path, required=True, help='New .cpio.gz file; adds .json sidecar')
    build_parser.add_argument('--touch', type=Path, help='Optional private existing AA touch ARM library')
    build_parser.set_defaults(action=build)
    run_parser = commands.add_parser('run', help='Run isolated QEMU and record console plus provenance')
    run_parser.add_argument('--kernel', type=Path, required=True)
    run_parser.add_argument('--initrd', type=Path, required=True)
    run_parser.add_argument('--output', type=Path, required=True, help='New log prefix; adds .log and .json')
    run_parser.add_argument('--board', choices=('virt', 'cmu'), required=True)
    run_parser.add_argument('--mode', choices=('baseline', 'shadow'), default='baseline')
    run_parser.add_argument('--phase', choices=('services', 'standalone', 'initprobe'), default='services')
    run_parser.add_argument('--seconds', type=positive_seconds, default=120)
    run_parser.add_argument('--kernel-arg', type=single_kernel_argument, action='append', default=[],
                            help='Append one explicit kernel argument after defaults; repeat as needed')
    run_parser.add_argument('--original-init', action='store_true',
                            help='Request OEM PID 1: built image uses /sbin/init; external CMU uses kernel default')
    run_parser.set_defaults(action=run)
    args = parser.parse_args()
    try:
        return args.action(args)
    except subprocess.CalledProcessError as exc:
        print(f'oem-system-emulation: external command failed (exit {exc.returncode})', file=sys.stderr)
    except (OSError, RuntimeError, ValueError) as exc:
        # Custom RuntimeErrors contain no input paths; OS exceptions can do so.
        message = str(exc) if isinstance(exc, RuntimeError) else exc.__class__.__name__
        print('oem-system-emulation: ' + message, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())
