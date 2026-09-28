# v74 OEM adapter

This directory contains executable adapter logic and an ARM32 installation
backend. It is not evidence that the package has run on a CMU.

## Integration

Build `adapter.cpp`, `v74_install.cpp`, and (only for ARM32 soft-float/softfp)
`arm_veneer.S`. Link pthread, dl, and the usual math runtime. C++11 is required;
no STL containers or dynamic allocation are used on the hook path. Load the
library at process startup: the fixed TLS storage uses the initial-exec model.

The bootstrap owns SHA-256 verification and detection of the first cold BLM
load. It calls `install_v74` before the loader caller can use
`GetServiceInterfaces`. `verified_cold_start` must never be guessed true for an
already loaded module. Both supplied paths must exactly match their `dladdr`
names. The BLM dependency bindings must be eager (`RTLD_NOW`/`LD_BIND_NOW`).
The backend refuses a lazy resolver or a different shim in the send GOT slot;
it does not bypass that shim. It never changes the touch hooks.

The backend's own `configure` call freezes runtime callback pointers. Do not
configure separately and then call `install_v74`; that is rejected. A separate
lower-only observer can use `configure` directly, but it has no mode context and
cannot scrub/substitute samples.

After successful installation, select OBSERVE (default) or SCRUB_STALE with
`set_mode`. SCRUB_STALE clears only the LOCATION speed/bearing validity bytes
and values when the enclosing verified RequestSendPosition call has mode 0.
It does not generate a location, change coordinates, repair DR, change native
mode 3, or alter SPEED/GEAR/satellite events. Multiple LOCATION events in one
context latch mutation off for the rest of the process.

Provide a bounded `ObservationSink` that copies the POD record to a fixed ring;
the sink must not retain its pointer or do blocking I/O. Position observations
carry the original LDS fields and generation before the stock call. SEND
observations copy both payloads before calling the original send. A full log
ring may drop telemetry; loss of estimator input must separately call
`invalidate`. All callbacks must return normally without C++ exceptions.

## DR extension (disabled by default)

`Options.allow_assist` must remain false until the deployment's profile,
source ownership/provenance, sensor quality/timing and lifecycle gates are
verified. The application supplies a `ProvenanceReader` for the exact original
request, not a poll of the most recent owner. Its epochs must follow source and
AA lifecycle changes; call `invalidate` at each such change and on sensor loss.

The worker receives the POSITION event and can publish an owned normalized
`DrSnapshot` tagged with the current generation after processing that control
event. Mode transitions invalidate synchronously, before the POSITION event;
repeated mode 0 does not continually advance the generation. The worker must
process its input/control queue in order, require fresh re-seeding after GPS
returns or INVALID, and never simply relabel an old estimate with a new
generation. A snapshot must carry independently verified `ready`, profile,
input-quality and limit flags. Encoder unit/range checks are additional checks,
not a substitute for those physical contracts.

The lower hook compares context/snapshot/current generation, exact-request
source/session epochs, source lease and frontier age at selection. It copies
the candidate under `pthread_mutex_trylock`; contention forwards original.
The worker publication API may block and must not be called on an OEM thread.
No new sends occur. Failed replacement sends return their real status/errno
without retrying the original payload.

## Exact backend evidence and restrictions

* BLM SHA-256: `10e7235bfce075b44c1a8ffc99bbc9b63af85d9262df868ca1874ec36d8d3b71`.
* Interface SHA-256: `e9eb5e0d42719c98efc5ef86a270b4b3c86467aced55d4c8158bd99e06bbd436`.
* RequestSendPosition ELF VA `0xC7460`; first two ARM instructions are
  `push {r4, fp, lr}` and `add fp, sp, #8`. Neither relocated instruction is
  PC-relative. The generated trampoline continues at `0xC7468`.
* The assembly veneer forwards this/input in r0/r1 and preserves r0-r3 around
  its exit callback, avoiding an invented C++ return declaration. The examined
  caller has no stack arguments and ignores the return value. Hard-float ABI
  builds are rejected. Cross-boundary exceptions/unwinding are not supported
  by this exact assembly binding and must not be introduced by callbacks.
* BLM send relocation is `R_ARM_JUMP_SLOT` at `0xF88BC`; accepted next target is
  interface load bias + `0x1A538`. Both runtime entry prefixes are compared.
  The exact stock ELF has no GNU_RELRO segment. Only its send GOT slot changes.
* RequestSendPosition -> SendLocation -> OrderSendVehicleData ->
  RaceAap::SendVehicleData -> export is a direct nested call chain. The worker
  queue is before RequestSendPosition, not in OrderSendVehicleData.
* The 31 BLM init-array entries consist of frame_dummy and static initializers
  of iostream/static enum maps. Examined static initializers have no VDM/AapProc
  startup or thread-create calls. GetServiceInterfaces only returns the static
  interfaces table. The first dlopen-return boundary is therefore the selected
  cold installation point; runtime loader integration still needs target QA.

No hot patch, dlclose, or live unpatch API is provided. Disabling stops new
mutation and leaves hook/trampoline storage valid until process exit. Installation
temporarily makes the target page RW/NX under the cold-start precondition. If
restoring its executable permission fails twice, `RESTORE_FAILED_FATAL` requires
the bootstrap to stop the service rather than return into an NX OEM page.

## Verification performed here

Host C++11 fake-OEM tests cover byte-identical forwarding, exact-once send and
return/errno preservation; precise mode-0 scrub; native modes; malformed and
multiple LOCATIONs; nested TLS; normalized DR encoding; epoch changes; rapid
GPS-return/unknown transitions; expiry; and host refusal of the ARM backend.
They also pass ASan/UBSan (LeakSanitizer disabled because this sandbox cannot
read its process task directory). Host tests do not establish ARM loader/ABI
or on-car behavior.

Example host command:

```
g++ -std=c++11 -Wall -Wextra -Werror -pthread -I src \
  src/adapter/adapter.cpp src/adapter/v74_install.cpp \
  tests/adapter/adapter_test.cpp -o adapter_test
```

Run the binary separately with each argument: `observe scrub native malformed
nested assist epoch reacquire expiry encoder backend`.

`tests/adapter/run_arm.sh` cross-compiles the real backend and veneer, rejects
shared objects with TEXTREL, then runs the 11 functional cases and an ARM
assembly fixture under qemu-user. Set `CROSS_COMPILE` to the toolchain's absolute
binary prefix and `QEMU_SYSROOT` to its target sysroot. The fixture invokes the
actual veneer and a synthetic original function, checking r0/r1 inputs, all
r0-r3 return words, exact-once calls, nested TLS restoration, repeated calls,
mode-0 scrub and errno. It does not load or execute proprietary OEM code.

This ARM suite also passed using the pinned m3-toolchain GCC 4.9.1 and its old
softfp sysroot, not only the modern fallback compiler. Its adapter shared object
requires GLIBC_2.4 symbol versions and has no TEXTREL. Passing these synthetic
checks establishes the compiler/veneer contract; cold loader installation still
requires stationary target validation against the exact OEM objects.
