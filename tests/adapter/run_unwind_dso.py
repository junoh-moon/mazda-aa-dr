#!/usr/bin/env python3
"""Exercise authored calls across the unmodified production ARM DSO.

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
    parser.add_argument('--suite', choices=('position', 'request', 'request-wire', 'session', 'bus', 'assist', 'runtime-assist'), default='position')
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
            'SESSION_PREPARE': '_ZN3mx57adapter21prepare_session_hooksERKNS0_15SessionBindingsE',
            'SESSION_ISSUE': '_ZN3mx57adapter18read_issue_sessionEv',
            'SESSION_SEND': '_ZN3mx57adapter17read_send_sessionEPKvPNS_7runtime13session_trace8SnapshotEPv',
            'SESSION_HEALTH': '_ZN3mx57adapter19session_hook_healthEv',
            'SESSION_CREATE': 'mx5_session_create', 'SESSION_DESTROY': 'mx5_session_destroy',
        }
        cases = ('normal', 'unrelated', 'failed_submit', 'destroy_queued', 'throw_notify', 'throw_work',
                 'cancel_notify', 'cancel_work', 'malformed', 'other_worker', 'delayed_callback',
                 'throw_getter', 'cancel_getter', 'session_transition')
        fixture, access = 'request_hooks_arm', 'request_dso_access.h'
        macro, marker = '-DMX5_REQUEST_DSO_TEST', 'PASS ARM request wrappers '
    elif args.suite == 'request-wire':
        names = {
            'PREPARE': '_ZN3mx57adapter21prepare_request_hooksERKNS0_15RequestBindingsEPFyPvES4_',
            'HEALTH': '_ZN3mx57adapter19request_hook_healthEv',
            'READ': '_ZN3mx57adapter18read_request_traceEPKvPNS_7runtime13request_trace5TraceEPv',
            'SUBMIT': 'mx5_request_submit', 'FREE': 'mx5_request_free', 'FREE_ONLY': 'mx5_request_free_only',
            'MESSAGE': 'mx5_request_message', 'WIRE_SEND': 'mx5_request_wire_send',
            'PENDING': 'mx5_request_pending', 'STEAL': 'mx5_request_steal',
            'POST_ENTER': 'mx5_request_post_enter', 'WORK_CALL': 'mx5_request_work_call',
        }
        cases = ('contract',)
        fixture, access = 'request_wire', 'request_wire_dso_access.h'
        macro, marker = '-DMX5_REQUEST_WIRE_DSO_TEST', 'PASS raw request wrappers '
    elif args.suite == 'session':
        names = {
            'PREPARE': '_ZN3mx57adapter21prepare_session_hooksERKNS0_15SessionBindingsE',
            'HEALTH': '_ZN3mx57adapter19session_hook_healthEv',
            'ISSUE': '_ZN3mx57adapter18read_issue_sessionEv',
            'SEND': '_ZN3mx57adapter17read_send_sessionEPKvPNS_7runtime13session_trace8SnapshotEPv',
            'CREATE': 'mx5_session_create', 'DESTROY': 'mx5_session_destroy',
            'CONFIGURE': '_ZN3mx57adapter9configureEPFiPvPNS0_11VehicleDataEERKNS0_7OptionsE',
            'MODE': '_ZN3mx57adapter8set_modeENS0_4ModeE',
            'GENERATION': '_ZN3mx57adapter10generationEv',
            'PUBLISH': '_ZN3mx57adapter16publish_snapshotERKNS0_10DrSnapshotE',
            'POSITION_ENTER': 'mx5_position_enter', 'POSITION_LEAVE': 'mx5_position_leave',
            'VEHICLE_SEND': 'mx5_send_vehicle_data',
        }
        cases = ('normal', 'failure', 'overlap', 'same_storage', 'closing_create', 'creating_during_destroy', 'null_success', 'output_race', 'late_destroy', 'distinct_storage', 'capacity',
                 'callback_bad', 'callback_null', 'readers', 'throw_create', 'throw_destroy', 'throw_status',
                 'cancel_create', 'cancel_destroy', 'cancel_status',
                 'prediction_destroy', 'prediction_recreate', 'prediction_create_failure',
                 'prediction_destroy_failure', 'prediction_status', 'prediction_create_inflight',
                 'prediction_destroy_inflight', 'prediction_status_inflight', 'prediction_cached_inflight')
        fixture, access = 'session_hooks', 'session_dso_access.h'
        macro, marker = '-DMX5_SESSION_DSO_TEST', 'PASS session wrappers '
    elif args.suite == 'bus':
        names = {
            'PREPARE': '_ZN3mx57adapter17prepare_bus_hooksERKNS0_11BusBindingsE',
            'HEALTH': '_ZN3mx57adapter15bus_hook_healthEv',
            'READ': '_ZN3mx57adapter19read_bus_connectionEPKv',
            'OBSERVE_POSITION': '_ZN3mx57adapter20observe_position_busEPKv',
            'READ_POSITION': '_ZN3mx57adapter17read_position_busEv',
            'CREATE': 'mx5_bus_create', 'CONNECT': 'mx5_bus_connect',
            'DISCONNECT': 'mx5_bus_disconnect', 'FREE': 'mx5_bus_free', 'SIGNAL': 'mx5_bus_signal',
            'CONFIGURE': '_ZN3mx57adapter9configureEPFiPvPNS0_11VehicleDataEERKNS0_7OptionsE',
            'MODE': '_ZN3mx57adapter8set_modeENS0_4ModeE',
            'GENERATION': '_ZN3mx57adapter10generationEv',
            'PUBLISH': '_ZN3mx57adapter16publish_snapshotERKNS0_10DrSnapshotE',
            'POSITION_ENTER': 'mx5_position_enter', 'POSITION_LEAVE': 'mx5_position_leave',
            'VEHICLE_SEND': 'mx5_send_vehicle_data',
        }
        cases = ('normal', 'position_source', 'position_sources_concurrent', 'signal', 'signal_reuse', 'failure', 'early_close', 'unobserved', 'overlap', 'cancel', 'readers',
                 'capacity', 'collision', 'bad_callback', 'throw_create', 'throw_connect',
                 'throw_disconnect', 'throw_free', 'throw_closed',
                 'prediction_entry_create', 'prediction_entry_connect', 'prediction_entry_disconnect',
                 'prediction_entry_free', 'prediction_entry_closed', 'prediction_entry_signal',
                 'prediction_exit_create', 'prediction_exit_connect', 'prediction_exit_disconnect',
                 'prediction_exit_free', 'prediction_exit_closed', 'prediction_exit_signal')
        fixture, access = 'bus_hooks', 'bus_dso_access.h'
        macro, marker = '-DMX5_BUS_DSO_TEST', 'PASS bus connection '
    elif args.suite == 'assist':
        names = {
            'CONFIGURE': '_ZN3mx57adapter9configureEPFiPvPNS0_11VehicleDataEERKNS0_7OptionsE',
            'MODE': '_ZN3mx57adapter8set_modeENS0_4ModeE',
            'INVALIDATE': '_ZN3mx57adapter10invalidateEv',
            'GENERATION': '_ZN3mx57adapter10generationEv',
            'PUBLISH': '_ZN3mx57adapter16publish_snapshotERKNS0_10DrSnapshotE',
            'POSITION_ENTER': 'mx5_position_enter', 'POSITION_LEAVE': 'mx5_position_leave',
            'VEHICLE_SEND': 'mx5_send_vehicle_data', 'DEFAULT_CONFIG': 'mx5_dr_default_config',
            'PIPELINE_CONSTRUCT': '_ZN3mx510navigation8PipelineC1Ev',
            'PIPELINE_DESTRUCT': '_ZN3mx510navigation8PipelineD1Ev',
            'PIPELINE_INIT': '_ZN3mx510navigation8Pipeline14init_qualifiedERK13mx5_dr_config14mx5_dr_context',
            'PIPELINE_MODEL_INIT': '_ZN3mx510navigation8Pipeline10init_modelERKNS0_12ModelProfileERK13mx5_dr_config14mx5_dr_contextbb',
            'PIPELINE_BIND': '_ZN3mx510navigation8Pipeline22bind_qualified_revokerEPFyPvES2_',
            'PIPELINE_ANCHOR': '_ZN3mx510navigation8Pipeline14enqueue_anchorERK13mx5_dr_anchory',
            'PIPELINE_POSITION': '_ZN3mx510navigation8Pipeline16enqueue_positionERKNS_7adapter11ObservationE',
            'PIPELINE_SPEED': '_ZN3mx510navigation8Pipeline13enqueue_speedERK15mx5_dr_evidenced',
            'PIPELINE_REVERSE': '_ZN3mx510navigation8Pipeline15enqueue_reverseERK15mx5_dr_evidencei',
            'PIPELINE_YAW': '_ZN3mx510navigation8Pipeline11enqueue_yawERK15mx5_dr_evidencedttyy',
            'PIPELINE_DRAIN': '_ZN3mx510navigation8Pipeline5drainEy',
            'PIPELINE_DIAGNOSTIC': '_ZNK3mx510navigation8Pipeline10diagnosticEy',
            'PIPELINE_PUBLICATION': '_ZNK3mx510navigation8Pipeline21qualified_publicationEyRKNS_7runtime23CoreBridgeQualificationEyPNS_7adapter10DrSnapshotE',
        }
        cases = ('straight', 'quality_gap', 'quality_cycle', 'turn', 'reverse', 'expiry', 'reacquire',
                 'native_return', 'stale_control', 'fault_recovery', 'core_reject',
                 'reinit', 'failed_reinit', 'failed_model_reinit', 'owner_exit', 'unverified', 'continuous_reacquire',
                 'anchor_first_reacquire', 'separate_reacquire', 'native_reacquire', 'quality_reacquire',
                 'single_gap_reacquire', 'ready_quality_reanchor')
        fixture, access = 'assist_publication', 'assist_dso_access.h'
        macro, marker = '-DMX5_ASSIST_DSO_TEST', 'PASS assist publication '
    elif args.suite == 'runtime-assist':
        names = {
            'CONFIGURE': '_ZN3mx57adapter9configureEPFiPvPNS0_11VehicleDataEERKNS0_7OptionsE',
            'MODE': '_ZN3mx57adapter8set_modeENS0_4ModeE',
            'GENERATION': '_ZN3mx57adapter10generationEv',
            'POSITION_ENTER': 'mx5_position_enter', 'POSITION_LEAVE': 'mx5_position_leave',
            'VEHICLE_SEND': 'mx5_send_vehicle_data', 'DEFAULT_CONFIG': 'mx5_dr_default_config',
            'ASSIST_CONSTRUCT': '_ZN3mx57runtime12AssistWorkerC1ERK13mx5_dr_configRKNS0_12AssistSourceE',
            'ASSIST_DESTRUCT': '_ZN3mx57runtime12AssistWorkerD1Ev',
            'RUN_WORKER': '_ZN3mx57runtime10run_workerEPKcS2_PNS0_12AssistWorkerE',
            'RUNTIME_CONFIG': '_ZN12_GLOBAL__N_16configE',
            'HOOK_INSTALLED': '_ZN12_GLOBAL__N_114hook_installedE',
            'RUNTIME_SINK': '_ZN12_GLOBAL__N_14sinkEPKN3mx57adapter11ObservationEPv',
            'AUDIT_FAILURE': '_ZN12_GLOBAL__N_116disable_mutationEv',
        }
        cases = ('publication', 'source_fault', 'unqualified', 'recovery', 'audit',
                 'journal_failure', 'pre_stopped', 'unhooked', 'shadow')
        fixture, access = 'runtime_assist', 'runtime_assist_dso_access.h'
        macro, marker = '-DMX5_RUNTIME_ASSIST_DSO_TEST', 'PASS runtime assist '
    offsets = {}
    for label, name in names.items():
        rows = [line.split() for line in symbols.splitlines() if line.split()[-1] == name]
        require(len(rows) == 1 and rows[0][1] in ('t', 'T', 'b', 'B', 'd', 'D'), name)
        offsets[label] = int(rows[0][0], 16)
    exports = subprocess.check_output([nm, '-D', '--defined-only', str(library)], text=True)
    exported = [line.split()[-1] for line in exports.splitlines()]
    require('dlopen' in exported, 'Missing product dlopen entry')
    require(not any(name.startswith(('_Z', '__cxa', '__gxx', '_Unwind', '__gnu_', '__aeabi_'))
                    for name in exported), 'Archive runtime symbol leaked into preload exports')
    (build / 'unwind_offsets.h').write_text(''.join(
        '#define TEST_' + label + ' 0x%xu\n' % offset for label, offset in offsets.items()))
    names = [fixture + '_test.cpp', access, 'run_unwind_dso.py']
    if args.suite not in ('request-wire', 'session', 'bus', 'assist', 'runtime-assist'):
        names.append(fixture + '_fixture.S')
    sources = [repo / 'tests/adapter' / name for name in names]
    for root, directories, files in os.walk(repo / 'src'):
        depth = len(Path(root).relative_to(repo / 'src').parts)
        require(depth < 16 or not directories, 'Source snapshot depth limit exceeded')
        sources.extend(sorted(Path(root) / name for name in files))
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
               str(build / 'source/tests/adapter' / (fixture + '_test.cpp'))]
    if args.suite not in ('request-wire', 'session', 'bus', 'assist', 'runtime-assist'):
        command.append(str(build / 'source/tests/adapter' / (fixture + '_fixture.S')))
    if args.suite == 'runtime-assist':
        # The pinned glibc provides the real monotonic fixture clock in librt.
        command.append('-lrt')
    command += ['-ldl', '-pthread', '-o', str(executable)]
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
