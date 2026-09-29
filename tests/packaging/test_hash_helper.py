"""Execute the real streaming hash helper and the tool-less USB fallback."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class HashHelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.build.name) / 'mx5dr-sha256'
        subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        str(REPO / 'src/tools/sha256_main.cpp'),
                        str(REPO / 'src/runtime/sha256.cpp'), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def test_vectors_streaming_and_read_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / 'file with spaces'
            for data in (b'', b'abc', bytes(range(256)) * 1025):
                file.write_bytes(data)
                result = subprocess.run([str(self.exe), str(file)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.split()[0], hashlib.sha256(data).hexdigest())
            for bad in (str(Path(tmp) / 'missing'), tmp):
                result = subprocess.run([str(self.exe), bad], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, '')

    def test_no_sha256sum_and_nonexecutable_usb_helper(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / '.mx5dr-fixture').touch()
            bundle = root / 'USB with spaces'
            bundle.mkdir()
            shutil.copyfile(self.exe, bundle / 'mx5dr-sha256')
            (bundle / 'mx5dr-sha256').chmod(0o644)
            data = bundle / 'payload'
            data.write_bytes(b'no native sha256sum, no executable USB files')
            manifest = bundle / 'SHA256SUMS'
            manifest.write_text(hashlib.sha256(data.read_bytes()).hexdigest() + '  payload\n')
            commands = root / 'bin'
            commands.mkdir()
            for cmd in ('mktemp', 'cp', 'chmod', 'rm', 'rmdir'):
                (commands / cmd).symlink_to(shutil.which(cmd))
            env = dict(os.environ, PATH=str(commands), MX5DR_FIXTURE_ROOT=str(root),
                       HERE=str(bundle), COMMON=str(REPO / 'packaging/common.sh'))
            script = '. "$COMMON"\nverify_bundle_manifest\nhash "$HERE/payload"\n'
            result = subprocess.run(['/bin/sh', '-c', script], env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), hashlib.sha256(data.read_bytes()).hexdigest())
            data.write_bytes(b'corrupt')
            result = subprocess.run(['/bin/sh', '-c', script], env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Bundle checksum mismatch', result.stderr)
