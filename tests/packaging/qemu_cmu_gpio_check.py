#!/usr/bin/env python3
"""Exercise a partial two-pad QEMU model without booting any firmware.

No electrical, USB-device, LDS, or navigation validation is implied.
"""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

PINS = ((0x020a4000, 31, 0x020e00d0, 'gpio3'),
        (0x020a8000, 15, 0x020e021c, 'gpio4'))


class VM:
    def __init__(self, binary, enabled):
        self.directory = tempfile.TemporaryDirectory(prefix='mx5-gpio-qtest-')
        root = Path(self.directory.name)
        self.log = (root / 'stderr').open('w+')
        command = [binary, '-machine', 'sabrelite', '-smp', '2', '-m', '128',
                   '-display', 'none', '-monitor', 'none', '-serial', 'null',
                   '-nic', 'none', '-S', '-qtest-log', '/dev/null',
                   '-qtest', f'unix:{root}/qtest.sock,server=on,wait=off',
                   '-qmp', f'unix:{root}/qmp.sock,server=on,wait=off']
        if enabled:
            command += ['-global', 'fsl-imx6.experimental-cmu-gpio-readback=on']
        self.process = subprocess.Popen(command, stdout=self.log, stderr=self.log)
        self.sockets = []
        self.events = []
        try:
            self.qtest = self.connect(root / 'qtest.sock').makefile('rwb', buffering=0)
            self.qmp = self.connect(root / 'qmp.sock').makefile('rwb', buffering=0)
            assert 'QMP' in json.loads(self.qmp.readline())
            self.qmp_call('qmp_capabilities')
        except BaseException:
            self.log.seek(0)
            print(self.log.read())
            self.close()
            raise

    def connect(self, path):
        stream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        stream.settimeout(5)
        self.sockets.append(stream)
        end = time.monotonic() + 5
        while time.monotonic() < end:
            if self.process.poll() is not None:
                raise RuntimeError('QEMU exited during startup')
            try:
                stream.connect(str(path))
                return stream
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(.01)
        raise TimeoutError('QEMU socket startup')

    def command(self, text):
        self.qtest.write((text + '\n').encode('ascii'))
        result = self.qtest.readline().decode('ascii').strip()
        if not result.startswith('OK'):
            raise RuntimeError(f'{text}: {result}')
        return result[2:].strip()

    def read(self, address):
        return int(self.command(f'readl {address:#x}'), 0)

    def write(self, address, value):
        self.command(f'writel {address:#x} {value:#x}')

    def qmp_call(self, method):
        self.qmp.write((json.dumps({'execute': method}) + '\n').encode())
        while True:
            row = json.loads(self.qmp.readline())
            if 'event' in row:
                self.events.append(row['event'])
                continue
            if 'return' not in row:
                raise RuntimeError(row)
            return row['return']

    def wait_event(self, name):
        while name not in self.events:
            row = json.loads(self.qmp.readline())
            if 'event' not in row:
                raise RuntimeError(row)
            self.events.append(row['event'])
        self.events.remove(name)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        for name in ('qtest', 'qmp'):
            if hasattr(self, name):
                getattr(self, name).close()
        for stream in self.sockets:
            stream.close()
        self.log.close()
        self.directory.cleanup()


class Readback(unittest.TestCase):
    def setUp(self):
        self.vm = VM(ARGS.qemu, ARGS.enable_model)
        self.addCleanup(self.vm.close)

    def configure(self, pin):
        base, bit, mux, _ = pin
        mask = 1 << bit
        self.vm.write(mux, 0x15)
        self.vm.write(base + 4, mask)
        return base, mask, mux

    def test_mux_stores_original_alt5_sion(self):
        for _, _, mux, _ in PINS:
            with self.subTest(mux=hex(mux)):
                self.vm.write(mux, 0x15)
                self.assertEqual(self.vm.read(mux), 0x15)

    def test_output_follows_both_levels_and_direction(self):
        for pin in PINS:
            with self.subTest(pin=pin[3]):
                base, mask, _ = self.configure(pin)
                for value in (0, mask, 0, mask):
                    self.vm.write(base, value)
                    self.assertEqual(self.vm.read(base + 8), value)
                self.vm.write(base + 4, 0)
                self.assertEqual(self.vm.read(base + 8), 0)
                self.vm.write(base + 4, mask)
                self.assertEqual(self.vm.read(base + 8), mask)

    def test_sion_and_gpio_mux_are_both_required(self):
        for pin in PINS:
            with self.subTest(pin=pin[3]):
                base, mask, mux = self.configure(pin)
                self.vm.write(base, mask)
                for value, expected in ((0x15, mask), (5, 0), (0x14, 0),
                                        (0x10, 0), (0x15, mask)):
                    self.vm.write(mux, value)
                    self.assertEqual(self.vm.read(base + 8), expected)

    def test_unmodelled_output_pins_remain_unchanged(self):
        for pin in PINS:
            with self.subTest(pin=pin[3]):
                base, mask, _ = self.configure(pin)
                self.vm.write(base + 4, 0xffffffff)
                self.vm.write(base, 0xffffffff)
                self.assertEqual(self.vm.read(base + 8), mask)
        self.vm.write(0x0209c004, 0xffffffff)
        self.vm.write(0x0209c000, 0xffffffff)
        self.assertEqual(self.vm.read(0x0209c008), 0)

    def test_input_levels_do_not_use_output_latch(self):
        for base, bit, mux, name in PINS:
            with self.subTest(pin=name):
                mask = 1 << bit
                self.vm.write(mux, 0x15)
                self.vm.write(base + 4, 0)
                self.vm.write(base, mask)
                for level in (0, 1, 0):
                    self.vm.command(f'set_irq_in /machine/soc/{name} unnamed-gpio-in {bit} {level}')
                    self.assertEqual(self.vm.read(base + 8), mask if level else 0)

    def test_reset_clears_mux_readback_enable(self):
        for pin in PINS:
            base, mask, _ = self.configure(pin)
            self.vm.write(base, mask)
        self.vm.qmp_call('system_reset')
        self.vm.wait_event('RESET')
        for base, bit, mux, _ in PINS:
            self.assertEqual(self.vm.read(mux), 5)
            # Restore the latch and direction only; reset must have removed SION.
            self.vm.write(base, 1 << bit)
            self.vm.write(base + 4, 1 << bit)
            self.assertEqual(self.vm.read(base + 8), 0)

    def test_explicitly_disabled_preserves_upstream(self):
        with_vm = VM(ARGS.qemu, False)
        self.addCleanup(with_vm.close)
        for base, bit, mux, _ in PINS:
            with_vm.write(mux, 0x15)
            with_vm.write(base + 4, 1 << bit)
            with_vm.write(base, 1 << bit)
            self.assertEqual(with_vm.read(mux), 0)
            self.assertEqual(with_vm.read(base + 8), 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--enable-model', action='store_true')
    ARGS = parser.parse_args()
    unittest.main(argv=[__file__], verbosity=2)
