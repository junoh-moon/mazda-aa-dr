"""Read-only parked check with synthetic bounded journals; no firmware needed."""
import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

PACK = Path(__file__).resolve().parents[2] / 'packaging'
COLLECTOR = Path(os.environ.get('MX5DR_TEST_BUILD', PACK.parent / 'build')) / 'test_collector'
BOOT = '12345678-1234-1234-1234-123456789abc'
OLD = '87654321-1234-1234-1234-123456789abc'
CONFIG = 'mode=SHADOW\nsample_ms=1000\n'
CONFIG_DIGEST = hashlib.sha256(CONFIG.encode()).hexdigest()
MANIFEST = 'mx5dr-one-boot-v3\n' + ('a' * 64 + '\n') + CONFIG_DIGEST + '\n' + ('a' * 64 + '\n') * 6
LEGACY_MANIFEST = 'mx5dr-one-boot-v2\n' + ('a' * 64 + '\n') * 7


class TrialStatusTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / '.mx5dr-fixture').touch()
        self.base = self.root / 'data_persist/mx5-aa-dr'
        self.logs = self.base / 'logs'
        self.logs.mkdir(parents=True)
        (self.base / 'guard').mkdir()
        (self.base / 'guard/last-boot').write_text(BOOT + '\n')
        (self.base / 'guard/consumed').write_text(MANIFEST)
        (self.base / 'guard/armed-boot').write_text(OLD + '\n')
        (self.base / 'mx5dr.conf').write_text(CONFIG)
        bootfile = self.root / 'proc/sys/kernel/random/boot_id'
        bootfile.parent.mkdir(parents=True)
        bootfile.write_text(BOOT + '\n')
        (self.root / 'proc/uptime').write_text('100.00 1.00\n')
        self.trace = [dict(kind='boot', boot_id=BOOT, mono_ns=1000000000, mode=4, install='ok'),
                      dict(kind='shadow_boot', active=True, capture_active=True),
                      dict(kind='health', mono_ns=99000000000, hook_installed=True,
                           audit_fault=0, dropped=0, capture_active=True, computation_active=True),
                      dict(kind='position', mono_ns=99000000000, mode=1),
                      dict(kind='shadow_calibration', mono_ns=99000000000,
                           gps_anchor_gate='WAITING'),
                      dict(kind='shadow', mono_ns=99000000000, domain='model',
                           assist_ready=False, model_valid=False, events=4, intervals=0,
                           result='E_NO_SEED', pipeline='WAITING'),
                      dict(kind='motion_batch', schema=1, epoch=1, events=[
                          [sensor, sensor, 99000000000, 90000, 0, 0, 0, 0, 1, 0]
                          for sensor in (1, 2, 3)])]
        self.collector = [dict(kind='collector_boot', boot_id=BOOT, schema=1),
                          dict(kind='poll', end_ns=99000000000, seq=0)]

    def write(self, name, rows):
        if name.startswith('collector.'):
            # Exact envelope order emitted by collector.cpp Journal::line.
            rows = [dict(stream='collector', collector_pid=123, observed_at_mono_ns=99000000000,
                         producer_mono_ns=None, producer_time_status='unknown', **row)
                    for row in rows]
        (self.logs / name).write_text(''.join(json.dumps(row, separators=(',', ':')) + '\n'
                                               for row in rows))

    def run_status(self, trace=None, collector=None):
        self.write('trace.0.jsonl', self.trace if trace is None else trace)
        self.write('collector.0.jsonl', self.collector if collector is None else collector)
        before = {str(p.relative_to(self.root)): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        result = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                                env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        after = {str(p.relative_to(self.root)): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        self.assertEqual(before, after)
        return result

    def add_service(self, pid, name, maps, stack='131072', uid=0, comm=None):
        # Default is the form observed on the real CMU (2026-10-04): comm L_<svc> and
        # argv "/jci/sm/sm_svclauncher -l <svc> <plugin> 0 -a". comm='sm_svclauncher'
        # keeps the earlier assumed form working; any other comm is a non-launcher.
        comm = comm or 'L_' + name
        proc = self.root / 'proc' / str(pid)
        proc.mkdir(parents=True)
        (proc / 'comm').write_text(comm + '\n')
        if comm == 'sm_svclauncher':
            args = ['/jci/sm/sm_svclauncher', '-s', name, '/jci/x/blm.so']
        elif comm == 'L_' + name:
            args = ['/jci/sm/sm_svclauncher', '-l', name, '/jci/x/blm.so', '0', '-a']
        else:
            args = [comm]
        (proc / 'cmdline').write_bytes(b'\0'.join(a.encode() for a in args) + b'\0')
        (proc / 'status').write_text('Name:\t%s\nUid:\t%d\t%d\t%d\t%d\n' % (comm, uid, uid, uid, uid))
        (proc / 'limits').write_text('Limit Soft Limit Hard Limit Units\n'
                                     'Max stack size %s unlimited bytes\n' % stack)
        (proc / 'maps').write_text(maps)

    def test_service_lines_report_stack_limit_uid_and_preload(self):
        self.add_service(201, 'jciAAPA', '40000000-40100000 r-xp 0 00:00 0 /data_persist/mx5-aa-dr/libmx5dr.so\n')
        self.add_service(202, 'jciVBS', '40000000-40100000 r-xp 0 00:00 0 /jci/vbs/svcjcivbs.so\n', uid=1001)
        self.add_service(203, 'jciAudio', '/data_persist/mx5-aa-dr/libmx5dr.so\n')  # another service is ignored
        self.write('trace.0.jsonl', self.trace)
        result = self.run_status()
        out = result.stdout
        self.assertIn('service_jciAAPA=running pid=201 uid=0 stack_soft_bytes=131072 package_preload=yes', out)
        self.assertIn('service_jciVBS=running pid=202 uid=1001 stack_soft_bytes=131072 package_preload=no', out)
        self.assertIn('service_jciLDS=not_running', out)
        self.assertNotIn('jciAudio', out)

    def test_service_lines_accept_the_real_cmu_process_form_and_the_earlier_form(self):
        self.add_service(211, 'jciVBS', '/data_persist/mx5-aa-dr/libmx5dr-vimtap.so\n')
        self.add_service(212, 'jciLDS', '/data_persist/mx5-aa-dr/libmx5dr-ldstap.so\n', comm='sm_svclauncher')
        self.write('trace.0.jsonl', self.trace)
        out = self.run_status().stdout
        self.assertIn('service_jciVBS=running pid=211 uid=0 stack_soft_bytes=131072 package_preload=yes', out)
        self.assertIn('service_jciLDS=running pid=212 uid=0 stack_soft_bytes=131072 package_preload=yes', out)

    def test_service_lines_never_trust_a_non_launcher_process(self):
        self.add_service(204, 'jciAAPA', '/data_persist/mx5-aa-dr/libmx5dr.so\n', comm='jciAAPA')
        self.write('trace.0.jsonl', self.trace)
        self.assertIn('service_jciAAPA=not_running', self.run_status().stdout)

    def test_current_collection_read_only(self):
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('does not approve driving or ASSIST', r.stdout)
        self.assertIn('guard_last_boot=current guard_consumed=present', r.stdout)
        self.assertIn('reverse_received_recently=observed receipt_only_not_direction_quality', r.stdout)

    def test_failed_od_cannot_validate_boot_id(self):
        shim = self.root / 'shim'
        shim.mkdir()
        (shim / 'od').write_text('#!/bin/sh\nexit 1\n')
        (shim / 'od').chmod(0o755)
        result = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True,
                                text=True, env=dict(os.environ,
                                                    MX5DR_FIXTURE_ROOT=str(self.root),
                                                    PATH=str(shim) + ':' + os.environ['PATH']))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Invalid current boot ID', result.stderr)

    def test_od_error_after_complete_output_does_not_validate_marker_or_config(self):
        shim = self.root / 'shim'
        shim.mkdir()
        (shim / 'od').write_text('''#!/bin/sh
case " $* " in
  *"$MX5DR_OD_FAIL_PATH"*) "$MX5DR_REAL_OD" "$@"; exit 77 ;;
esac
exec "$MX5DR_REAL_OD" "$@"
''')
        (shim / 'od').chmod(0o755)
        base_env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root),
                        MX5DR_REAL_OD=shutil.which('od'),
                        PATH=str(shim) + ':' + os.environ['PATH'])
        for path, expected in (('boot_id', 'Invalid current boot ID'),
                               ('guard/consumed', 'guard_consumed=invalid'),
                               ('mx5dr.conf', 'config_mode=unconfirmed')):
            with self.subTest(path=path):
                result = subprocess.run(['sh', str(PACK / 'trial_status.sh')],
                                        capture_output=True, text=True,
                                        env=dict(base_env, MX5DR_OD_FAIL_PATH=path))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stdout + result.stderr)
                self.assertNotIn('startup_state=guard_committed_after_new_boot',
                                 result.stdout)

    def test_guard_markers_survive_empty_log_failure(self):
        (self.base / 'guard/armed-boot').unlink()
        r = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                           env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('one_boot=consumed_this_boot retained_bytes=0', r.stdout)
        self.assertIn('guard_last_boot=current guard_consumed=present', r.stdout)
        self.assertIn('guard_arm=absent guard_armed_boot=missing guard_previous_armed_boot=missing startup_state=guard_selected_reboot_unconfirmed', r.stdout)
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(OLD + '\n')
        r = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                           env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('one_boot=unconfirmed retained_bytes=0', r.stdout)
        self.assertIn('guard_last_boot=different guard_consumed=present', r.stdout)
        (self.base / 'guard/last-boot').unlink()
        (self.base / 'guard/consumed').unlink()
        r = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                           env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('guard_last_boot=missing guard_consumed=absent', r.stdout)

    def test_armed_boot_distinguishes_ignition_cycle_from_linux_reboot(self):
        (self.base / 'guard/arm').write_text(MANIFEST)
        (self.base / 'guard/armed-boot').write_text(BOOT + '\n')
        r = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True,
                           text=True, env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        self.assertNotEqual(r.returncode, 0)  # No logs are present.
        self.assertIn('one_boot=arm_present retained_bytes=0', r.stdout)
        self.assertIn('guard_armed_boot=current', r.stdout)
        self.assertIn('startup_state=awaiting_linux_reboot', r.stdout)
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(OLD + '\n')
        r = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True,
                           text=True, env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        self.assertIn('guard_armed_boot=different', r.stdout)
        self.assertIn('startup_state=new_linux_boot_arm_unconsumed', r.stdout)

    def test_consumed_guard_distinguishes_same_boot_selection_from_reboot(self):
        marker = self.base / 'guard/armed-boot'
        marker.write_text(BOOT + '\n')
        same = self.run_status()
        self.assertNotEqual(same.returncode, 0)
        self.assertIn('startup_state=guard_selected_same_boot_as_arm', same.stdout)
        self.assertIn('linux_reboot_after_arm=unavailable', same.stdout)
        marker.write_text(OLD + '\n')
        different = self.run_status()
        self.assertEqual(different.returncode, 0, different.stdout + different.stderr)
        self.assertIn('startup_state=guard_committed_after_new_boot', different.stdout)
        self.assertIn('linux_reboot_after_arm=observed', different.stdout)
        self.assertIn('guard_config_binding=matched', different.stdout)

    def test_changed_config_cannot_borrow_consumed_guard_selection(self):
        (self.base / 'mx5dr.conf').write_text('mode=SHADOW\nsample_ms=1500\n')
        changed = self.run_status()
        self.assertNotEqual(changed.returncode, 0)
        self.assertIn('config_mode=SHADOW', changed.stdout)
        self.assertIn('guard_config_binding=changed', changed.stdout)
        self.assertIn('startup_state=guard_config_changed_since_selection', changed.stdout)
        self.assertIn('linux_reboot_after_arm=unavailable', changed.stdout)

    def test_legacy_manifest_is_retained_but_cannot_qualify_v3_startup(self):
        (self.base / 'guard/consumed').write_text(LEGACY_MANIFEST)
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('guard_consumed=present', result.stdout)
        self.assertIn('guard_consumed_schema=v2', result.stdout)
        self.assertIn('one_boot=legacy_consumed_this_boot', result.stdout)
        self.assertIn('guard_current_boot=unavailable', result.stdout)
        self.assertIn('startup_state=legacy_guard_manifest', result.stdout)
        (self.base / 'guard/consumed').write_text(MANIFEST)
        (self.base / 'guard/arm').write_text(LEGACY_MANIFEST)
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('guard_arm_schema=v2', result.stdout)
        self.assertIn('startup_state=legacy_guard_manifest', result.stdout)

    def test_truncated_consumed_marker_cannot_claim_guard_selection(self):
        (self.base / 'guard/consumed').write_text('truncated\n')
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('guard_consumed=invalid', result.stdout)
        self.assertIn('one_boot=unconfirmed', result.stdout)

    def test_same_size_corrupt_consumed_marker_cannot_claim_guard_selection(self):
        (self.base / 'guard/consumed').write_bytes(b'x' * len(MANIFEST))
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('guard_consumed=invalid', result.stdout)
        self.assertIn('one_boot=unconfirmed', result.stdout)

    def test_nul_replaced_manifest_final_newline_cannot_claim_guard_selection(self):
        (self.base / 'guard/consumed').write_bytes(MANIFEST.encode()[:-1] + b'\x00')
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('guard_consumed=invalid', result.stdout)
        self.assertIn('one_boot=unconfirmed', result.stdout)

    def test_nul_inside_v3_manifest_cannot_claim_guard_selection(self):
        content = bytearray(MANIFEST.encode())
        content[25] = 0
        (self.base / 'guard/consumed').write_bytes(content)
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('guard_consumed=invalid', result.stdout)
        self.assertIn('one_boot=unconfirmed', result.stdout)

    def test_invalid_optional_guard_id_retains_current_boot_diagnostic(self):
        (self.base / 'guard/armed-boot').unlink()
        (self.base / 'guard/armed-boot').symlink_to('missing-target')
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('current_boot_id=' + BOOT, result.stdout)
        self.assertIn('guard_armed_boot=invalid', result.stdout)
        self.assertIn('startup_state=guard_selected_reboot_unconfirmed', result.stdout)

    def test_missing_final_newline_does_not_qualify_guard_boot_marker(self):
        for content in (OLD.encode(), OLD.encode() + b'\x00'):
            with self.subTest(content=content):
                (self.base / 'guard/armed-boot').write_bytes(content)
                result = self.run_status()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('guard_armed_boot=invalid', result.stdout)
                self.assertIn('startup_state=guard_selected_reboot_unconfirmed', result.stdout)

    def test_corrupt_previous_marker_cannot_borrow_old_positive_startup(self):
        (self.base / 'guard/armed-boot.previous').write_text('corrupt\n')
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('guard_previous_armed_boot=invalid', result.stdout)
        self.assertIn('startup_state=prior_arming_history_invalid', result.stdout)
        self.assertIn('one_boot=unconfirmed', result.stdout)

    def test_vim_side_channel_marker_and_state(self):
        result = self.run_status()
        self.assertIn('vim_side_channel_marker=absent', result.stdout)
        self.assertIn('vim_side_channel=unavailable', result.stdout)   # shadow_boot without the field
        (self.base / 'vimchan-off').write_text('off\n')
        self.trace[1] = dict(kind='shadow_boot', active=True, capture_active=True, vim_side_channel='disabled')
        result = self.run_status()
        self.assertIn('vim_side_channel_marker=present', result.stdout)
        self.assertIn('vim_side_channel=disabled', result.stdout)

    def test_runtime_disable_marker_blocks_old_boot_evidence(self):
        (self.logs / 'disable-next-start').write_text('restore_failed_fatal\n')
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('runtime_disable_next_start=present', result.stdout)
        self.assertIn('startup_state=runtime_disabled_next_start', result.stdout)
        self.assertIn('one_boot=unconfirmed', result.stdout)

    def test_off_mode_keeps_old_markers_without_claiming_current_start(self):
        (self.base / 'mx5dr.conf').write_text(' # trial disabled\n mode = OFF  # comment\n')
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('config_mode=OFF', result.stdout)
        self.assertIn('one_boot=unconfirmed', result.stdout)
        self.assertIn('startup_state=trial_disabled', result.stdout)
        self.assertIn('linux_reboot_after_arm=unavailable', result.stdout)

    def test_unavailable_or_invalid_config_cannot_borrow_old_success(self):
        config = self.base / 'mx5dr.conf'
        for data in (None, 'mode=SHADOW\nmode=OFF\n', 'mode=ASSIST\n',
                     'mode=SHADOW\nunknown=1\n', 'mode=SHADOW\x00\n'):
            if data is None:
                config.unlink(missing_ok=True)
            else:
                config.write_text(data)
            result = self.run_status()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('config_mode=unconfirmed', result.stdout)
            self.assertIn('startup_state=trial_config_unconfirmed', result.stdout)

    def test_capture_stop_request_cannot_claim_live_trial(self):
        (self.logs / 'capture.stop').mkdir()
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('startup_state=capture_stop_requested', result.stdout)
        self.assertIn('one_boot=unconfirmed', result.stdout)

    def test_boot_id_survives_invalid_uptime(self):
        (self.root / 'proc/uptime').write_text('unavailable\n')
        result = self.run_status()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('current_boot_id=' + BOOT, result.stdout)

    def test_usb_return_after_aa_disconnect_keeps_retained_and_current_separate(self):
        (self.root / 'proc/uptime').write_text('200.00 1.00\n')
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0)  # Recent collection is unavailable.
        self.assertIn('capture_active=unavailable', r.stdout)
        self.assertIn('retained_runtime_last_boot=current_boot boot_id=' + BOOT, r.stdout)
        self.assertIn('position_records=1 motion_batches=1', r.stdout)
        self.assertIn('records_only_not_current_readiness', r.stdout)

    def test_rebooted_usb_return_preserves_previous_boot_without_retiming(self):
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(OLD + '\n')
        (self.root / 'proc/uptime').write_text('2.00 1.00\n')
        self.trace.append(dict(kind='capture_end', boot_id=BOOT, mono_ns=99900000000))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('runtime_current_boot=unavailable', r.stdout)
        self.assertIn('guard_last_boot=different guard_consumed=present', r.stdout)
        self.assertIn('retained_runtime_last_boot=previous_boot boot_id=' + BOOT, r.stdout)
        self.assertIn('position_records=1 motion_batches=1 capture_end_record=observed', r.stdout)
        self.assertNotIn('health_recent=observed', r.stdout)

    def test_retained_summary_continues_rotation_but_does_not_invent_a_boot(self):
        self.write('trace.2.jsonl', self.trace[:4])
        r = self.run_status(trace=[dict(kind='position', mono_ns=100000000000, mode=1)])
        self.assertIn('retained_runtime_last_boot=current_boot boot_id=' + BOOT, r.stdout)
        self.assertIn('position_records=2 motion_batches=0', r.stdout)
        (self.logs / 'trace.2.jsonl').unlink()
        r = self.run_status(trace=[dict(kind='position', mono_ns=100000000000, mode=1)])
        self.assertIn('retained_runtime_last_boot=unavailable boot_id=unknown', r.stdout)
        self.assertIn('position_records=0 motion_batches=0', r.stdout)

    def test_new_or_malformed_boot_cannot_inherit_retained_counts(self):
        self.write('trace.1.jsonl', self.trace)
        for identity in (OLD, 'bad-log-text'):
            r = self.run_status(trace=[dict(kind='boot', boot_id=identity, mono_ns=1, mode=4)])
            self.assertIn('position_records=0 motion_batches=0', r.stdout)
            if identity == OLD:
                self.assertIn('retained_runtime_last_boot=previous_boot boot_id=' + OLD, r.stdout)
            else:
                self.assertIn('retained_runtime_last_boot=unavailable boot_id=unknown', r.stdout)

    def test_retained_model_rows_survive_aa_reset_without_claiming_current_solution(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        self.trace.append(dict(row, model_valid=True, result='OK', pipeline='OK',
                               drain_calls_total=40))
        self.trace.append(dict(kind='shadow_session', mono_ns=99000000000, reset=True))
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('model_solution=not_observed', r.stdout)
        self.assertIn('calculation_attempt_recent=unavailable', r.stdout)
        self.assertIn('retained_model_diagnostic_records=2 model_valid_records=1 drain_attempt_records=1', r.stdout)
        self.assertIn('records_only_not_trial_success', r.stdout)

    def test_same_boot_runtime_restart_retains_counts_but_new_boot_resets_them(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(model_valid=True, result='OK', pipeline='OK', drain_calls_total=40)
        for identity in (BOOT, OLD):
            rows = self.trace + [dict(kind='boot', boot_id=identity, mono_ns=99500000000, mode=4),
                                 dict(kind='position', mono_ns=99900000000, mode=1)]
            r = self.run_status(trace=rows)
            self.assertNotEqual(r.returncode, 0)  # New worker has no health yet.
            self.assertIn('model_solution=not_observed', r.stdout)
            if identity == BOOT:
                self.assertIn('position_records=2 motion_batches=1', r.stdout)
                self.assertIn('retained_model_diagnostic_records=1 model_valid_records=1 drain_attempt_records=1', r.stdout)
            else:
                self.assertIn('position_records=1 motion_batches=0', r.stdout)
                self.assertIn('retained_model_diagnostic_records=0 model_valid_records=0 drain_attempt_records=0', r.stdout)

    def test_retained_rows_do_not_promote_malformed_model_or_other_boot_end(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        rows = [item for item in self.trace if item is not row]
        for change in ({'domain': 'qualified'}, {'events': '4'}, {'model_valid': 'true'}):
            rows.append(dict(row, **change))
        # A missing/quoted counter is not proof of a drain invocation. Its
        # otherwise well-formed diagnostic still counts as a retained row.
        rows.append(dict(row, drain_calls_total='40'))
        rows.append(dict(kind='capture_end', boot_id=OLD, mono_ns=99900000000))
        r = self.run_status(trace=rows)
        self.assertIn('capture_end_record=not_observed', r.stdout)
        self.assertIn('retained_model_diagnostic_records=1 model_valid_records=0 drain_attempt_records=0', r.stdout)

    def test_capture_without_computation_preserves_collection_result(self):
        self.trace[1]['active'] = False
        self.trace[2]['computation_active'] = False
        rows = [row for row in self.trace if row['kind'] != 'shadow']
        r = self.run_status(trace=rows)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('capture_active=observed', r.stdout)
        self.assertIn('computation_active=unavailable', r.stdout)
        self.assertIn('model_diagnostic_recent=unavailable', r.stdout)

    def test_model_queue_count_does_not_claim_input_processing(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row['events'] = 0
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('model_diagnostic_recent=observed events_queued_total=0', r.stdout)
        self.assertNotIn('inputs_processed', r.stdout)

    def test_parked_wait_for_anchor_is_explicit_without_requiring_a_solution(self):
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('model_diagnostic_recent=observed events_queued_total=4', r.stdout)
        self.assertIn('model_solution=not_observed', r.stdout)
        self.assertIn('pipeline=WAITING', r.stdout)
        self.assertIn('gps_anchor_gate=WAITING', r.stdout)
        self.assertIn('Capture startup evidence only', r.stdout)

    def test_latest_solution_state_replaces_a_previous_valid_result(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        good = dict(row, mono_ns=98000000000, model_valid=True, result='OK', pipeline='OK')
        self.trace.insert(self.trace.index(row), good)
        r = self.run_status()
        self.assertIn('model_solution=not_observed', r.stdout)
        row.update(model_valid=True, result='OK', pipeline='OK')
        self.assertIn('model_solution=observed', self.run_status().stdout)

    def test_capture_terminal_records_revoke_recent_health_and_model_snapshot(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(model_valid=True, result='OK', pipeline='OK')
        for kind in ('capture_end', 'capture_incomplete'):
            r = self.run_status(trace=self.trace + [dict(kind=kind, mono_ns=99000000000,
                                                        boot_id=BOOT)])
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.assertIn('capture_active=unavailable', r.stdout)
            self.assertIn('model_diagnostic_recent=unavailable', r.stdout)
            self.assertIn('model_solution=not_observed', r.stdout)

    def test_model_boundaries_revoke_prior_solution_without_losing_raw_capture(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(model_valid=True, result='OK', pipeline='OK')
        for boundary in (dict(kind='shadow_session', mono_ns=99000000000, reset=True),
                         dict(kind='shadow_bus', mono_ns=99000000000, reset=True),
                         dict(kind='shadow_input_reset', mono_ns=99000000000,
                              reason='stale'),
                         dict(kind='shadow_pipeline_reset', mono_ns=99000000000,
                              reason='LATE', operation='raw', receive_seq=45),
                         dict(kind='shadow_disabled', reason='audit_fault')):
            result = self.run_status(trace=self.trace + [boundary])
            self.assertEqual(result.returncode, 0, boundary)
            self.assertIn('capture_active=observed', result.stdout)
            self.assertIn('model_diagnostic_recent=unavailable', result.stdout)
            self.assertIn('model_solution=not_observed', result.stdout)
            self.assertIn('gps_anchor_gate=none_observed', result.stdout)

    def test_primary_pipeline_reset_retracts_old_solution_and_shows_reason(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(model_valid=True, result='OK', pipeline='OK')
        reset = dict(kind='shadow_pipeline_reset', mono_ns=99000000000,
                     domain='model', assist_ready=False, reason='BAD_INPUT',
                     operation='raw', input_ns=99000000000,
                     receive_seq=45, sensor=1, call=0, resets=1)
        result = self.run_status(trace=self.trace + [reset])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('model_solution=not_observed', result.stdout)
        self.assertIn('last_pipeline_reset_this_boot=BAD_INPUT operation=raw receive_seq=45', result.stdout)

    def test_drain_counter_is_a_separate_calculation_attempt_not_a_solution(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row['drain_calls_total'] = 3
        row['events'] = 0
        result = self.run_status()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('calculation_attempt_recent=observed drain_calls_total=3 intervals_total=0', result.stdout)
        self.assertIn('model_solution=not_observed', result.stdout)

    def test_missing_stale_future_or_malformed_model_diagnostic_is_not_observed(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        for change in ({'mono_ns': 1000000000}, {'mono_ns': 101000000000},
                       {'events': -1}, {'events': '4'}, {'domain': 'qualified'}):
            rows = [dict(item, **change) if item is row else item for item in self.trace]
            result = self.run_status(trace=rows)
            self.assertEqual(result.returncode, 0, change)
            self.assertIn('model_diagnostic_recent=unavailable', result.stdout)
            self.assertNotIn('events_queued_total=', result.stdout)
        result = self.run_status(trace=[item for item in self.trace if item is not row])
        self.assertEqual(result.returncode, 0)
        self.assertIn('model_diagnostic_recent=unavailable', result.stdout)

    def test_position_and_rejection_reasons_are_visible(self):
        rows = [row for row in self.trace if row['kind'] != 'position']
        rows.append(dict(kind='shadow_position_rejected', mono_ns=99000000000,
                         reason='session_unavailable'))
        rows.append(dict(kind='shadow_input_reset', mono_ns=99000000000,
                         reason='stale'))
        rows.append(dict(kind='shadow_motion_excluded', mono_ns=99000000000,
                         reason='receipt_before_session'))
        r = self.run_status(trace=rows)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('oem_position_recent=unavailable', r.stdout)
        self.assertIn('last_position_rejection_30s=session_unavailable', r.stdout)
        self.assertIn('last_motion_reset_30s=stale', r.stdout)
        self.assertIn('last_model_exclusion_30s=receipt_before_session', r.stdout)

    def test_rejected_raw_checked_now_does_not_refresh_old_receipt(self):
        self.trace[-1]['events'] = self.trace[-1]['events'][:2]
        self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                               sensor=3, checked_ns=99000000000, received_ns=1000000000,
                               reason='stale'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('reverse_received_recently=unavailable receipt_only_not_direction_quality', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=observed worker_check_only', r.stdout)
        self.assertIn('rejected_raw_seen_this_boot=true', r.stdout)

    def test_recent_rejected_raw_is_diagnostic_only(self):
        self.trace[-1]['events'] = self.trace[-1]['events'][:2]
        self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                               sensor=3, checked_ns=99000000000, received_ns=99000000000,
                               reason='sequence_discontinuity'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('reverse_received_recently=unavailable receipt_only_not_direction_quality', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=observed worker_check_only', r.stdout)

    def test_future_at_worker_check_never_becomes_capture_success_later(self):
        self.trace[-1]['events'] = self.trace[-1]['events'][:2]
        self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                               sensor=3, checked_ns=99000000000, received_ns=99500000000,
                               reason='future'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('reverse_received_recently=unavailable', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=observed', r.stdout)

    def test_only_rejected_sensors_do_not_pass_collection_gate(self):
        self.trace.pop()
        for sensor in (1, 2, 3):
            self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                                   sensor=sensor, checked_ns=99000000000,
                                   received_ns=99000000000, reason='sequence_discontinuity'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('wheels_rejected_checked_recently=observed', r.stdout)
        self.assertIn('yaw_rejected_checked_recently=observed', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=observed', r.stdout)

    def test_untimed_rejection_is_visible_without_recent_receipt_claim(self):
        self.trace[-1]['events'] = self.trace[-1]['events'][:2]
        self.trace.append(dict(kind='motion_rejected', authenticated_decoded=True,
                               sensor=3, checked_ns=0, received_ns=99000000000,
                               reason='clock_unavailable'))
        self.trace.append(dict(kind='shadow_input_reset', mono_ns=0,
                               reason='clock_unavailable'))
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('reverse_received_recently=unavailable', r.stdout)
        self.assertIn('reverse_rejected_checked_recently=unavailable', r.stdout)
        self.assertIn('untimed_rejected_raw_seen=true', r.stdout)
        self.assertIn('untimed_motion_reset_seen=true', r.stdout)

    def test_silent_writer_stop_does_not_reuse_five_second_old_health(self):
        self.trace[2]['mono_ns'] = 94000000000
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(model_valid=True, result='OK', pipeline='OK')
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('health_recent=unavailable window=5s', r.stdout)
        self.assertIn('model_solution=not_observed', r.stdout)

    def test_persistent_profile_health_window_and_digest_receipts(self):
        # log_profile=persistent (validation/PERSISTENT_LOGGING_2026-10-06.md):
        # health at most once per 10 s and raw motion rows only around events;
        # the 10 s digest carries each sensor's latest receipt time.
        self.trace[0]['log_profile'] = 'persistent'
        self.trace[2]['mono_ns'] = 88000000000      # 12 s old: within 15 s, not 5 s
        self.trace = [row for row in self.trace if row['kind'] != 'motion_batch']
        self.trace.append(dict(kind='log_digest', schema=1, digest='periodic', profile='persistent',
                               mono_ns=95000000000, wheels_last_ns=95000000000,
                               yaw_last_ns=95000000000, reverse_last_ns=95000000000))
        r = self.run_status()
        self.assertIn('health_recent=observed window=15s', r.stdout)
        for sensor in ('wheels', 'yaw', 'reverse'):
            self.assertIn(sensor + '_received_recently=observed', r.stdout)
        self.assertIn('log_profile=persistent health_window_s=15', r.stdout)
        # A digest of another boot or from the future proves nothing.
        self.trace[-1].update(wheels_last_ns=500000000, yaw_last_ns=101000000000)
        r = self.run_status()
        self.assertIn('wheels_received_recently=unavailable', r.stdout)
        self.assertIn('yaw_received_recently=unavailable', r.stdout)
        # The full profile keeps its 5 s health window for the same rows.
        del self.trace[0]['log_profile']
        r = self.run_status()
        self.assertIn('health_recent=unavailable window=5s', r.stdout)
        self.assertIn('log_profile=full health_window_s=5', r.stdout)

    def test_log_caps_follow_the_installed_config(self):
        # Log retention (validation/LOG_RETENTION_2026-10-10.md): the persistent
        # install is 3 x 16 MiB trace and the collector's 2 x 1 MiB clamp.
        (self.base / 'mx5dr.conf').write_text(
            'mode=SHADOW\nmax_log_bytes=16777216\nmax_log_files=3\nsample_ms=1000\nlog_profile=persistent\n')
        out = self.run_status().stdout
        self.assertIn('trace_cap_bytes=50331648 collector_cap_bytes=2097152 (48+2 MiB, rotates)', out)
        self.assertIn('every boot appends to trace.0 until it is full, so restarts no longer push a drive out', out)
        self.assertIn('about 4.6-6.9 h (tunnel mode about 2.2-3.2 h)', out)
        self.assertIn('Collector files still rotate per boot', out)
        self.assertNotIn('120 MiB', out)
        # One-boot trial config: 3 x 40 MiB and the collector's 2 x 4 MiB.
        (self.base / 'mx5dr.conf').write_text('mode=SHADOW\nmax_log_bytes=41943040\nmax_log_files=3\nsample_ms=1000\n')
        out = self.run_status().stdout
        self.assertIn('trace_cap_bytes=125829120 collector_cap_bytes=8388608 (120+8 MiB, rotates)', out)
        self.assertIn('the 120 MiB trace can rotate out its oldest data after about 58 minutes', out)
        # Runtime defaults when the keys are absent: 3 x 8 MiB.
        (self.base / 'mx5dr.conf').write_text(CONFIG)
        self.assertIn('trace_cap_bytes=25165824 collector_cap_bytes=8388608 (24+8 MiB, rotates)',
                      self.run_status().stdout)
        (self.base / 'mx5dr.conf').write_text(CONFIG + 'max_log_files=9\n')
        self.assertIn('trace_cap_bytes=unconfirmed', self.run_status().stdout)

    def test_persistent_profile_config_key_is_accepted(self):
        (self.base / 'mx5dr.conf').write_text(CONFIG + 'log_profile=persistent\n')
        self.assertIn('config_mode=SHADOW', self.run_status().stdout)
        (self.base / 'mx5dr.conf').write_text(CONFIG + 'log_profile=quiet\n')
        self.assertIn('config_mode=unconfirmed', self.run_status().stdout)

    def test_silent_collector_stop_does_not_reuse_eight_second_old_poll(self):
        self.collector[-1]['end_ns'] = 91000000000
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('collector_poll_recent=unavailable window=8s', r.stdout)

    def test_old_model_snapshot_is_not_presented_as_current_solution(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row.update(mono_ns=97000000000, model_valid=True, result='OK', pipeline='OK')
        r = self.run_status()
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('model_diagnostic_recent=unavailable', r.stdout)
        self.assertIn('model_solution=not_observed', r.stdout)

    def test_calculation_attempt_requires_an_actual_drain_counter(self):
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        for counter, observed in ((None, False), (0, False), (1, True), ('1', False)):
            row['drain_calls_total'] = counter
            r = self.run_status()
            self.assertIn('calculation_attempt_recent=' + ('observed' if observed else 'unavailable'), r.stdout)
            self.assertEqual(r.returncode, 0, r.stdout)

    def test_pipeline_reset_reason_survives_a_later_successful_input(self):
        self.trace.insert(3, dict(kind='shadow_pipeline_reset', mono_ns=98000000000,
                                 reason='LATE', operation='raw', receive_seq=45))
        row = next(row for row in self.trace if row['kind'] == 'shadow')
        row['pipeline'] = 'OK'
        r = self.run_status()
        self.assertIn('last_pipeline_reset_this_boot=LATE operation=raw receive_seq=45', r.stdout)
        self.assertIn('capture_active=observed', r.stdout)

    def test_storage_stop_is_visible_even_while_last_health_is_recent(self):
        self.write('trace.storage.json', [dict(kind='storage_stop', stream='trace',
                   boot_id=BOOT, mono_ns=99500000000, reason='low_space',
                   available_bytes=7 * 1024 * 1024, reserve_bytes=8 * 1024 * 1024)])
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('storage_stop=trace reason=low_space', r.stdout)
        self.assertIn('capture_active=unavailable', r.stdout)
        self.assertIn('computation_active=unavailable', r.stdout)

    def test_old_boot_storage_stop_does_not_disable_current_capture(self):
        self.write('trace.storage.json', [dict(kind='storage_stop', stream='trace',
                   boot_id=OLD, mono_ns=99000000000, reason='low_space')])
        self.assertEqual(self.run_status().returncode, 0)

    def test_restarted_worker_does_not_hide_prior_storage_failure(self):
        self.trace[0]['mono_ns'] = 90000000000
        self.write('trace.storage.json', [dict(kind='storage_stop', stream='trace',
                   boot_id=BOOT, mono_ns=80000000000, reason='low_space')])
        r = self.run_status()
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn('storage_stop=trace reason=low_space', r.stdout)
        self.assertIn('capture_active=observed', r.stdout)

    def test_old_boot_and_missing_boot_do_not_use_fresh_looking_rows(self):
        self.trace[0]['boot_id'] = OLD
        self.assertNotEqual(self.run_status().returncode, 0)
        self.assertNotEqual(self.run_status(trace=self.trace[1:]).returncode, 0)

    def test_expired_or_future_health_and_sensors(self):
        for ns in (1000000000, 101000000000):
            self.trace[2]['mono_ns'] = ns
            self.assertNotEqual(self.run_status().returncode, 0)
        self.trace[2]['mono_ns'] = 99000000000
        self.trace[-1]['events'][2][2] = 1000000000
        self.assertNotEqual(self.run_status().returncode, 0)

    def test_uptime_centisecond_quantization_has_bounded_allowance(self):
        # Real CLOCK_MONOTONIC records may be just under 10 ms ahead of the
        # truncated /proc/uptime text, but a later future record must fail.
        for ns, expected in ((100009999999, 0), (100010000000, 0),
                             (100010000001, 1), (100020000000, 1)):
            self.collector[-1]['end_ns'] = ns
            r = self.run_status()
            self.assertEqual(r.returncode, expected, r.stdout + r.stderr)
        self.collector[-1]['end_ns'] = 99000000000
        self.trace[2]['mono_ns'] = 100010000001
        self.assertNotEqual(self.run_status().returncode, 0)
        self.trace[2]['mono_ns'] = 69999999999
        self.assertNotEqual(self.run_status().returncode, 0)  # age limit unchanged

    def test_rejected_capture_hook_and_audit(self):
        for field, value in (('capture_active', False), ('hook_installed', False),
                             ('audit_fault', 1), ('dropped', 1)):
            original = self.trace[2][field]
            self.trace[2][field] = value
            self.assertNotEqual(self.run_status().returncode, 0)
            self.trace[2][field] = original

    def test_collector_stopped_stale_and_old_boot(self):
        self.assertNotEqual(self.run_status(collector=self.collector + [dict(kind='collector_stop', reason='signal')]).returncode, 0)
        self.collector[-1]['end_ns'] = 1000000000
        self.assertNotEqual(self.run_status().returncode, 0)
        self.collector[-1]['end_ns'] = 99000000000
        self.collector[0]['boot_id'] = OLD
        self.assertNotEqual(self.run_status().returncode, 0)

    def test_rotation_continuity_and_new_boot_reset(self):
        self.write('trace.1.jsonl', self.trace[:2])
        self.assertEqual(self.run_status(trace=self.trace[2:]).returncode, 0)
        self.assertNotEqual(self.run_status(trace=[dict(kind='boot', boot_id=OLD, mono_ns=1, mode=4)] + self.trace[2:]).returncode, 0)

    def test_oneboot_marker_and_rearm_never_count_as_active_capture(self):
        (self.base / 'guard/last-boot').write_text(OLD + '\n')
        self.assertNotEqual(self.run_status().returncode, 0)
        (self.base / 'guard/last-boot').write_text(BOOT + '\n')
        (self.base / 'guard/arm').write_text('pending')
        self.assertNotEqual(self.run_status().returncode, 0)

    @unittest.skipUnless(COLLECTOR.exists(), 'Host collector build unavailable')
    def test_real_collector_journal_envelope(self):
        actual_boot = Path('/proc/sys/kernel/random/boot_id').read_text()
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(actual_boot)
        (self.base / 'guard/last-boot').write_text(actual_boot)
        (self.base / 'mx5dr.conf').write_text('mode=SHADOW\nsample_ms=500\n')
        updated = hashlib.sha256((self.base / 'mx5dr.conf').read_bytes()).hexdigest()
        (self.base / 'guard/consumed').write_text(MANIFEST.replace(CONFIG_DIGEST, updated, 1))
        proc = subprocess.Popen([str(COLLECTOR), '--root', str(self.base),
                                 '--bus-address', 'unix:path=' + str(self.root / 'absent')], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        captured = []
        try:
            deadline = time.monotonic() + 3
            logfile = self.logs / 'collector.0.jsonl'
            while time.monotonic() < deadline:
                if logfile.exists() and '"kind":"poll"' in logfile.read_text():
                    break
                self.assertIsNone(proc.poll())
                time.sleep(.01)
            self.assertTrue(logfile.exists())
            for line in logfile.read_text().splitlines():
                captured.append(line)
                if '"kind":"poll"' in line:
                    break
            self.assertTrue(any('"kind":"poll"' in line for line in captured))
        finally:
            (self.logs / 'collector.stop').mkdir(exist_ok=True)
            try:
                proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()
                self.fail('Host fixture collector failed cooperative shutdown')
        # Replay the real emitted envelope as a fixed snapshot. Otherwise a
        # second live poll can race the fixture's earlier /proc/uptime value.
        logfile.write_text('\n'.join(captured) + '\n')
        now = time.monotonic_ns() - 100000000
        self.trace[0]['boot_id'] = actual_boot.strip()
        self.trace[0]['mono_ns'] = now - 1000000000
        for row in self.trace[2:]:
            if 'mono_ns' in row:
                row['mono_ns'] = now
        for row in self.trace[-1]['events']:
            row[2] = now
        self.write('trace.0.jsonl', self.trace)
        (self.root / 'proc/uptime').write_text(Path('/proc/uptime').read_text())
        r = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                           env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('collector_poll_recent=observed', r.stdout)

    def test_actual_collector_envelope_required(self):
        self.write('trace.0.jsonl', self.trace)
        # Plain kind-first records are not produced by the collector journal.
        (self.logs / 'collector.0.jsonl').write_text(''.join(
            json.dumps(row, separators=(',', ':')) + '\n' for row in self.collector))
        r = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                           env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('collector_poll_recent=unavailable', r.stdout)

    def test_symlink_log_rejected(self):
        target = self.root / 'outside'
        target.write_text('do not interpret\n')
        (self.logs / 'trace.2.jsonl').symlink_to(target)
        self.assertNotEqual(self.run_status().returncode, 0)

    def test_rows_written_after_the_snapshot_time_do_not_hide_a_live_runtime(self):
        # 2026-10-05 shadow.5 export: the journals kept growing after `now` was
        # read, and the newest health/poll rows (newer than `now`) replaced the
        # live values, giving health_recent/hooks/POLL unavailable during a GO.
        later = 100500000000  # 0.5 s after the fixture uptime of 100.00 s
        trace = self.trace + [
            dict(kind='health', mono_ns=later, hook_installed=True, audit_fault=0, dropped=0,
                 capture_active=True, computation_active=True),
            dict(kind='position', mono_ns=later, mode=0),
            dict(kind='shadow', mono_ns=later, domain='model', assist_ready=False, model_valid=False,
                 events=9, intervals=0, result='E_NO_SEED', pipeline='WAITING'),
            dict(kind='capture_end', boot_id=BOOT, mono_ns=later)]
        collector = self.collector + [dict(kind='poll', end_ns=later, seq=1)]
        self.write('collector.0.jsonl', collector)
        # A collector stop recorded after `now` (exact production envelope).
        with (self.logs / 'collector.0.jsonl').open('a') as stream:
            stream.write(json.dumps(dict(stream='collector', collector_pid=123, observed_at_mono_ns=later,
                                         producer_mono_ns=None, producer_time_status='unknown',
                                         kind='collector_stop', samples=2, reason='stop_marker'),
                                    separators=(',', ':')) + '\n')
        self.write('trace.0.jsonl', trace)
        result = subprocess.run(['sh', str(PACK / 'trial_status.sh')], capture_output=True, text=True,
                                env=dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root)))
        r = result
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('health_recent=observed window=5s health_age_s=1', r.stdout)
        self.assertIn('hooks=observed install=ok health_hook_installed=true', r.stdout)
        self.assertIn('capture_active=observed', r.stdout)
        self.assertIn('collector_poll_recent=observed window=8s poll_age_s=1', r.stdout)
        self.assertIn('oem_position_recent=observed mode=1', r.stdout)

    def test_hook_check_uses_boot_install_and_ages_are_shown(self):
        trace = [dict(self.trace[0], install='symbol_missing')] + self.trace[1:]
        r = self.run_status(trace)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('hooks=unavailable install=symbol_missing health_hook_installed=true', r.stdout)
        (self.root / 'proc/uptime').write_text('120.00 1.00\n')
        r = self.run_status()
        self.assertIn('health_recent=unavailable window=5s health_age_s=21', r.stdout)
        self.assertIn('hooks=observed install=ok', r.stdout)  # a stale health row does not hide the hook
        self.assertIn('collector_poll_recent=unavailable window=8s poll_age_s=21', r.stdout)

    def beta_trace(self, install='ok', enabled=True):
        config = 'mode=BETA\nsample_ms=1000\n'
        (self.base / 'mx5dr.conf').write_text(config)
        (self.base / 'guard/consumed').write_text(
            MANIFEST.replace(CONFIG_DIGEST, hashlib.sha256(config.encode()).hexdigest()))
        boot = dict(kind='boot', boot_id=BOOT, mono_ns=1000000000, mode=5, install=install,
                    assist_ready=False, session_hooks='declined_third_party_interposer',
                    beta=dict(mode='BETA', enabled=enabled,
                              reason='adapter_opt_in' if enabled else 'hook_not_installed',
                              session_fence='declined_send_storage_counter'),
                    install_diag=dict(stage=0, owner='none'))

        def state(old, new, reason):
            return dict(kind='beta_state', mono_ns=50000000000, domain='beta', assist_ready=False,
                        **{'from': old}, to=new, reason=reason, adapter_mode=4)

        def send(choice, result=0):
            return dict(kind='send', call=1, generation=1, mono_ns=50000000000, mode=0,
                        type=1, length=48, choice=choice, reason=0, result=result)
        if not enabled:
            return [boot, state('DISABLED', 'DISABLED', 'hook_not_installed')] + self.trace[1:]
        return [boot] + self.trace[1:] + [
            state('DISABLED', 'ARMED', 'enabled'), state('ARMED', 'GPS_LOST', 'gps_lost'),
            state('GPS_LOST', 'ENGAGED', 'published'), send(3), send(3), send(0),
            dict(kind='beta_hold', mono_ns=50000000000, domain='beta', event='hold_set', count=1,
                 held=True, state='ENGAGED'),
            send(3, result=-1),
            state('ENGAGED', 'WITHDRAWN', 'send_result_hold'), state('WITHDRAWN', 'ARMED', 'gps_returned'),
            state('ARMED', 'GPS_LOST', 'gps_lost'), state('GPS_LOST', 'ENGAGED', 'published'),
            state('ENGAGED', 'ARMED', 'gps_returned')]

    def test_beta_boot_reports_enable_hook_fence_and_owner_count_line(self):
        r = self.run_status(self.beta_trace())
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('config_mode=BETA', r.stdout)
        self.assertIn('guard_config_binding=matched', r.stdout)
        self.assertIn('capture_active=observed', r.stdout)
        self.assertIn('beta_requested=true beta_scope=current_boot', r.stdout)
        self.assertIn('beta_boot_enabled=true beta_boot_reason=adapter_opt_in beta_hook=installed '
                      'beta_install=ok beta_session_fence=declined', r.stdout)
        self.assertIn('beta_enable=armed beta_last_state=ARMED beta_last_reason=gps_returned', r.stdout)
        self.assertIn('beta_engaged=2 beta_replaced_sends=3 beta_replaced_nonzero=1 beta_hold_set=1 '
                      'beta_withdrawals=1 beta_last_withdraw_reason=send_result_hold', r.stdout)
        self.assertIn('BETA: engaged 2 times, replaced 3 sends, speed overlay sends 0, nonzero 0, hold 1, '
                      'last state ARMED (gps_returned), scope current_boot', r.stdout)

    def test_beta_counts_survive_the_drive_reboot_as_previous_boot(self):
        trace = self.beta_trace()
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(OLD + '\n')
        (self.root / 'proc/uptime').write_text('2.00 1.00\n')
        r = self.run_status(trace)
        self.assertIn('beta_requested=true beta_scope=previous_boot', r.stdout)
        self.assertIn('BETA: engaged 2 times, replaced 3 sends, speed overlay sends 0, nonzero 0, hold 1, '
                      'last state ARMED (gps_returned), scope previous_boot', r.stdout)

    def test_beta_disabled_reason_and_missing_hook_are_visible(self):
        r = self.run_status(self.beta_trace(install='symbol_missing', enabled=False))
        self.assertIn('beta_boot_enabled=false beta_boot_reason=hook_not_installed beta_hook=not_installed', r.stdout)
        self.assertIn('beta_enable=disabled:hook_not_installed', r.stdout)
        self.assertIn('BETA: engaged 0 times, replaced 0 sends, speed overlay sends 0, nonzero 0, hold 0, '
                      'last state DISABLED', r.stdout)

    def test_beta_mode5_boot_counts_as_shadow_capture_and_computation(self):
        r = self.run_status(self.beta_trace())
        self.assertIn('runtime_current_boot=observed mode=5', r.stdout)
        for line in ('capture_active=observed', 'computation_active=observed',
                     'model_diagnostic_recent=observed', 'wheels_received_recently=observed',
                     'collector_poll_recent=observed'):
            self.assertIn(line, r.stdout)
        self.assertNotIn('BETA NO_FIX', r.stdout)  # absent field: no line

    def test_no_fix_speed_overlay_sends_are_counted_apart_from_replacements(self):
        trace = self.beta_trace()

        def row(kind, **fields):
            return dict(kind=kind, mono_ns=50000000000, **fields)
        trace += [
            row('beta_state', domain='beta', assist_ready=False, to='NO_FIX', reason='no_fix',
                adapter_mode=4, position_class='NO_FIX', **{'from': 'ARMED'}),
            row('beta_state', domain='beta', assist_ready=False, to='SPEED_ENGAGED', reason='speed_published',
                adapter_mode=4, position_class='NO_FIX', **{'from': 'NO_FIX'})]
        for result in (0, 0, -1):
            trace.append(dict(kind='send', call=9, generation=1, mono_ns=50000000000, mode=1, type=1,
                              length=48, choice=4, reason=0, result=result))
        r = self.run_status(trace)
        self.assertIn('beta_speed_engaged=1 beta_speed_overlay_sends=3 beta_speed_overlay_nonzero=1', r.stdout)
        self.assertIn('beta_engaged=2 beta_replaced_sends=3 beta_replaced_nonzero=1', r.stdout)
        self.assertIn('BETA: engaged 2 times, replaced 3 sends, speed overlay sends 3, nonzero 1, hold 1, '
                      'last state SPEED_ENGAGED (speed_published), scope current_boot', r.stdout)

    def test_optional_position_class_prints_the_no_fix_line(self):
        trace = self.beta_trace()
        for row in trace:
            if row['kind'] == 'beta_state':
                row['position_class'] = 'NO_FIX' if row['to'] == 'GPS_LOST' else 'FIX'
        r = self.run_status(trace)
        self.assertIn('BETA NO_FIX: 2 state rows', r.stdout)

    def test_shadow_boot_has_no_beta_count_line(self):
        r = self.run_status()
        self.assertIn('beta_requested=false', r.stdout)
        self.assertNotIn('BETA:', r.stdout)
        self.assertNotIn('beta_engaged=', r.stdout)

    def test_symlink_parent_rejected(self):
        target = self.root / 'outside'
        self.logs.rename(target)
        self.logs.symlink_to(target, target_is_directory=True)
        self.assertNotEqual(self.run_status().returncode, 0)


if __name__ == '__main__':
    unittest.main()
