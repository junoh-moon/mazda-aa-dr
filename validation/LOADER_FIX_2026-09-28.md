# OFF/loader change — 2026-09-28

This changes issue #2; vehicle installation remains on hold. Polling isolation,
external crash recovery and phone acceptance are separate work. Stock dependency
name/version coverage is recorded in [BLM_DEPENDENCY_CLOSURE.md](BLM_DEPENDENCY_CLOSURE.md); it is not an OEM loader execution. Historical 0.1 validation records have not been rewritten.

The first ordinary exact-target load reads configuration and the disable marker
before any extra dlopen probe. OFF, invalid/missing configuration and a present
or uninspectable marker forward the original flags. A failed additional NOW load
retries the original flags exactly once; neither that fallback nor later calls
can install hooks. If the caller already requested NOW, no identical retry is
added. The final real loader call owns the returned handle, errno and failure
message; internal successful-bootstrap lookup errors are cleared.

A fixed-size process report retains the original NOW failure and retry result.
The fallback emits one best-effort stderr record (under 384 bytes), without
allocation or loader API calls. It is not a durable log guarantee or a wall-time
bound on an externally supplied stderr sink; write failure is recorded in the
report and does not replace the caller's errno/dlerror. OFF/non-target paths do
not emit it.

No application mutex is held across a loader call. Concurrent/reentrant target
exposure cancels the cold candidate. The adapter acquires an atomic patch lease
only after symbol, mapping, original-byte and hash checks. The lease spans only
memory preparation/patching and releases after executable permissions are safe;
there are no dlopen/dlsym/dladdr/dl_iterate_phdr or file/hash calls inside it.
Production remains linked with -z now. Target NOLOAD participates before its real
call and before a successful handle is returned, including a call that started
while the state was still FRESH. A fatal RX restoration failure keeps the gate
closed until the existing fatal bootstrap exit; this is not general recovery.

Scope: the recognized pathname is exactly `/jci/aapa/blmjciaapa.so`, matching the
known service path. Concurrent alias-path loads, dlmopen namespaces, saved direct
loader pointers that bypass this interposer, and arbitrary constructor-created
OEM producers are not established as safe. This change does not prove general
hot-patching or establish the cold-start assumptions on a vehicle.

Host verification: `make test` passed, including 24 fresh-process synthetic ELF
interposer scenarios. Cases cover OFF/invalid/marker flag preservation, NOW-only
failure with LAZY success, both failures, caller-NOW, already-loaded targets,
NOLOAD, invalid flags, constructor reentry, eight concurrent callers, a reproduced
cross-constructor lock inversion, cancellation during preparation, waiting during
the patch, an early-NOLOAD race, and failed stderr writes. Next-chain errno and
caller dlerror are asserted. No OEM module is executed. Root reran `make test`, built the production ARM DSO
with pinned GCC 4.9.1, and passed `tests/run_arm_all.sh` in QEMU. The same 24
actual-interposer synthetic scenarios also passed after cross-compilation against
that pinned sysroot and execution under qemu-arm. This establishes synthetic ARM
behavior, not execution of the OEM BLM, existing touch preload or physical CMU.

Run the isolated suite with `python3 tests/runtime/test_loader_interposer.py`.
It also accepts CC/CXX, LOADER_TEST_ARCH_FLAGS and QEMU_SYSROOT for a cross build
and qemu-arm execution with the same fixtures.
