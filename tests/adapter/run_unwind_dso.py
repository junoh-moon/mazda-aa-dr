#!/usr/bin/env python3
"""Exercise authored exceptions across the unmodified production ARM DSO.

No OEM code is loaded. The caller must verify the build/toolchain inputs (as
run_arm_all.sh does). This additionally checks that archive exception/runtime
symbols are hidden so LD_PRELOAD does not interpose them into the OEM process.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--cross-prefix', required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--suite', choices=('position', 'request'), default='position')
    args = parser.parse_args()
    library = args.library.resolve()
    build = args.output_dir.resolve()
    build.mkdir(parents=True, exist_ok=True)
    # A failed rerun must not leave a previous success record at this path.
    (build / 'unwind-dso.json').unlink(missing_ok=True)
    repo = Path(__file__).resolve().parents[2]
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    library_hash = digest(library)
    nm = args.cross_prefix + 'nm'
    symbols = subprocess.check_output([nm, '--defined-only', str(library)], text=True)
    names = {
        'CONFIGURE': '_ZN3mx57adapter9configureEPFiPvPNS0_11VehicleDataEERKNS0_7OptionsE',
        'MODE': '_ZN3mx57adapter8set_modeENS0_4ModeE',
        # Use the exact C GOT entry too, not only the adapter's C++ helper.
        'SEND': 'mx5_send_vehicle_data',
        'POSITION': 'mx5_position_veneer', 'TRAMPOLINE': 'mx5_position_trampoline',
    }
    cases = ('throw_position', 'throw_send', 'nested_throw', 'cancel_position', 'cancel_send',
             'throw_enter', 'cancel_enter', 'nested_send')
    fixture = 'veneer_unwind'
    access = 'unwind_dso_access.h'
    macro = '-DMX5_UNWIND_DSO_TEST'
    marker = 'PASS ARM unwind '
    if args.suite == 'request':
        names = {
            'PREPARE': '_ZN3mx57adapter21prepare_request_hooksERKNS0_15RequestBindingsEPFyPvES4_',
            'HEALTH': '_ZN3mx57adapter19request_hook_healthEv',
            'READ': '_ZN3mx57adapter18read_request_traceEPKvPNS_7runtime13request_trace5TraceEPv',
            'SUBMIT': 'mx5_request_submit', 'FREE': 'mx5_request_free', 'FREE_ONLY': 'mx5_request_free_only',
            'POST': 'mx5_request_post_veneer', 'WORK': 'mx5_request_work_veneer',
            'DESTROY': 'mx5_request_destroy_veneer', 'POST_ENTER': 'mx5_request_post_enter',
        }
        cases = ('normal', 'unrelated', 'failed_submit', 'destroy_queued', 'throw_notify', 'throw_work',
                 'cancel_notify', 'cancel_work', 'malformed', 'other_worker', 'delayed_callback',
                 'throw_getter', 'cancel_getter')
        fixture, access = 'request_hooks_arm', 'request_dso_access.h'
        macro, marker = '-DMX5_REQUEST_DSO_TEST', 'PASS ARM request wrappers '
    offsets = {}
    for label, name in names.items():
        rows = [line.split() for line in symbols.splitlines() if line.split()[-1] == name]
        require(len(rows) == 1 and rows[0][1] in ('t', 'T', 'b', 'B'), name)
        offsets[label] = int(rows[0][0], 16)
    exports = subprocess.check_output([nm, '-D', '--defined-only', str(library)], text=True)
    exported = [line.split()[-1] for line in exports.splitlines()]
    require('dlopen' in exported, 'Missing product dlopen entry')
    require(not any(name.startswith(('_Z', '__cxa', '__gxx', '_Unwind', '__gnu_', '__aeabi_'))
                    for name in exported), 'Archive runtime symbol leaked into preload exports')
    (build / 'unwind_offsets.h').write_text(''.join(
        '#define TEST_' + label + ' 0x%xu\n' % offset for label, offset in offsets.items()))
    sources = [repo / 'tests/adapter' / name for name in
               (fixture + '_test.cpp', access, fixture + '_fixture.S', 'run_unwind_dso.py')]
    sources += sorted(p for p in (repo / 'src').rglob('*') if p.is_file())
    inputs = {str(p.relative_to(repo)): digest(p) for p in sources}
    for source in sources:
        copied = build / 'source' / source.relative_to(repo)
        copied.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, copied)
        require(digest(copied) == inputs[str(source.relative_to(repo))], 'Input changed during snapshot')
    executable = build / 'unwind-dso-test'
    command = [args.cross_prefix + 'g++', '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-D_GNU_SOURCE', macro, '-mcpu=cortex-a9', '-mfpu=neon',
               '-mfloat-abi=softfp', '-marm', '-I' + str(build / 'source/src'), '-I' + str(build),
               str(build / 'source/tests/adapter' / (fixture + '_test.cpp')),
               str(build / 'source/tests/adapter' / (fixture + '_fixture.S')),
               '-ldl', '-pthread', '-o', str(executable)]
    subprocess.run(command, check=True)
    environment = dict(os.environ)
    for name in ('LD_PRELOAD', 'LD_LIBRARY_PATH', 'LD_AUDIT', 'QEMU_LD_PREFIX',
                 'QEMU_SET_ENV', 'QEMU_UNSET_ENV'):
        environment.pop(name, None)
    results = []
    for case in cases:
        run = ['qemu-arm', '-L', str(args.sysroot), '-E', 'LD_PRELOAD=' + str(library),
               '-E', 'MX5_UNWIND_LIBRARY=' + str(library), str(executable), case]
        result = subprocess.run(run, env=environment, capture_output=True, text=True, timeout=20)
        (build / (case + '.log')).write_text(result.stdout + result.stderr)
        require(result.returncode == 0 and (marker + case + ':') in result.stdout,
                'Failed case ' + case + ': ' + repr(result))
        results.append(dict(case=case, exit=result.returncode, command=run))
    require(digest(library) == library_hash, 'Production DSO changed during test')
    require({str(p.relative_to(repo)): digest(p) for p in sources} == inputs,
            'Source changed during test; completed results do not describe current source')
    record = dict(library_sha256=library_hash, executable_sha256=digest(executable),
                  source_sha256=inputs, command=command,
                  offsets=offsets, exported=exported, cases=results, oem_executed=False)
    (build / 'unwind-dso.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS production DSO: %s suite, %d cases; archive runtime exports hidden' % (args.suite, len(cases)))


if __name__ == '__main__':
    main()
