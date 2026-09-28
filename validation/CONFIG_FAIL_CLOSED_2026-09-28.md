# Explicit runtime mode — review follow-up, 2026-09-28

This addresses the blocking finding in
[PR #7's review](https://github.com/junoh-moon/mazda-aa-dr/pull/7#issuecomment-5869146157).
The reviewed parent is `7f89638affd5f1058ff25082790685f1dc98b2f3`.

## Behavior

Previously, merely opening a readable configuration file selected OBSERVE.
Empty files, comments, or valid numeric options without a mode therefore passed
`startup_enabled` and reached the eager-load/hook path.

`read_config` now initializes mode to OFF and requires the mode key to have been
successfully parsed exactly once. Missing mode yields `valid=false, mode=0`.
Explicit OFF remains valid but disabled; explicit OBSERVE, SCRUB and SHADOW keep
their existing meanings. Numeric defaults remain available after explicit mode
selection. Invalid values, duplicate keys and unsupported ASSIST remain rejected.
The installer already writes an explicit mode and needs no default change.

No loader state-machine, firmware address, ABI, recovery or ASSIST behavior was
changed. PR #1 was merged; #7 now targets `master`.

## Reproduction and verification

Environment: Linux x86_64, GCC/G++ 13.3.0, Python 3.12. Host tests compile with
`-Wall -Wextra -Werror`.

Regression tests were added before changing production code. On the reviewed
parent's production implementation, the config test aborted and all five new
actual-interposer scenarios failed. The original 24 loader scenarios passed.
After the parser fix, the following passed:

| Command | Result |
| --- | --- |
| `make build/test_runtime && build/test_runtime` | 38 config/SHA checks passed |
| `make test-loader` | 29 fresh-process synthetic ELF loader scenarios passed |
| `git diff --check` | No whitespace errors |

The five new configurations are empty, whitespace-only, comment-only (including
commented-out `mode=OBSERVE`), only `sample_ms`, and all valid numeric options
without mode (no final newline). Unit tests check invalid OFF and rejected startup
for each. Positive tests retain explicit modes and numeric defaults, including a
mode placed after another option without a final newline. Existing malformed and
out-of-range numeric cases now include an explicit mode so missing-mode rejection
cannot hide a regression in their own validation.

Each new loader scenario runs production `loader.cpp` and `config.cpp` against a
synthetic DSO that loads with LAZY but fails with NOW. It asserts exactly one
next-chain call with the original flags, zero bootstraps, BYPASS outcome, preserved
caller errno/dlerror and no fallback diagnostic. A later target load must also
forward once without another startup-policy evaluation.

This follow-up did not run the full `make test`, an ARM build/QEMU suite, an OEM
module, an existing touch preload or a physical CMU. Earlier ARM results remain
historical evidence for the parent revision. No vehicle package was rebuilt.
The existing 0.2 trial ZIP and heads of stacked PRs #8/#9 do not yet include this
fix; they must incorporate it and be rebuilt/revalidated before use.

## Nonblocking review notes still open

The existing stderr diagnostic can block on a full pipe or trigger SIGPIPE if a
pipe has no reader; a small fixed buffer does not bound write time. This change
does not claim to fix that separate failure path. Fatal RX recovery still keeps
the patch gate closed until process exit, including the marker-write interval.
The process diagnostic report remains a non-concurrent diagnostic interface.
These notes are separate from the corrected missing-mode activation blocker.
