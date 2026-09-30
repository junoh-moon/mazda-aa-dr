# v74 OEM adapter

This directory contains executable adapter logic and an ARM32 installation
backend. It is not evidence that the package has run on a CMU.

## Integration

Build `adapter.cpp`, `v74_install.cpp`, `request_hooks.cpp`, `session_hooks.cpp`, and the runtime
`request_trace.cpp`/`request_observer.cpp`. ARM32 softfp also uses
`arm_veneer.S` and `request_veneer.S`. The Makefile enables exception cleanup
for the wrappers and Observer. Link pthread, dl, and the math runtime. C++11 is required;
no STL containers or dynamic allocation are used on the hook path. Load the
library at process startup: the fixed TLS storage uses the initial-exec model.

The bootstrap owns SHA-256 verification and detection of the first cold BLM
load. It calls `install_v74` before the loader caller can use
`GetServiceInterfaces`. `verified_cold_start` must never be guessed true for an
already loaded module. Both supplied paths must exactly match their `dladdr`
names. The BLM dependency bindings must be eager (`RTLD_NOW`/`LD_BIND_NOW`).
The backend refuses a lazy resolver or a different shim in the send GOT slot;
it does not bypass that shim. It never changes the touch hooks.

Production sets `observe_requests=true`, supplies the existing `blm_handle`,
and uses `read_request_trace` and `read_send_session`. The same cold transaction
installs four BLM entries and seven GOT slots across BLM, JCIDBUS and the LDS data client. Their
whole-file hashes, mappings, entry bytes and original slot targets are checked
before the lease. BLM worker/vtable symbols are local ELF symbols; they cannot
be queried with `dlsym`. JCIDBUS code pages remain executable and unchanged.
After every fallible memory operation succeeds, cleanup slots are published
before submit, with an ARM memory barrier after each pointer store. A failed
RX restoration rolls back all BLM entries; an unrecoverable page failure stops
the service. Prepared original targets remain valid until process exit.

The request observer copies actual reply metadata before the AA util can
flatten errors, then joins the exact live worker and position pointer. A
request remains pending until its actual method free, including cancellation
without notification. It never owns callback userdata or OEM reference counts.
POSITION and SEND records carry the same owned trace; health reports observation
loss and ABI mismatches. These process-local IDs and receipt times do not prove
provider/receiver/session qualification or producer measurement time.

Session observation wraps the exact create/destroy APIs and the status callback
in the original 76-byte table. It preserves the other 18 entries, userdata,
the full SessionInfo pointer, results and errno. Up to 64 immutable contexts live
until process exit; they are never recycled, so a late old callback cannot become
a callback for a new session at the same address. Exhaustion or conflicting
observation disables the observation claim and keeps forwarding the OEM call.
Readers and callbacks use bounded lock-free atomic operations, with no mutex,
allocation, I/O or source retry loop. This is not a wall-clock latency guarantee.

The request's `session_context` is the unique live context observed at issue;
`send_session` separately identifies the actual storage argument at send. Neither
implies request ownership, a connected phone or acceptance. The older qualified
session fields remain unknown. Lifecycle overlap, ambiguity and faults remain
explicit journal results. Raw state is known only after a real callback.
Snapshots include a coherent lifecycle `revision`, incremented even for failed
calls and late callbacks; it is copied at request issue. This process-local
number is not qualified request/session ownership. Exhaustion latches an
observation fault instead of reusing a revision. Context storage is
constant-initialized even on GCC 4.9; an early preload call must not
be erased by later global constructors. See
[session product verification](../../validation/SESSION_PRODUCT_2026-09-30.md).

Create, destroy and status callbacks revoke the adapter prediction generation
before the original call and again at completion, including exceptions and
deferred cancellation. The second revocation also rejects candidates published
during the original call. While any such call is in flight, session readers
report `transition`; concurrent readers never wait or suppress the callback.
When a session reader is configured, ASSIST selection forwards the original
payload unless that reader observed a live, unambiguous send storage. This is
only a negative guard: it does not qualify a session, phone or queued request.
It is not synchronization with OEM teardown after the selection check.

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
The v74 session hooks now perform the observed lifecycle revocations above.
The worker separately resets MODEL/holdout prediction, calibration, and queued
inputs when the coherent observed session boundary changes. A transition,
missing/ambiguous session, or observation fault suspends MODEL input; raw
capture continues. Old request revisions cannot seed either calculator after
a reset, and raw receipts before the worker's boundary check are excluded.
Recovery requires fresh post-boundary motion coverage and a new GPS anchor
pair. Ordinary GPS/GAP changes do not alter this session fence, so normal
tunnel prediction continues. These are negative MODEL guards: qualified
request provenance, sensor timing and phone acceptance remain unimplemented
or unverified, and live ASSIST remains disabled.

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
* The assembly veneer forwards r0-r3 through a register frame and preserves
  the original result registers across C++ scope cleanup, avoiding an invented
  return declaration. The examined caller has no stack arguments and ignores
  the return value. Hard-float ABI builds are rejected. EHABI records and C++
  cleanup propagate exceptions and deferred pthread cancellation through our
  position/send and notify/worker frames. Authored ARM and exact production-DSO tests cover this
  boundary, including a relocated trampoline. This does not repair or prove
  unwind support in every OEM caller/body. Asynchronous cancellation and
  longjmp across a live C++ scope are outside this contract.
  Static exception-runtime archive symbols remain hidden in the preload.
* BLM send relocation is `R_ARM_JUMP_SLOT` at `0xF88BC`; accepted next target is
  interface load bias + `0x1A538`. Both runtime entry prefixes are compared.
  The exact stock ELF has no GNU_RELRO segment. The lower-only backend changes
  this send slot; production also installs the request and session slots above.
* RequestSendPosition -> SendLocation -> OrderSendVehicleData ->
  RaceAap::SendVehicleData -> export is a direct nested call chain. The worker
  queue is before RequestSendPosition, not in OrderSendVehicleData.
* The 31 BLM init-array entries consist of frame_dummy and static initializers
  of iostream/static enum maps. Examined static initializers have no VDM/AapProc
  startup or thread-create calls. GetServiceInterfaces only returns the static
  interfaces table. The first dlopen-return boundary is therefore the selected
  cold installation point. Actual product bootstrap is tested with original
  APIs in a diagnostic VM; normal vehicle startup remains a separate test.

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
