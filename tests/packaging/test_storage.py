"""Storage handling with real scripts/files and authored df readings."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch

import test_synthetic_install


class StorageTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_synthetic_install.SyntheticPackagingTests()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.commands = Path(self.fixture.tmp.name) / 'storage-commands'
        self.commands.mkdir()
        self.base = self.fixture.root / 'data_persist/mx5-aa-dr'

    def free_space(self, kib):
        df = self.commands / 'df'
        df.write_text('#!/bin/sh\n'
                      "printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\\n'\n"
                      f"printf '/dev/fixture 131072 65536 {kib} 50%% /data_persist\\n'\n")
        df.chmod(0o755)
        return patch.dict(os.environ, PATH=str(self.commands) + os.pathsep + os.environ['PATH'])

    def test_low_space_install_does_not_modify_startup_or_create_backups(self):
        with self.free_space(4096):
            r = self.fixture.run_script('install.sh', ok=False)
        self.assertIn('Insufficient persistent space', r.stderr)
        self.assertEqual(self.fixture.autostart.read_bytes(), self.fixture.original_autostart)
        self.assertFalse(self.base.exists())

    def test_install_includes_remaining_log_capacity_in_its_budget(self):
        with self.free_space(32 * 1024):
            r = self.fixture.run_script('install.sh', ok=False)
        self.assertIn('required_kib=', r.stdout)
        self.assertFalse(self.base.exists())

    def test_existing_bounded_logs_are_reused_without_demanding_twice_the_space(self):
        logs = self.base / 'logs'
        logs.mkdir(parents=True)
        for stream, count, size in (('trace', 3, 8 * 1024 * 1024),
                                    ('collector', 2, 1024 * 1024)):
            for i in range(count):
                with (logs / f'{stream}.{i}.jsonl').open('wb') as f:
                    f.truncate(size)
        with self.free_space(16 * 1024):
            r = self.fixture.run_script('install.sh')
        self.assertIn('remaining_log_kib=0', r.stdout)
        self.assertEqual((logs / 'trace.0.jsonl').stat().st_size, 8 * 1024 * 1024)

    def test_export_cannot_accumulate_archives_on_persistent_storage(self):
        self.fixture.run_script('install.sh')
        r = self.fixture.run_script('export_logs.sh', str(self.base), ok=False)
        self.assertIn('outside persistent storage', r.stderr)
        self.assertFalse(list(self.base.glob('mx5dr-logs-*')))
