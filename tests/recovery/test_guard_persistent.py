#!/usr/bin/env python3
"""Persistent BETA guard: per-boot selection with the automatic reset fail-safe.

Host fixtures only (authored files, no OEM binary is executed). The same
harness runs the guard under ARM/QEMU through GUARD_CXX/GUARD_RUNNER.
"""
import hashlib, os, pathlib, shlex, signal, subprocess, sys, tempfile, time, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_guard import HERE, BASE, CFG, TOKEN, TAP_TOKEN, LDS_TOKEN  # noqa: E402

BOOTS = ['%08x-1234-4234-8234-0123456789ab' % (0x10000000 + n) for n in range(40)]
REPORTS = ['thread_info.out', 'ps_info.out', 'top_info.out', 'meminfo.out',
           'free_info.out', 'df_info.out', 'dmesg.out']


def build(extra):
    out = tempfile.TemporaryDirectory()
    exe = pathlib.Path(out.name) / 'guard'
    subprocess.run(shlex.split(os.environ.get('GUARD_CXX', 'g++')) +
                   shlex.split(os.environ.get('GUARD_ARCH_FLAGS', '')) +
                   ['-std=c++11', '-Wall', '-Wextra', '-Werror', '-DMX5DR_GUARD_TESTING'] + extra +
                   [str(HERE / 'src/guard/guard.cpp'), str(HERE / 'src/runtime/sha256.cpp'), '-o', str(exe)],
                   check=True)
    return out, shlex.split(os.environ.get('GUARD_RUNNER', '')) + [str(exe)]


class Fixture(unittest.TestCase):
    RULE = []

    @classmethod
    def setUpClass(cls):
        cls.build, cls.command = build(cls.RULE)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.env = dict(os.environ, MX5DR_GUARD_ROOT=str(self.root))
        (self.root / '.mx5dr-fixture').touch()
        for p in [BASE + '/guard', BASE + '/logs', 'jci/sm', 'proc/sys/kernel/random', 'tmp', 'data']:
            (self.root / p).mkdir(parents=True, exist_ok=True)
        for p in [BASE, BASE + '/guard']:
            (self.root / p).chmod(0o755)
        # The collector account owns logs; the guard treats it as untrusted.
        (self.root / BASE / 'logs').chmod(0o750)
        self.put(BASE + '/libmx5dr.so', b'author-fixture-payload')
        self.put(BASE + '/libmx5dr-vimtap.so', b'author-fixture-tap')
        self.put(BASE + '/libmx5dr-ldstap.so', b'author-fixture-lds-tap')
        self.put(BASE + '/mx5dr.conf', b'mode=BETA\nmax_log_bytes=41943040\nmax_log_files=3\nsample_ms=1000\n')
        for p in ['jci/sm/sm.conf', 'jci/sm/sm_WCP.conf']:
            self.put(p, CFG.encode())
        trial = (CFG.replace('/data_persist/touch.so', TOKEN + ':/data_persist/touch.so')
                 .replace('/data_persist/vbs.so', TAP_TOKEN + ':/data_persist/vbs.so')
                 .replace('/data_persist/lds-existing.so', LDS_TOKEN + ':/data_persist/lds-existing.so')).encode()
        for p in ['normal.trial', 'wcp.trial']:
            self.put(BASE + '/guard/' + p, trial)
        for p in ['normal.source.sha256', 'wcp.source.sha256']:
            self.put(BASE + '/guard/' + p, (hashlib.sha256(CFG.encode()).hexdigest() + '\n').encode())
        # An old reset report from before installation is the baseline, not a failure.
        self.report('dmesg.out', b'[ 1451.0] old reset\n')
        self.boot_index = 0
        self.boot(0)

    def put(self, p, b, mode=0o600):
        x = self.root / p
        x.write_bytes(b)
        x.chmod(mode)

    def report(self, name, data):
        (self.root / 'data' / name).write_bytes(data)

    def boot(self, n):
        self.boot_index = n
        self.put('proc/sys/kernel/random/boot_id', (BOOTS[n] + '\n').encode())

    def call(self, *a, env=None):
        return subprocess.run(self.command + list(a), env=env or self.env, text=True, capture_output=True)

    def enable(self):
        r = self.call('enable')
        self.assertEqual(r.returncode, 0, r.stderr)

    def select(self, name='sm.conf', env=None):
        return self.call('select', '/jci/sm/' + name, env=env)

    def state(self):
        text = (self.root / BASE / 'guard/persist-state').read_text()
        lines = text.splitlines()
        self.assertEqual(lines[0], 'mx5dr-persist-state-v2')
        return dict(line.split('=', 1) for line in lines[1:])

    def healthy(self, boot, uptime='130', raw=None):
        path = self.root / BASE / 'logs/healthy'
        if path.exists() or path.is_symlink():
            path.unlink()
        path.write_bytes(raw if raw is not None else ('boot_id=%s\nuptime_s=%s\n' % (BOOTS[boot], uptime)).encode())

    def product_boot(self, n, expect=True, name='sm.conf', confirm=True):
        """New Linux boot n; returns the selection result. A selected boot is
        confirmed as the autostart block does 90 s later, unless confirm=False
        (a boot shorter than the confirmation delay)."""
        self.boot(n)
        r = self.select(name)
        if expect:
            self.assertEqual(r.returncode, 0, r.stderr)
            path = pathlib.Path(r.stdout.strip())
            self.assertRegex(r.stdout, r'/tmp/mx5dr-trial-[A-Za-z0-9]{6}/sm\.conf\n$')
            self.assertEqual(path.read_bytes(), (self.root / BASE / 'guard/normal.trial').read_bytes())
            if confirm:
                self.sm_process(r.stdout.strip())
                c = self.call('confirm')
                self.assertEqual(c.returncode, 0, c.stderr)
                self.assertEqual(self.state()['confirmed_boot'], BOOTS[n])
        else:
            self.assertNotEqual(r.returncode, 0)
            self.assertEqual(r.stdout, '')
        return r

    def sm_process(self, trial, pid='266'):
        """Authored /proc entry in the vehicle ps form; never an OEM process."""
        proc = self.root / 'proc' / pid
        proc.mkdir(parents=True, exist_ok=True)
        sm_path = trial[len(str(self.root)):] if trial.startswith(str(self.root)) else trial
        (proc / 'cmdline').write_bytes(b'\0'.join([b'/jci/sm/sm', b'-f', sm_path.encode(), b'-e',
                                                   b'/tmp/smevents.txt']) + b'\0')

    def reset_during(self, tag):
        # The SM rewrites its reports just before it stops the watchdog.
        for name in REPORTS:
            self.report(name, ('%s %s\n' % (name, tag)).encode())


class PersistentRuleOff(Fixture):
    def test_manifest_records_rule_and_bindings(self):
        self.enable()
        lines = (self.root / BASE / 'guard/persist').read_text().splitlines()
        self.assertEqual(lines[:4], ['mx5dr-persist-v1', 'mode=BETA', 'healthy_rule=off', 'mx5dr-one-boot-v3'])
        self.assertEqual(len(lines), 12)
        st = self.state()
        self.assertEqual(st['enabled_boot'], BOOTS[0])
        self.assertEqual((st['fail_count'], st['attempt_boot'], st['tripped']), ('0', 'none', 'no'))
        self.assertFalse((self.root / BASE / 'guard/arm').exists())

    def test_enabling_boot_never_selects_and_every_later_boot_does(self):
        self.enable()
        self.product_boot(0, expect=False)  # SM restart in the installing boot
        for n in range(1, 8):
            self.product_boot(n)
            st = self.state()
            self.assertEqual((st['attempt_boot'], st['fail_count'], st['tripped']), (BOOTS[n], '0', 'no'))
            self.assertEqual(st['previous'], 'none' if n == 1 else 'confirmed')
            self.assertEqual(st['attempts_since_healthy'], str(n))
            self.assertEqual((self.root / BASE / 'guard/last-boot').read_text(), BOOTS[n] + '\n')
        self.assertFalse((self.root / BASE / 'guard/consumed').exists())

    def test_same_boot_double_select_and_wcp(self):
        self.enable()
        self.product_boot(1, name='sm_WCP.conf')
        for name in ('sm.conf', 'sm_WCP.conf'):
            self.assertNotEqual(self.select(name).returncode, 0)
        # Even a lost last-boot marker cannot reopen this boot: the attempt record fences it.
        (self.root / BASE / 'guard/last-boot').unlink()
        r = self.select()
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(r.stdout, '')

    def test_concurrent_selection_at_most_one(self):
        self.enable()
        self.boot(1)
        procs = [subprocess.Popen(self.command + ['select', '/jci/sm/sm.conf'], env=self.env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(8)]
        for p in procs:
            p.communicate()
        self.assertEqual(sum(p.returncode == 0 for p in procs), 1)

    def test_one_failure_then_good_boot_resets_counter(self):
        self.enable()
        self.product_boot(1)
        self.reset_during('boot1')
        self.product_boot(2)
        st = self.state()
        self.assertEqual((st['previous'], st['fail_count'], st['tripped']), ('failed_reset', '1', 'no'))
        self.product_boot(3)  # boot 2 ended without a new report
        st = self.state()
        self.assertEqual((st['previous'], st['fail_count']), ('confirmed', '0'))
        self.reset_during('boot3')
        self.product_boot(4)
        self.assertEqual(self.state()['fail_count'], '1')

    def test_two_consecutive_failures_trip_and_stay_tripped(self):
        self.enable()
        self.product_boot(1)
        self.reset_during('boot1')
        self.product_boot(2)
        self.reset_during('boot2')
        self.product_boot(3, expect=False)
        st = self.state()
        self.assertEqual((st['tripped'], st['fail_count'], st['previous'], st['attempt_boot']),
                         ('reset_reports', '2', 'failed_reset', 'none'))
        trip = (self.root / BASE / 'guard/persist-state').read_bytes()
        for n in range(4, 9):
            self.product_boot(n, expect=False)
            self.assertEqual((self.root / BASE / 'guard/persist-state').read_bytes(), trip)
        # Evidence stays: the reports and the persistent manifest are untouched.
        self.assertTrue((self.root / BASE / 'guard/persist').is_file())
        self.assertIn(b'boot2', (self.root / 'data/dmesg.out').read_bytes())
        self.assertEqual(list((self.root / 'tmp').glob('mx5dr-trial-*/sm.conf')).__len__(), 2)
        # Owner re-enable (menu 1) clears the trip; the next boot is product again.
        self.enable()
        st = self.state()
        self.assertEqual((st['tripped'], st['fail_count'], st['enabled_boot']), ('no', '0', BOOTS[8]))
        self.product_boot(8, expect=False)
        self.product_boot(9)
        self.assertEqual(self.state()['previous'], 'none')

    def test_reset_detection_does_not_use_clocks(self):
        self.enable()
        self.product_boot(1)
        path = self.root / 'data/dmesg.out'
        st = path.stat()
        # Same size and same (1970) mtime: only the content changed.
        data = path.read_bytes()
        path.write_bytes(bytes(reversed(data)))
        os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))
        self.product_boot(2)
        self.assertEqual(self.state()['previous'], 'failed_reset')
        # A touched mtime with identical content still differs (opaque equality).
        self.product_boot(3)
        self.assertEqual(self.state()['previous'], 'confirmed')
        os.utime(path, ns=(0, 5_000_000_000))
        self.product_boot(4)
        self.assertEqual(self.state()['previous'], 'failed_reset')
        self.assertEqual(self.state()['fail_count'], '1')
        self.product_boot(5)
        self.assertEqual(self.state()['fail_count'], '0')
        # Fresh counters (menu 1 in boot 5); a new report name and a removed
        # report both count.
        self.enable()
        self.product_boot(6)
        self.report('extra_info.out', b'x')
        self.product_boot(7)
        self.assertEqual(self.state()['previous'], 'failed_reset')
        (self.root / 'data/extra_info.out').unlink()
        self.product_boot(8, expect=False)
        self.assertEqual(self.state()['tripped'], 'reset_reports')

    def test_report_names_other_than_out_and_unchanged_reports_are_ignored(self):
        self.enable()
        self.product_boot(1)
        (self.root / 'data/natp').mkdir()
        self.report('notes.txt', b'unrelated')
        self.product_boot(2)
        self.assertEqual(self.state()['previous'], 'confirmed')

    def test_report_alias_chain_and_unusable_report_directory(self):
        (self.root / 'tmp/mnt').mkdir()
        (self.root / 'data').rename(self.root / 'tmp/mnt/data')
        (self.root / 'data').symlink_to('/mnt/data')
        (self.root / 'mnt').symlink_to('/tmp/mnt')
        self.enable()
        self.product_boot(1)
        (self.root / 'tmp/mnt/data/meminfo.out').write_bytes(b'reset')
        self.product_boot(2)
        self.assertEqual(self.state()['previous'], 'failed_reset')
        # An unexpected alias target or a missing directory declines (stock), state unchanged.
        before = (self.root / BASE / 'guard/persist-state').read_bytes()
        (self.root / 'data').unlink()
        (self.root / 'data').symlink_to('/tmp/elsewhere')
        self.product_boot(3, expect=False)
        (self.root / 'data').unlink()
        self.product_boot(4, expect=False)
        self.assertEqual((self.root / BASE / 'guard/persist-state').read_bytes(), before)
        (self.root / 'data').symlink_to('mnt/data')
        self.product_boot(5)

    def test_too_many_reports_decline(self):
        self.enable()
        for n in range(64):  # plus the existing dmesg.out: 65 reports
            self.report('r%02d.out' % n, b'x')
        self.product_boot(1, expect=False)
        (self.root / 'data/r63.out').unlink()
        self.product_boot(2)

    def test_report_symlink_is_fingerprinted_not_followed(self):
        self.enable()
        target = self.root / 'outside'
        target.write_bytes(b'a')
        (self.root / 'data/ps_info.out').symlink_to(target)
        self.product_boot(1)
        target.write_bytes(b'changed')  # not followed: no failure
        self.product_boot(2)
        self.assertEqual(self.state()['previous'], 'confirmed')

    def test_runtime_disable_marker_trips(self):
        self.enable()
        self.product_boot(1)
        (self.root / BASE / 'logs/disable-next-start').write_text('x')
        self.product_boot(2, expect=False)
        st = self.state()
        self.assertEqual((st['tripped'], st['previous'], st['fail_count']), ('runtime_disabled', 'confirmed', '0'))
        self.product_boot(3, expect=False)

    def test_stale_capture_freeze_is_cleared_only_when_selected(self):
        self.enable()
        logs = self.root / BASE / 'logs'
        self.product_boot(1)
        (logs / 'capture.stop').mkdir()
        (logs / 'capture.done').write_text(BOOTS[1] + '\n')
        self.product_boot(2)
        self.assertFalse((logs / 'capture.stop').exists())
        self.assertFalse((logs / 'capture.done').exists())
        # Nonempty or symlinked markers are never recursed into or followed.
        (logs / 'capture.stop').mkdir()
        (logs / 'capture.stop/keep').write_text('k')
        outside = self.root / 'outside'
        outside.write_text('o')
        (logs / 'capture.done').symlink_to(outside)
        self.product_boot(3)
        self.assertTrue((logs / 'capture.stop/keep').exists())
        self.assertTrue((logs / 'capture.done').is_symlink())
        self.assertEqual(outside.read_text(), 'o')

    def test_healthy_marker_is_read_but_rule_off_never_fails_boot(self):
        self.enable()
        for n in range(1, 7):
            self.product_boot(n)  # no healthy marker at all
        st = self.state()
        self.assertEqual((st['fail_count'], st['tripped'], st['healthy_previous']), ('0', 'no', 'no'))
        self.assertEqual(st['attempts_since_healthy'], '6')
        self.healthy(6)
        self.product_boot(7)
        st = self.state()
        self.assertEqual((st['healthy_previous'], st['previous'], st['attempts_since_healthy']), ('yes', 'healthy', '1'))

    def test_healthy_marker_validation(self):
        self.enable()
        self.product_boot(1)
        bad = {
            'wrong_boot': ('boot_id=%s\nuptime_s=130\n' % BOOTS[5]).encode(),
            'short_uptime': ('boot_id=%s\nuptime_s=119\n' % BOOTS[1]).encode(),
            'leading_zero': ('boot_id=%s\nuptime_s=0130\n' % BOOTS[1]).encode(),
            'no_newline': ('boot_id=%s\nuptime_s=130' % BOOTS[1]).encode(),
            'extra_line': ('boot_id=%s\nuptime_s=130\nx=1\n' % BOOTS[1]).encode(),
            'uppercase': ('boot_id=%s\nuptime_s=130\n' % BOOTS[1].upper()).encode(),
            'oversized': ('boot_id=%s\nuptime_s=130\n' % BOOTS[1]).encode() + b' ' * 200,
            'long_number': ('boot_id=%s\nuptime_s=12345678901\n' % BOOTS[1]).encode(),
            'nul': ('boot_id=%s\nuptime_s=13\x000\n' % BOOTS[1]).encode(),
        }
        n = 1
        for label, raw in bad.items():
            with self.subTest(label=label):
                self.healthy(n, raw=raw)
                n += 1
                self.product_boot(n)
                self.assertEqual(self.state()['healthy_previous'], 'no')
        # Symlink to a valid marker: never followed.
        good = self.root / 'good'
        good.write_text('boot_id=%s\nuptime_s=130\n' % BOOTS[n])
        marker = self.root / BASE / 'logs/healthy'
        marker.unlink()
        marker.symlink_to(good)
        n += 1
        self.product_boot(n)
        self.assertEqual(self.state()['healthy_previous'], 'no')
        # A FIFO does not block the guard.
        marker.unlink()
        os.mkfifo(marker)
        n += 1
        self.product_boot(n)
        self.assertEqual(self.state()['healthy_previous'], 'no')
        marker.unlink()
        # A symlinked logs directory is not followed either.
        logs = self.root / BASE / 'logs'
        logs.rename(self.root / 'logs-real')
        (self.root / 'logs-real/healthy').write_text('boot_id=%s\nuptime_s=130\n' % BOOTS[n])
        logs.symlink_to(self.root / 'logs-real')
        n += 1
        self.product_boot(n)
        self.assertEqual(self.state()['healthy_previous'], 'no')
        logs.unlink()
        (self.root / 'logs-real').rename(logs)
        self.healthy(n)
        n += 1
        self.product_boot(n)
        self.assertEqual(self.state()['healthy_previous'], 'yes')

    def test_bindings_invalidate_without_state_change(self):
        self.enable()
        self.product_boot(1)
        before = (self.root / BASE / 'guard/persist-state').read_bytes()
        changes = [
            (BASE + '/libmx5dr.so', b'changed payload'),
            (BASE + '/libmx5dr-vimtap.so', b'changed tap'),
            (BASE + '/libmx5dr-ldstap.so', b'changed lds'),
            ('jci/sm/sm.conf', (CFG + '<!-- touch edit -->').encode()),
            (BASE + '/guard/wcp.trial', b'other template'),
            (BASE + '/mx5dr.conf', b'mode=BETA\nmax_log_bytes=65536\nmax_log_files=3\nsample_ms=1000\n'),
        ]
        n = 2
        for path, data in changes:
            with self.subTest(path=path):
                saved = (self.root / path).read_bytes()
                self.put(path, data)
                self.product_boot(n, expect=False)
                self.assertEqual((self.root / BASE / 'guard/persist-state').read_bytes(), before)
                self.put(path, saved)
                n += 1
        self.product_boot(n)

    def test_mode_other_than_beta_never_selects_or_enables(self):
        for conf in (b'mode=SHADOW\n', b'mode=OBSERVE\n', b'mode=BETAX\n'):
            with self.subTest(conf=conf):
                self.put(BASE + '/mx5dr.conf', conf + b'max_log_bytes=41943040\n')
                self.assertNotEqual(self.call('enable').returncode, 0)
                self.assertFalse((self.root / BASE / 'guard/persist').exists())

    def test_persistent_and_one_boot_are_exclusive(self):
        self.enable()
        self.put(BASE + '/guard/arm', b'anything')
        self.product_boot(1, expect=False)
        self.assertNotEqual(self.call('enable').returncode, 0)
        (self.root / BASE / 'guard/arm').unlink()
        self.product_boot(2)

    def test_damaged_symlinked_or_loose_files_decline(self):
        self.enable()
        self.product_boot(1)
        state = self.root / BASE / 'guard/persist-state'
        good = state.read_bytes()
        bad_states = [
            good.replace(b'fail_count=0', b'fail_count=00'),
            good.replace(b'fail_count=0', b'fail_count=2'),
            good.replace(b'tripped=no', b'tripped=maybe'),
            good.replace(b'previous=none', b'previous=none\nextra=1'),
            good + b'\n',
            good.rstrip(b'\n'),
            good.replace(b'mx5dr-persist-state-v2', b'mx5dr-persist-state-v1'),
        ]
        n = 2
        for raw in bad_states:
            with self.subTest(raw=raw[:60]):
                self.put(BASE + '/guard/persist-state', raw)
                self.product_boot(n, expect=False)
                n += 1
        self.put(BASE + '/guard/persist-state', good)
        for target in ('persist', 'persist-state'):
            with self.subTest(symlink=target):
                path = self.root / BASE / 'guard' / target
                saved = path.read_bytes()
                path.unlink()
                (self.root / 'elsewhere').write_bytes(saved)
                path.symlink_to(self.root / 'elsewhere')
                self.product_boot(n, expect=False)
                path.unlink()
                self.put(BASE + '/guard/' + target, saved)
                (self.root / BASE / 'guard' / target).chmod(0o666)
                self.product_boot(n + 1, expect=False)
                (self.root / BASE / 'guard' / target).chmod(0o600)
                (self.root / 'elsewhere').unlink()
                n += 2
        persist = self.root / BASE / 'guard/persist'
        saved = persist.read_bytes()
        for raw in (saved.replace(b'healthy_rule=off', b'healthy_rule=on'),
                    saved.replace(b'mode=BETA', b'mode=SHADOW'),
                    saved.replace(b'mx5dr-persist-v1', b'mx5dr-persist-v0')):
            self.put(BASE + '/guard/persist', raw)
            self.product_boot(n, expect=False)
            n += 1
        self.put(BASE + '/guard/persist', saved)
        self.product_boot(n)

    def test_power_loss_ordering_never_publishes(self):
        self.enable()
        self.product_boot(1)
        n = 2
        for stage in ('trial-dir', 'persist-state', 'last-boot'):
            with self.subTest(stage=stage):
                before = (self.root / BASE / 'guard/persist-state').read_bytes()
                self.boot(n)
                r = self.select(env=dict(self.env, MX5DR_GUARD_FAIL_FSYNC=stage))
                self.assertNotEqual(r.returncode, 0)
                self.assertEqual(r.stdout, '')
                self.assertEqual(list((self.root / 'tmp').glob('mx5dr-trial-*/sm.conf')).__len__(), 1)
                # last-boot is never this boot: the old marker, or revoked.
                marker = self.root / BASE / 'guard/last-boot'
                self.assertEqual(marker.read_text() if marker.exists() else None,
                                 None if stage == 'last-boot' else BOOTS[1] + '\n')
                st = self.state()
                if stage == 'trial-dir':
                    self.assertEqual((self.root / BASE / 'guard/persist-state').read_bytes(), before)
                else:
                    # The attempt record may be durable; it only fences this boot.
                    self.assertEqual(st['attempt_boot'], BOOTS[n])
                    self.assertNotEqual(self.select().returncode, 0)
                n += 1
        # The rollback failure is explicit.
        self.boot(n)
        r = self.select(env=dict(self.env, MX5DR_GUARD_FAIL_FSYNC='last-boot,last-boot-cancel'))
        self.assertEqual(r.returncode, 3)
        self.assertIn('unable to confirm durable rollback', r.stderr)
        self.assertFalse((self.root / BASE / 'guard/last-boot').exists())
        # The undelivered attempts never ran the product, but each one counts
        # as unconfirmed (it can never be confirmed): the third trips to stock.
        st = self.state()
        self.assertEqual((st['unconfirmed_count'], st['recent']), ('2', 'CUU'))
        self.product_boot(n + 1, expect=False)
        st = self.state()
        self.assertEqual((st['tripped'], st['unconfirmed_count'], st['recent']), ('unconfirmed', '3', 'CUUU'))

    def test_stale_temporaries_are_removed_under_the_lock(self):
        self.enable()
        guard = self.root / BASE / 'guard'
        stale = ['.persist-state.123', '.last-boot.4567', '.persist.9', '.arm.42', '.last-decision.77']
        keep = ['.persist-state.12a', '.other.123', '.persist-state.', 'persist-state.5', '.persist-state.12345678901']
        for name in stale + keep:
            (guard / name).write_text('x')
            (guard / name).chmod(0o600)
        outside = self.root / 'outside'
        outside.write_text('o')
        (guard / '.persist.55').symlink_to(outside)
        (guard / '.last-boot.66').mkdir()
        self.product_boot(1)
        for name in stale:
            self.assertFalse((guard / name).exists(), name)
        for name in keep:
            self.assertTrue((guard / name).exists(), name)
        self.assertTrue((guard / '.persist.55').is_symlink())
        self.assertEqual(outside.read_text(), 'o')
        self.assertTrue((guard / '.last-boot.66').is_dir())

    def test_pid_collision_with_a_stale_temporary(self):
        """A power cut left .persist-state.<pid> and .last-boot.<pid>; the same
        PID is reused by the next guard (exec keeps the PID)."""
        self.enable()
        self.product_boot(1)
        self.boot(2)
        gate = subprocess.Popen(['sh', '-c', 'echo $$; read go; exec "$@"', 'sh'] + self.command +
                                ['select', '/jci/sm/sm.conf'], env=self.env, text=True,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        pid = gate.stdout.readline().strip()
        guard = self.root / BASE / 'guard'
        for name in ('persist-state', 'last-boot'):
            (guard / ('.%s.%s' % (name, pid))).write_text('stale from a power cut')
            (guard / ('.%s.%s' % (name, pid))).chmod(0o600)
        out, err = gate.communicate('go\n')
        self.assertEqual(gate.returncode, 0, err)
        self.assertRegex(out, r'/tmp/mx5dr-trial-\w{6}/sm\.conf\n$')
        self.assertEqual(self.state()['attempt_boot'], BOOTS[2])
        self.assertEqual(sorted(p.name for p in guard.glob('.*.%s' % pid)), [])

    def test_guard_deadline_ends_a_hung_selection_without_a_path(self):
        self.enable()
        self.product_boot(1)
        before = (self.root / BASE / 'guard/persist-state').read_bytes()
        for n, stage in enumerate(('start', 'reports', 'fsync'), start=2):
            with self.subTest(stage=stage):
                self.boot(n)
                started = time.monotonic()
                r = self.select(env=dict(self.env, MX5DR_GUARD_HANG=stage, MX5DR_GUARD_ALARM='2'))
                self.assertLess(time.monotonic() - started, 20)
                self.assertEqual(r.returncode, -signal.SIGALRM)
                self.assertEqual(r.stdout, '')
                if stage != 'fsync':
                    # (A hung fsync may leave an unpublished /tmp file; SM never sees it.)
                    self.assertEqual(list((self.root / 'tmp').glob('mx5dr-trial-*/sm.conf')).__len__(), 1)
                    self.assertEqual((self.root / BASE / 'guard/persist-state').read_bytes(), before)
        # The next boot still works (a hung fsync may have left a temporary).
        self.product_boot(6)

    # ---- guard-owned confirmation, probation and caps (2026-10-06 G2) ----
    def test_probation_first_attempt_unconfirmed_trips_immediately(self):
        """A deterministic fault without an SM report (SM rejects the trial,
        panic, power cut) is bounded to ONE product boot after menu 1."""
        self.enable()
        self.assertEqual(self.state()['probation'], 'yes')
        self.product_boot(1, confirm=False)
        self.product_boot(2, expect=False)
        st = self.state()
        self.assertEqual((st['tripped'], st['previous'], st['recent']), ('probation', 'unconfirmed', 'U'))
        for n in range(3, 7):
            self.product_boot(n, expect=False)
        self.enable()
        self.product_boot(7)
        self.product_boot(8)
        self.assertEqual(self.state()['probation'], 'no')

    def test_no_report_loop_stops_after_three_unconfirmed_attempts(self):
        self.enable()
        self.product_boot(1)  # probation passed
        selected = []
        for n in range(2, 12):
            r = self.product_boot(n, expect=(n <= 4), confirm=False)
            selected.append(r.returncode == 0)
        self.assertEqual(selected, [True, True, True] + [False] * 7)
        st = self.state()
        self.assertEqual((st['tripped'], st['unconfirmed_count'], st['recent']), ('unconfirmed', '3', 'CUUU'))

    def test_only_a_confirmed_attempt_resets_the_unconfirmed_counter(self):
        self.enable()
        self.product_boot(1)
        self.product_boot(2, confirm=False)
        self.product_boot(3, confirm=False)
        self.product_boot(4)          # judges 3: unconfirmed_count 2
        self.assertEqual(self.state()['unconfirmed_count'], '2')
        self.product_boot(5, confirm=False)  # judges 4 confirmed: 0
        self.assertEqual(self.state()['unconfirmed_count'], '0')
        self.product_boot(6, confirm=False)  # judges 5: 1
        self.reset_during('boot 6')
        self.product_boot(7, confirm=False)  # judges 6: reset, counter kept at 1
        st = self.state()
        self.assertEqual((st['previous'], st['unconfirmed_count'], st['fail_count']), ('failed_reset', '1', '1'))
        self.product_boot(8, confirm=False)  # judges 7: 2; fail_count not reset by unconfirmed
        self.assertEqual((self.state()['unconfirmed_count'], self.state()['fail_count']), ('2', '1'))
        self.product_boot(9, expect=False)  # judges 8: 3
        self.assertEqual(self.state()['tripped'], 'unconfirmed')

    def test_alternating_reset_and_good_boots_hit_the_cumulative_cap(self):
        self.enable()
        self.product_boot(1)
        outcomes = []
        for n in range(2, 8):
            if n % 2 == 0:
                self.reset_during('boot %d' % (n - 1))
            r = self.product_boot(n, expect=(n < 6))
            outcomes.append((n, self.state()['previous'], self.state()['tripped']))
        # R C R C R within 10 attempts: the third reset trips although no two were consecutive.
        self.assertEqual(outcomes[4], (6, 'failed_reset', 'reset_reports_repeated'))
        self.assertEqual(self.state()['recent'], 'RCRCR')

    def test_old_resets_leave_the_ten_attempt_window(self):
        self.enable()
        self.product_boot(1)
        self.reset_during('boot 1')
        for n in range(2, 12):
            self.product_boot(n)       # judges 1 (R), then 2..10 (C)
        self.reset_during('boot 11')
        self.product_boot(12)          # judges 11: R
        self.product_boot(13)          # judges 12: C
        self.reset_during('boot 13')
        self.product_boot(14)          # judges 13: R; the first R left the window
        st = self.state()
        self.assertEqual((st['recent'], st['fail_count'], st['tripped']), ('CCCCCCCRCR', '1', 'no'))

    def test_confirm_rules(self):
        self.enable()
        # Installing boot: nothing to confirm.
        self.assertNotEqual(self.call('confirm').returncode, 0)
        r = self.product_boot(1, confirm=False)
        trial = r.stdout.strip()
        # SM not running with this trial (rejected config / stock SM): rejected.
        self.assertNotEqual(self.call('confirm').returncode, 0)
        self.sm_process(trial.replace('/sm.conf', 'X/sm.conf'), pid='300')
        self.assertNotEqual(self.call('confirm').returncode, 0)
        stock = self.root / 'proc/301'
        stock.mkdir(parents=True)
        (stock / 'cmdline').write_bytes(b'/jci/sm/sm\0-f\0/jci/sm/sm.conf\0-e\0/tmp/smevents.txt\0')
        self.assertNotEqual(self.call('confirm').returncode, 0)
        self.assertEqual(self.state()['confirmed_boot'], 'none')
        # The SM of this boot with the published trial: confirmed, then idempotent.
        self.sm_process(trial)
        self.assertEqual(self.call('confirm').returncode, 0)
        before = (self.root / BASE / 'guard/persist-state').read_bytes()
        self.assertEqual(self.call('confirm').returncode, 0)
        self.assertEqual((self.root / BASE / 'guard/persist-state').read_bytes(), before)
        # A confirm that runs after a reboot (different boot id) is rejected.
        self.boot(2)
        self.assertNotEqual(self.call('confirm').returncode, 0)
        self.assertEqual((self.root / BASE / 'guard/persist-state').read_bytes(), before)
        # A missing commit marker (last-boot) is rejected too.
        r = self.product_boot(2, confirm=False)
        self.sm_process(r.stdout.strip())
        (self.root / BASE / 'guard/last-boot').unlink()
        self.assertNotEqual(self.call('confirm').returncode, 0)
        self.assertEqual(self.state()['confirmed_boot'], 'none')

    def test_confirm_not_run_counts_as_unconfirmed(self):
        """Ignition off 30 s after start: the backgrounded confirm never runs."""
        self.enable()
        self.product_boot(1)
        self.product_boot(2, confirm=False)
        self.product_boot(3)
        st = self.state()
        self.assertEqual((st['previous'], st['unconfirmed_count'], st['recent']), ('unconfirmed', '1', 'CU'))

    def test_trip_and_lost_trial_state_cannot_be_confirmed(self):
        self.enable()
        r = self.product_boot(1)
        self.reset_during('b1')
        self.product_boot(2, confirm=False)
        self.reset_during('b2')
        self.boot(3)
        self.assertNotEqual(self.select().returncode, 0)
        self.assertEqual(self.state()['tripped'], 'reset_reports')
        self.assertNotEqual(self.call('confirm').returncode, 0)

    def test_failed_enable_publication_revokes(self):
        r = self.call('enable', env=dict(self.env, MX5DR_GUARD_FAIL_FSYNC='persist'))
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((self.root / BASE / 'guard/persist').exists())
        r = self.call('enable', env=dict(self.env, MX5DR_GUARD_FAIL_FSYNC='persist,persist-cancel'))
        self.assertEqual(r.returncode, 3)
        self.assertFalse((self.root / BASE / 'guard/persist').exists())
        # Without the manifest nothing selects, and a stale state alone authorizes nothing.
        self.assertTrue((self.root / BASE / 'guard/persist-state').exists())
        self.product_boot(1, expect=False)

    def test_trip_record_power_loss_is_rederived(self):
        self.enable()
        self.product_boot(1)
        self.reset_during('b1')
        self.product_boot(2)
        self.reset_during('b2')
        before = (self.root / BASE / 'guard/persist-state').read_bytes()
        self.boot(3)
        r = self.select(env=dict(self.env, MX5DR_GUARD_FAIL_FSYNC='persist-state'))
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(r.stdout, '')
        # Even if the renamed trip record were lost, the same evidence trips again.
        self.put(BASE + '/guard/persist-state', before)
        self.boot(4)
        r = self.select()
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(r.stdout, '')
        self.assertEqual(self.state()['tripped'], 'reset_reports')
        self.assertEqual(list((self.root / 'tmp').glob('mx5dr-trial-*/sm.conf')).__len__(), 2)


class PersistentRuleOn(Fixture):
    RULE = ['-DMX5DR_GUARD_HEALTHY_RULE=1']

    def test_manifest_records_rule_on(self):
        self.enable()
        self.assertIn('healthy_rule=on\n', (self.root / BASE / 'guard/persist').read_text())

    def test_healthy_sequence(self):
        self.enable()
        for n in range(1, 8):
            if n > 1:
                self.healthy(n - 1)
            self.product_boot(n)
            st = self.state()
            self.assertEqual((st['fail_count'], st['tripped'], st['attempts_since_healthy']), ('0', 'no', '1'))
            self.assertEqual(st['previous'], 'none' if n == 1 else 'healthy')

    def test_boot_loop_without_healthy_trips(self):
        self.enable()
        self.product_boot(1)
        self.product_boot(2)
        self.product_boot(3)
        st = self.state()
        self.assertEqual((st['previous'], st['fail_count']), ('confirmed', '0'))  # 2 attempts so far
        self.product_boot(4)  # boot 3 was the third attempt without healthy
        st = self.state()
        self.assertEqual((st['previous'], st['fail_count'], st['tripped']), ('failed_bootloop', '1', 'no'))
        self.product_boot(5, expect=False)
        st = self.state()
        self.assertEqual((st['tripped'], st['fail_count']), ('boot_loop', '2'))
        self.product_boot(6, expect=False)

    def test_unhealthy_without_failure_keeps_counter_and_healthy_resets(self):
        self.enable()
        self.product_boot(1)
        self.reset_during('b1')
        self.product_boot(2)
        self.assertEqual(self.state()['fail_count'], '1')
        self.product_boot(3)  # boot 2: no reset, no healthy: not a recovery
        st = self.state()
        self.assertEqual((st['previous'], st['fail_count']), ('confirmed', '1'))
        self.healthy(3)
        self.product_boot(4)
        st = self.state()
        self.assertEqual((st['previous'], st['fail_count'], st['attempts_since_healthy']), ('healthy', '0', '1'))

    def test_reset_after_healthy_marker_still_fails(self):
        self.enable()
        self.product_boot(1)
        self.healthy(1)
        self.reset_during('late crash')
        self.product_boot(2)
        st = self.state()
        self.assertEqual((st['previous'], st['healthy_previous'], st['fail_count']), ('failed_reset', 'yes', '1'))

    def test_rule_off_manifest_declines_in_rule_on_build(self):
        self.enable()
        persist = self.root / BASE / 'guard/persist'
        self.put(BASE + '/guard/persist', persist.read_bytes().replace(b'healthy_rule=on', b'healthy_rule=off'))
        self.product_boot(1, expect=False)


if __name__ == '__main__':
    unittest.main()
