"""The shipped collector must never read the stock SMDB or signal/spawn/wait on other processes.

2026-10-04 vehicle trial: the CMU reset itself after jciblmVdt stopped answering SM heartbeats.
The collector had been spawning the stock smdb-read tool and SIGKILLing it on timeout; libjcismdb
guards the SMDB with a named POSIX semaphore (no owner, not robust) that a killed holder never
releases. The collector no longer touches the SMDB at all; these tests keep that true for the
source and for the shipped binary.
"""
import ctypes
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[2]
FORBIDDEN_IMPORTS = {
    'fork', 'vfork', '__fork', '__libc_fork', 'posix_spawn', 'posix_spawnp', 'execv', 'execve',
    'execvp', 'execl', 'execlp', 'system', 'popen', 'waitpid', '__waitpid', 'wait', 'wait4',
    'kill', 'killpg', 'sem_open', 'sem_wait', 'sem_post', 'sem_trywait', 'shm_open', 'mmap',
}
FORBIDDEN_STRINGS = ('smdb-read', '/jci/smdb', 'vdm_vdt_current_data', 'libjcismdb')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


class CollectorSafetyTests(unittest.TestCase):
    def test_source_has_no_smdb_or_process_control(self):
        code = strip_comments((REPO / 'src/collector/collector.cpp').read_text())
        for token in ('smdb', 'SMDB', 'SIGKILL', 'waitpid', 'posix_spawn', 'fork(', 'execv', 'kill(',
                      'sem_open', 'sem_wait', 'shm_open', 'popen', 'system('):
            with self.subTest(token=token):
                # Only the fixed journal value "smdb_disabled" may mention the name.
                self.assertNotIn(token, code.replace('smdb_disabled', ''))
        for header in ('<spawn.h>', '<sys/wait.h>'):
            self.assertNotIn(header, code)

    def test_release_bundle_collector_binary(self):
        bundle = os.environ.get('MX5DR_RELEASE_BUNDLE')
        if not bundle or not (Path(bundle) / 'mx5dr-collector').is_file():
            self.skipTest('MX5DR_RELEASE_BUNDLE with the shipped collector is not set')
        binary = Path(bundle) / 'mx5dr-collector'
        symbols = subprocess.check_output(['readelf', '--dyn-syms', '-W', str(binary)], text=True)
        imported = {line.split()[-1].split('@')[0] for line in symbols.splitlines()
                    if ' UND ' in line and line.split()[-1] not in ('', 'Name')}
        self.assertEqual(imported & FORBIDDEN_IMPORTS, set())
        data = binary.read_bytes()
        for text in FORBIDDEN_STRINGS:
            with self.subTest(text=text):
                self.assertFalse(text.encode() in data, 'shipped collector contains ' + text)

    def test_a_killed_semaphore_holder_leaves_the_lock_held(self):
        # Mechanism only (POSIX semantics libjcismdb relies on), not a statement about the vehicle.
        libc = ctypes.CDLL(None, use_errno=True)
        if not hasattr(libc, 'sem_open'):
            self.skipTest('sem_open is not available')
        libc.sem_open.restype = ctypes.c_void_p
        libc.sem_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint, ctypes.c_uint]
        libc.sem_wait.argtypes = [ctypes.c_void_p]
        libc.sem_trywait.argtypes = [ctypes.c_void_p]
        libc.sem_unlink.argtypes = [ctypes.c_char_p]
        name = ('/mx5dr-test-%d' % os.getpid()).encode()
        libc.sem_unlink(name)
        ready_r, ready_w = os.pipe()
        pid = os.fork()
        if pid == 0:
            try:
                sem = libc.sem_open(name, os.O_CREAT, 0o600, 1)
                libc.sem_wait(sem)              # take the lock, as SMDB_Lock does
                os.write(ready_w, b'1')
                time.sleep(30)                  # killed here, before any sem_post
            finally:
                os._exit(0)
        try:
            os.close(ready_w)
            self.assertEqual(os.read(ready_r, 1), b'1')
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)
            sem = libc.sem_open(name, 0, 0, 0)
            self.assertTrue(sem)
            self.assertEqual(libc.sem_trywait(sem), -1)      # nobody can ever take it again
            self.assertEqual(ctypes.get_errno(), 11)         # EAGAIN
        finally:
            libc.sem_unlink(name)


if __name__ == '__main__':
    unittest.main()
