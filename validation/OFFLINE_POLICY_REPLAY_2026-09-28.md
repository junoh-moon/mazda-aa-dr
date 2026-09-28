# Offline policy replay validation — 2026-09-28

Base: `048ee575940fa810a45e34fd18c0afb81cec9e29` (v0.2.0-observe.1).
New files: PC-only replay tool, synthetic tests, Korean experiment contract.

`make test-tools`: **39 tests passed, no skips**: the existing 20 auditor tests
and 19 new policy replay tests. Coverage includes exact SCRUB bytes/padding,
native-mode preservation, unassigned DROP return/errno, no-op SCRUB, mode and
time gaps, boot boundaries, dirty/missing health, unsupported contexts,
duplicate LOCATION calls, backwards time, separate non-LOCATION results,
truncated/violating traces, TAR rotation without extraction, resource limits,
CLI exit codes, and withholding every projection after any input fault.

`make test` passed the remaining host components (guard 14, loader 29 scenarios,
core 1,425 checks and replay, adapter 11 scenarios, config/SHA 38 checks,
journals, collector 10, pipeline 8 sends). The initial invocation skipped the
13 packaging cases because the default private-fixture path was absent.
The packaging target was then rerun with the available private stock fixture
explicitly selected; see final result below. Historical release test counts
have not been edited.

Final packaging rerun: **13 tests passed, no skips**, including the actual
release payload and existing-touch-token preservation. The private fixture
was used only for read-only identity comparison, never executed or published.

No new ARM build is needed for this PC-only change. No CMU, OEM executable,
live logs, or phone was used. Live DROP/ASSIST and phone fallback are not
implemented or validated.
