"""Preload libraries must not export their statically linked C++/gcc runtime.

The LD_PRELOADed libraries are mapped into stock services. jciLDS loads the real libstdc++
through libjciusbmgr_client.so and libjcisystem_client.so, so a preload that exports
std::terminate, __cxa_*, __gxx_personality_v0, typeinfo or operator delete would interpose its
own statically linked copy over the stock runtime. libmx5dr.so already hid these with
--exclude-libs,ALL; libmx5dr-ldstap.so exported ~100 of them until 2026-10-04 (found by an
independent review of the vehicle-trial data). Needs MX5DR_RELEASE_BUNDLE (the shipped files).
"""
import os
from pathlib import Path
import re
import subprocess
import unittest

LIBS = ('libmx5dr.so', 'libmx5dr-vimtap.so', 'libmx5dr-ldstap.so')
RUNTIME = re.compile(r'^(_ZN10__cxxabiv|_ZNK10__cxxabiv|_ZNSt|_ZNKSt|_ZSt|_ZT[ISV]|_ZN9__gnu|_Zd[la]|_Zn[wa]|'
                     r'__cxa|__gxx|__gnu_|__gcclibcxx|_Unwind|__aeabi|__gcc|__emutls)')


class PreloadExportTests(unittest.TestCase):
    def exports(self, path):
        out = subprocess.check_output(['readelf', '--dyn-syms', '-W', str(path)], text=True)
        names = set()
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 8 and parts[4] == 'GLOBAL' and parts[5] == 'DEFAULT' and parts[6] != 'UND':
                names.add(parts[7].split('@')[0])
        return names

    def test_no_static_runtime_symbols_are_exported(self):
        bundle = os.environ.get('MX5DR_RELEASE_BUNDLE')
        if not bundle:
            self.skipTest('MX5DR_RELEASE_BUNDLE is not set')
        for lib in LIBS:
            path = Path(bundle) / lib
            if not path.is_file():
                self.skipTest(lib + ' is not in the bundle')
            with self.subTest(lib=lib):
                leaked = sorted(n for n in self.exports(path) if RUNTIME.match(n))
                self.assertEqual(leaked, [], '%s exports its runtime: %s' % (lib, leaked[:8]))


if __name__ == '__main__':
    unittest.main()
