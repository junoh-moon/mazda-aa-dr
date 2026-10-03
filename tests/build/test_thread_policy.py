import pathlib
import re
import unittest

SRC = pathlib.Path(__file__).resolve().parents[2] / "src"
HELPER = SRC / "runtime" / "worker_thread.h"


class ThreadPolicyTests(unittest.TestCase):
    """The stock CMU gives default-attribute threads only 128 KiB of stack
    (init_cmu RLIMIT_STACK soft 131072, used by glibc 2.11 as the pthread default).
    Product code that runs inside stock services must create threads through
    mx5::runtime::create_thread with an explicit stack size."""

    def sources(self):
        for path in sorted(SRC.rglob("*")):
            if path.suffix in (".c", ".cc", ".cpp", ".h", ".hpp") and path != HELPER:
                yield path, path.read_text(errors="replace")

    def test_helper_sets_an_explicit_stack_size(self):
        text = HELPER.read_text()
        self.assertIn("pthread_attr_setstacksize", text)
        self.assertIn("WORKER_STACK_BYTES", text)

    def test_no_default_attribute_threads_in_product_code(self):
        offenders = []
        for path, text in self.sources():
            for number, line in enumerate(text.splitlines(), 1):
                code = line.split("//")[0]
                if re.search(r"\bpthread_create\s*\(", code) or re.search(r"\bstd::thread\b", code):
                    offenders.append("%s:%d: %s" % (path.relative_to(SRC.parent), number, line.strip()))
        self.assertEqual(offenders, [], "create threads with mx5::runtime::create_thread (explicit stack)")


if __name__ == "__main__":
    unittest.main()
