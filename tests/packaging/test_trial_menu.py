"""Numeric USB workflow with real helpers and synthetic firmware fixtures."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import unittest

import test_synthetic_install

BOOT = '12345678-1234-1234-1234-123456789abc\n'
MANIFEST = 'mx5dr-one-boot-v3\n' + ('a' * 64 + '\n') * 8


class TrialMenuTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_synthetic_install.SyntheticPackagingTests()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.root = self.fixture.root
        self.usb = self.root / 'tmp/mnt/sdb1'
        self.usb.parent.mkdir(parents=True)
        shutil.move(str(self.fixture.bundle), self.usb)
        self.fixture.bundle = self.usb
        self.base = self.root / 'data_persist/mx5-aa-dr'
        self.logs = self.base / 'logs'
        bootfile = self.root / 'proc/sys/kernel/random/boot_id'
        bootfile.parent.mkdir(parents=True, exist_ok=True)
        bootfile.write_text(BOOT)
        (self.root / 'proc/uptime').write_text('100.00 1.00\n')
        # Linux exposes /proc/mounts as a kernel-owned relative symlink. The
        # earlier regular-file fixture missed the actual menu3 startup failure.
        (self.root / 'proc/self').mkdir()
        self.mounts = self.root / 'proc/self/mounts'
        self.mounts.write_text(f'/dev/sdb1 {self.usb} vfat rw 0 0\n')
        (self.root / 'proc/mounts').symlink_to('self/mounts')
        self.env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(self.root),
                        MX5DR_FIXTURE_FINISH_WAIT='0')

    def menu(self, keys, preexec_fn=None):
        return subprocess.run(['sh', str(self.usb / 'trial')], input=keys,
                              text=True, capture_output=True, env=self.env,
                              cwd=self.root, timeout=20, preexec_fn=preexec_fn)

    def prepare_logs(self, acknowledged=True):
        self.fixture.run_script('install.sh')
        (self.logs / 'trace.0.jsonl').write_text('retained raw evidence\n')
        if acknowledged:
            (self.logs / 'capture.done').write_text(BOOT)

    def assert_export(self):
        archives = list(self.usb.glob('mx5dr-logs-*.tar'))
        self.assertEqual(len(archives), 1)
        archive = archives[0]
        checksum = archive.with_suffix('.tar.sha256').read_text().split()[0]
        self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), checksum)
        with tarfile.open(archive) as tar:
            member, = [m for m in tar.getmembers()
                       if m.name.endswith('/mx5-aa-dr/logs/trace.0.jsonl')]
            self.assertEqual(tar.extractfile(member).read(),
                             b'retained raw evidence\n')
        self.assertEqual((self.logs / 'trace.0.jsonl').read_text(),
                         'retained raw evidence\n')
        return (self.usb / 'trial-result.txt').read_text()

    def test_eof_blank_and_invalid_input_never_install(self):
        for keys in ('', '\n0\n', 'bad\n0\n', '$(touch unexpected)\n0\n'):
            result = self.menu(keys)
            self.assertIn('1 Install', result.stdout)
            self.assertFalse(self.base.exists())
            self.assertEqual(self.fixture.autostart.read_bytes(),
                             self.fixture.original_autostart)
            self.assertFalse((self.root / 'unexpected').exists())

    def test_install_uses_bundle_mode_without_more_typed_commands(self):
        (self.usb / 'bundle-default-mode').write_text('SHADOW\n')
        result = self.menu('1\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('mode=SHADOW', (self.base / 'mx5dr.conf').read_text())
        self.assertIn('ONE-BOOT BEGIN', self.fixture.autostart.read_text())
        self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_menu_two_distinguishes_arming_boot_from_new_linux_boot(self):
        installed = self.menu('1\n')
        self.assertEqual(installed.returncode, 0, installed.stdout + installed.stderr)
        self.assertEqual((self.base / 'guard/armed-boot').read_text(), BOOT)
        # This host fixture stages files but cannot execute the ARM guard.
        (self.base / 'guard/arm').write_text(MANIFEST)
        before = self.menu('2\n0\n')
        self.assertIn('current_boot_id=' + BOOT.strip(), before.stdout)
        self.assertIn('startup_state=awaiting_linux_reboot', before.stdout)
        self.assertIn('one_boot=arm_present retained_bytes=0', before.stdout)
        self.assertIn('NO   GUARD awaiting_linux_reboot', before.stdout)
        self.assertIn('NO-GO', before.stdout)
        self.assertIn('startup_state=awaiting_linux_reboot',
                      (self.usb / 'startup-result.txt').read_text())
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(
            BOOT.replace('12345678', '87654321'))
        after = self.menu('2\n0\n')
        self.assertIn('startup_state=new_linux_boot_arm_unconsumed', after.stdout)
        self.assertIn('wait GUARD not consumed yet', after.stdout)
        self.assertIn('one_boot=arm_present retained_bytes=0', after.stdout)
        self.assertFalse((self.base / 'guard/last-boot').exists())

    def test_install_failure_is_not_reported_as_success(self):
        (self.usb / 'libmx5dr.so').write_bytes(b'damaged')
        result = self.menu('1\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Truncated ELF header', result.stderr)
        self.assertEqual(self.fixture.autostart.read_bytes(),
                         self.fixture.original_autostart)

    def reboot_witness(self):
        # This witness only checks the explicit menu handoff. The reboot
        # helper and original BusyBox/PID 1 path have separate tests.
        (self.usb / 'reboot_cmu.sh').write_text(
            '#!/bin/sh\necho requested >> "$MX5DR_FIXTURE_ROOT/reboot.calls"\n')

    def test_install_returns_to_menu_and_reboot_requires_explicit_five(self):
        self.reboot_witness()
        result = self.menu('1\n5\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / 'reboot.calls').read_text(), 'requested\n')
        self.assertIn('5 Reboot CMU', result.stdout)

    def test_eof_after_install_never_requests_reboot(self):
        self.reboot_witness()
        result = self.menu('1\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / 'reboot.calls').exists())

    def test_failed_reboot_request_is_reported(self):
        (self.usb / 'reboot_cmu.sh').write_text('#!/bin/sh\nexit 7\n')
        result = self.menu('5\n')
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)

    def test_status_checks_boot_boundary_and_saves_it_without_aa_connected(self):
        self.prepare_logs()
        receipt = self.usb / 'reboot-request.txt'
        receipt.write_text('schema=1\nboot_id=' + BOOT)
        result = self.menu('2\n0\n')
        self.assertIn('reboot_check=same_boot', result.stdout)
        report = self.usb / 'startup-result.txt'
        self.assertIn('reboot_check=same_boot', report.read_text())
        self.assertIn('NO   BOOT  same_boot', result.stdout)
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(
            BOOT.replace('12345678', '87654321'))
        result = self.menu('2\n0\n')
        self.assertIn('reboot_check=new_boot_observed', result.stdout)
        self.assertIn('reboot_check=new_boot_observed', report.read_text())
        self.assertIn('ok   BOOT  new boot', result.stdout)
        # The verdict is also kept at the end of the saved startup report.
        self.assertIn('---- GO / NO-GO ----', report.read_text())
        self.assertIn('ok   BOOT  new boot', report.read_text())

    def test_verdict_is_the_last_thing_on_the_screen_after_menu_two(self):
        self.prepare_logs()
        result = self.menu('2\n0\n')
        self.assertEqual(result.stdout.count('Parked USB trial menu'), 1)
        out = result.stdout
        self.assertLess(out.index('---- GO / NO-GO ----'), out.index('0 Exit (1-5 as listed before)'))
        self.assertIn('Startup check saved', out[:out.index('---- GO / NO-GO ----')])
        for line in out[out.index('---- GO / NO-GO ----'):].splitlines():
            self.assertLessEqual(len(line), 40, line)

    def verdict(self, text, want=''):
        script = (self.usb / 'trial').read_text()
        start = script.index('startup_verdict() {')
        function = script[start:script.index('\n}\n', start) + 3]
        return subprocess.run(['sh', '-c', function + '\nstartup_verdict "$1"', 'sh', want], input=text,
                              text=True, capture_output=True, check=True).stdout

    def test_beta_verdict_shows_mode_enable_hook_fence_and_counts(self):
        go = self.verdict(self.BETA_GO, 'BETA').splitlines()
        self.assertEqual(go[-1], 'GO')
        for expect in ('ok   MODE  BETA', 'ok   BETA  armed', 'ok   HOOK  installed', 'ok   FENCE declined'):
            self.assertIn(expect, go)
        self.assertEqual(sum(l.startswith('ok   ') for l in go), 13)
        self.assertIn('BETA: engaged 0 times, replaced 0 sends,', go)
        for line in go:
            self.assertLessEqual(len(line), 40, line)
        after = self.BETA_GO.replace('beta_engaged=0', 'beta_engaged=12').replace(
            'BETA: engaged 0 times, replaced 0 sends, hold 0, last state ARMED (enabled), scope current_boot',
            'BETA: engaged 12 times, replaced 3456 sends, hold 1, last state WITHDRAWN (budget_limit), '
            'scope previous_boot').replace('beta_scope=current_boot', 'beta_scope=previous_boot')
        lines = self.verdict(after, 'BETA').splitlines()
        text = ' '.join(l.strip() for l in lines)
        self.assertIn('BETA: engaged 12 times, replaced 3456 sends, hold 1, last state WITHDRAWN '
                      '(budget_limit), scope previous_boot', text)
        self.assertIn('wait BETA  not started', lines)
        for line in lines:
            self.assertLessEqual(len(line), 40, line)

    def test_beta_verdict_failures_and_mode_mismatch(self):
        for old, new, expect in (
                ('beta_enable=armed', 'beta_enable=disabled:hook_not_installed', 'NO   BETA  disabled:hook_not_instal'),
                ('beta_enable=armed', 'beta_enable=none_observed', 'wait BETA  no state yet'),
                ('beta_hook=installed', 'beta_hook=not_installed', 'NO   HOOK  not_installed'),
                ('beta_session_fence=declined', 'beta_session_fence=missing', 'NO   FENCE missing')):
            lines = self.verdict(self.BETA_GO.replace(old, new), 'BETA').splitlines()
            self.assertIn(expect, lines)
        # A BETA USB must not pass a SHADOW installation, nor the reverse.
        lines = self.verdict(self.GO, 'BETA').splitlines()
        self.assertIn('NO   MODE  SHADOW not BETA', lines)
        self.assertEqual(lines[-1], 'NO-GO')
        lines = self.verdict(self.BETA_GO, 'SHADOW').splitlines()
        self.assertIn('NO   MODE  BETA not SHADOW', lines)
        self.assertEqual(self.verdict(self.GO, 'SHADOW').splitlines()[-1], 'GO')

    def test_beta_bundle_menu_names_the_mode_and_installs_it_with_one_key(self):
        (self.usb / 'bundle-default-mode').write_text('BETA\n')
        result = self.menu('1\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('This USB installs mode BETA: BETA = SHADOW capture', result.stdout)
        self.assertIn('1 Install BETA for the next boot', result.stdout)
        self.assertIn('mode=BETA', (self.base / 'mx5dr.conf').read_text())
        for name in ('normal.trial', 'wcp.trial'):
            self.assertIn('libmx5dr-vimtap.so', (self.base / 'guard' / name).read_text())
        status = self.menu('2\n0\n')
        self.assertIn('bundle_mode=BETA', status.stdout)
        self.assertIn('bundle_mode=BETA', (self.usb / 'startup-result.txt').read_text())

    def test_shadow_bundle_menu_says_location_is_never_replaced(self):
        (self.usb / 'bundle-default-mode').write_text('SHADOW\n')
        result = self.menu('0\n')
        self.assertIn('This USB installs mode SHADOW: SHADOW = capture only; LOCATION is never replaced',
                      result.stdout)
        self.assertIn('1 Install SHADOW for the next boot', result.stdout)

    GO = ('reboot_check=new_boot_observed (boot ID comparison)\n'
          'runtime_disable_next_start=absent\n'
          'one_boot=consumed_this_boot retained_bytes=1275 trace_cap_bytes=1\n'
          'guard_arm=absent guard_armed_boot=different startup_state=guard_committed_after_new_boot\n'
          'config_mode=SHADOW\n'
          'service_jciAAPA=running pid=812 uid=0 stack_soft_bytes=131072 package_preload=yes\n'
          'service_jciLDS=running pid=813 uid=0 stack_soft_bytes=131072 package_preload=yes\n'
          'service_jciVBS=running pid=814 uid=0 stack_soft_bytes=131072 package_preload=yes\n'
          'collector_poll_recent=observed window=8s\n')

    BETA_GO = (GO.replace('config_mode=SHADOW', 'config_mode=BETA') +
               'beta_requested=true beta_scope=current_boot records_only_not_phone_acceptance\n'
               'beta_boot_enabled=true beta_boot_reason=adapter_opt_in beta_hook=installed beta_install=ok '
               'beta_session_fence=declined beta_session_hooks=declined_third_party_interposer\n'
               'beta_enable=armed beta_last_state=ARMED beta_last_reason=enabled\n'
               'beta_engaged=0 beta_replaced_sends=0 beta_replaced_nonzero=0 beta_hold_set=0 '
               'beta_withdrawals=0 beta_last_withdraw_reason=none\n'
               'BETA: engaged 0 times, replaced 0 sends, hold 0, last state ARMED (enabled), scope current_boot\n')

    def test_verdict_go_wait_and_no_go(self):
        go = self.verdict(self.GO).splitlines()
        self.assertEqual(go[0], '---- GO / NO-GO ----')
        self.assertEqual(go[-1], 'GO')
        self.assertEqual(sum(l.startswith('ok   ') for l in go), 10)
        wait = self.verdict(self.GO.replace(
            'service_jciAAPA=running pid=812 uid=0 stack_soft_bytes=131072 package_preload=yes',
            'service_jciAAPA=not_running')).splitlines()
        self.assertIn('wait AAPA  not running', wait)
        self.assertEqual(wait[-1], 'WAIT 60 s, then run 2 again')
        wait = self.verdict(self.GO.replace('retained_bytes=1275', 'retained_bytes=0').replace(
            'collector_poll_recent=observed', 'collector_poll_recent=none')).splitlines()
        self.assertIn('wait DATA  0 bytes', wait)
        self.assertIn('wait POLL  none', wait)
        # A definite failure wins over a pending item.
        mixed = self.verdict(self.GO.replace('package_preload=yes', 'package_preload=no', 1).replace(
            'collector_poll_recent=observed', 'collector_poll_recent=none')).splitlines()
        self.assertIn('NO   AAPA  preload no', mixed)
        self.assertEqual(mixed[-1], 'NO-GO')
        for text, expect in (('reboot_check=same_boot\n', 'NO   BOOT  same_boot'),
                             (self.GO.replace('config_mode=SHADOW', 'config_mode=OFF'), 'NO   MODE  OFF'),
                             (self.GO.replace('guard_committed_after_new_boot', 'invalid_arm_marker'),
                              'NO   GUARD invalid_arm_marker'),
                             (self.GO.replace('absent', 'present'), 'NO   STOP  present')):
            result = self.verdict(text).splitlines()
            self.assertIn(expect, result)
            self.assertEqual(result[-1], 'NO-GO')
        empty = self.verdict('').splitlines()
        self.assertEqual(empty[-1], 'NO-GO')
        self.assertEqual(sum(l.startswith('NO ') for l in empty), 9)

    def test_invalid_reboot_receipt_does_not_claim_a_new_boot(self):
        self.prepare_logs()
        (self.usb / 'reboot-request.txt').write_text('boot_id=broken\n')
        result = self.menu('2\n0\n')
        self.assertIn('reboot_check=unavailable', result.stdout)
        self.assertNotIn('reboot_check=new_boot_observed', result.stdout)

    def test_status_report_write_error_is_not_reported_as_saved(self):
        import resource
        import signal
        self.prepare_logs()
        report = self.usb / 'startup-result.txt'
        report.write_text('previous parked check\n')

        def disallow_file_growth():
            # A real kernel write error, including when this test runs as root.
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))

        result = self.menu('2\n0\n', preexec_fn=disallow_file_growth)
        self.assertIn('Startup check could not be saved', result.stdout)
        self.assertNotIn('Startup check saved:', result.stdout)
        self.assertEqual(report.read_text(), 'previous parked check\n')
        self.assertFalse(list(self.usb.glob('startup-result.??????')))

    def test_status_is_read_only_and_failure_remains_visible(self):
        self.prepare_logs()
        before = self.fixture.autostart.read_bytes()
        result = self.menu('2\n0\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('capture_active=unavailable', result.stdout)
        self.assertFalse((self.logs / 'capture.stop').exists())
        self.assertEqual(self.fixture.autostart.read_bytes(), before)
        self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_finish_exports_to_the_launching_usb_and_records_outcome(self):
        self.prepare_logs()
        self.assertTrue((self.root / 'proc/mounts').is_symlink())
        result = self.menu('3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = self.assert_export()
        self.assertIn('finish_exit=0', report)
        self.assertIn('export_exit=0', report)
        self.assertTrue((self.logs / 'capture.stop').is_dir())

    def test_regular_mount_table_remains_compatible(self):
        mounts_alias = self.root / 'proc/mounts'
        mounts_alias.unlink()
        mounts_alias.write_bytes(self.mounts.read_bytes())
        self.prepare_logs()
        result = self.menu('3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('export_exit=0', self.assert_export())

    def test_finish_saves_startup_diagnostics_in_usb_report(self):
        self.prepare_logs()
        result = self.menu('3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = self.assert_export()
        self.assertIn('diagnostics_scope=current_recovery_boot', report)
        self.assertIn('diagnostic_schema=1', report)
        self.assertIn('diagnostics_exit=', report)

    def test_diagnostic_failure_preserves_partial_facts_and_raw_export(self):
        self.prepare_logs()
        # Authored failure endpoint tests only menu error propagation, not the
        # real helper's collection. A partial diagnostic cannot veto export.
        (self.usb / 'startup_diagnostics.sh').write_text(
            '#!/bin/sh\nprintf "diagnostic_schema=1\\npartial_fact=retained\\n"\nexit 7\n')
        result = self.menu('3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = self.assert_export()
        self.assertIn('partial_fact=retained', report)
        self.assertIn('diagnostics_exit=7', report)
        self.assertIn('finish_exit=0', report)
        self.assertIn('export_exit=0', report)

    def test_missing_diagnostic_helper_still_exports_retained_logs(self):
        self.prepare_logs()
        helper = self.usb / 'startup_diagnostics.sh'
        if helper.exists():
            helper.unlink()
        result = self.menu('3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = self.assert_export()
        self.assertRegex(report, r'diagnostics_exit=[1-9][0-9]*')
        self.assertIn('export_exit=0', report)

    def test_diagnostic_snapshot_precedes_capture_stop(self):
        self.prepare_logs()
        # An authored endpoint witnesses ordering around the real finish helper.
        (self.usb / 'startup_diagnostics.sh').write_text(
            '#!/bin/sh\n'
            'if [ -d "$MX5DR_FIXTURE_ROOT/data_persist/mx5-aa-dr/logs/capture.stop" ]; '
            'then echo snapshot=after_stop; else echo snapshot=before_stop; fi\n')
        result = self.menu('3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = self.assert_export()
        self.assertIn('snapshot=before_stop', report)
        self.assertNotIn('snapshot=after_stop', report)
        self.assertTrue((self.logs / 'capture.stop').is_dir())

    def test_status_failure_does_not_block_finish_and_export(self):
        self.prepare_logs()
        result = self.menu('2\n3\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assert_export()

    def previous_boot_retrieval(self, acknowledged):
        self.prepare_logs(acknowledged=acknowledged)
        rows = [dict(kind='boot', boot_id=BOOT.strip(), mono_ns=1000000000, mode=4),
                dict(kind='position', mono_ns=99000000000, mode=1),
                dict(kind='motion_batch', schema=1, epoch=1, events=[[1, 1, 99000000000, 1, 0, 0, 0, 0, 1, 0]])]
        payload = ''.join(json.dumps(row, separators=(',', ':')) + '\n' for row in rows).encode()
        (self.logs / 'trace.0.jsonl').write_bytes(payload)
        (self.base / 'guard/last-boot').write_text(BOOT)
        (self.root / 'proc/sys/kernel/random/boot_id').write_text(BOOT.replace('12345678', '87654321'))
        (self.root / 'proc/uptime').write_text('2.00 1.00\n')
        before = self.fixture.autostart.read_bytes()
        # No menu2/arm/install command is issued on return; AA occupied the
        # single USB port during the authored previous-boot recording.
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)  # Current-boot finish is unproved.
        report = (self.usb / 'trial-result.txt').read_text()
        self.assertIn('status_scope=current_boot', report)
        self.assertIn('export_scope=all_retained_boots', report)
        self.assertIn('retained_runtime_last_boot=previous_boot boot_id=' + BOOT.strip(), report)
        self.assertIn('position_records=1 motion_batches=1', report)
        self.assertIn('status_exit=1', report)
        self.assertIn('finish_exit=1', report)
        self.assertIn('export_exit=0', report)
        self.assertIn('Current status is not a verdict on the retained trial', result.stdout)
        archive, = self.usb.glob('mx5dr-logs-*.tar')
        self.assertEqual(archive.with_suffix('.tar.sha256').read_text().split()[0],
                         hashlib.sha256(archive.read_bytes()).hexdigest())
        with tarfile.open(archive) as tar:
            member, = [m for m in tar.getmembers()
                       if m.name.endswith('/mx5-aa-dr/logs/trace.0.jsonl')]
            self.assertEqual(tar.extractfile(member).read(), payload)
        self.assertEqual((self.logs / 'trace.0.jsonl').read_bytes(), payload)
        self.assertFalse((self.base / 'guard/arm').exists())
        self.assertEqual(self.fixture.autostart.read_bytes(), before)

    def test_single_port_reboot_return_exports_without_previous_ack(self):
        self.previous_boot_retrieval(acknowledged=False)

    def test_single_port_reboot_return_exports_without_accepting_old_ack(self):
        self.previous_boot_retrieval(acknowledged=True)

    def test_unconfirmed_finish_still_exports_and_retains_failure(self):
        self.prepare_logs(acknowledged=False)
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        report = self.assert_export()
        self.assertIn('finish_exit=1', report)
        self.assertIn('export_exit=0', report)
        self.assertIn('Freeze not confirmed', report)

    def test_missing_usb_mount_does_not_freeze_or_fill_cmu_tmp(self):
        self.prepare_logs()
        self.mounts.write_text('rootfs / rootfs rw 0 0\n')
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('mounted USB', result.stdout + result.stderr)
        self.assertFalse((self.logs / 'capture.stop').exists())
        self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_hidden_usb_mount_does_not_stop_capture(self):
        self.prepare_logs()
        self.mounts.write_text(
            f'/dev/sdb1 {self.usb} vfat rw 0 0\n'
            f'tmpfs {self.usb} tmpfs rw 0 0\n')
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('mounted USB', result.stdout + result.stderr)
        self.assertFalse((self.logs / 'capture.stop').exists())

    def test_other_mount_does_not_make_the_launching_usb_mounted(self):
        self.prepare_logs()
        for row in (f'/dev/sda1 {self.usb.parent}/sda1 vfat rw 0 0\n',
                    f'/dev/sdb1 {self.usb}/nested vfat rw 0 0\n',
                    f'/dev/mmcblk0p1 {self.usb} ext4 rw 0 0\n'):
            with self.subTest(mount=row):
                self.mounts.write_text(row)
                result = self.menu('3\n')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('mounted USB', result.stdout + result.stderr)
                self.assertFalse((self.logs / 'capture.stop').exists())
                self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_missing_mount_table_does_not_freeze_or_export(self):
        self.prepare_logs()
        self.mounts.unlink()  # The real /proc/mounts alias is now dangling.
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.logs / 'capture.stop').exists())
        self.assertFalse(list(self.usb.glob('mx5dr-logs-*')))

    def test_kernel_alias_exception_does_not_allow_a_symlink_usb_report(self):
        self.prepare_logs()
        sentinel = self.root / 'untouched-report'
        sentinel.write_bytes(b'preserve original report target\n')
        (self.usb / 'trial-result.txt').symlink_to(sentinel)
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Not a regular non-symlink file: ' + str(self.usb / 'trial-result.txt'),
                      result.stderr)
        self.assertEqual(sentinel.read_bytes(), b'preserve original report target\n')

    def test_export_after_remove_preserves_raw_logs_without_reinstall_or_rearm(self):
        self.prepare_logs(acknowledged=False)
        removed = self.menu('4\n')
        self.assertEqual(removed.returncode, 0, removed.stdout + removed.stderr)
        self.assertIn('mode=OFF', (self.base / 'mx5dr.conf').read_text())
        before = {path: path.read_bytes() for path in
                  (self.fixture.autostart, self.fixture.sm,
                   self.root / 'jci/sm/sm_WCP.conf', self.base / 'mx5dr.conf')}
        result = self.menu('3\n')  # No install, arm or diagnostic command.
        self.assertNotEqual(result.returncode, 0)  # Current-boot finish unproved.
        self.assertIn('export_exit=0', result.stdout)
        report = self.assert_export()
        self.assertIn('status_exit=1', report)
        self.assertIn('finish_exit=1', report)
        self.assertIn('export_scope=all_retained_boots', report)
        archive, = self.usb.glob('mx5dr-logs-*.tar')
        with tarfile.open(archive) as tar:
            member, = [m for m in tar.getmembers()
                       if m.name.endswith('/mx5-aa-dr/mx5dr.conf')]
            self.assertEqual(tar.extractfile(member).read(),
                             before[self.base / 'mx5dr.conf'])
        for path, contents in before.items():
            self.assertEqual(path.read_bytes(), contents)
        self.assertFalse((self.base / 'guard/arm').exists())
        self.assertFalse((self.base / 'pending').exists())
        self.assertFalse((self.base / 'installed.txt').exists())

    def test_export_failure_is_visible_and_logged(self):
        self.prepare_logs()
        # Simulate an export I/O failure, not a missing optional input. Missing
        # config is now valid evidence that the expanded archive must retain.
        (self.usb / 'export_logs.sh').write_text(
            '#!/bin/sh\necho "Export failed: authored I/O failure" >&2\nexit 1\n')
        result = self.menu('3\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.usb / 'trial-result.txt').is_file())
        report = (self.usb / 'trial-result.txt').read_text()
        self.assertIn('finish_exit=0', report)
        self.assertIn('export_exit=1', report)
        self.assertIn('Export failed', report)

    def test_remove_keeps_original_settings_and_logs(self):
        self.prepare_logs()
        result = self.menu('4\n')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.fixture.autostart.read_bytes(),
                         self.fixture.original_autostart)
        self.assertTrue((self.logs / 'trace.0.jsonl').exists())


if __name__ == '__main__':
    unittest.main()
