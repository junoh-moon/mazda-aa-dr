# Project guidance for automated contributors

Read `docs/STATUS_KO.md`, `docs/REVIEW_2026-09-28_KO.md`, and the relevant source before proposing a change. Those current status documents supersede historical design and installation GO statements.

- Target: first-generation Mazda Connect NA 74.00.324A. Do not generalize offsets, hashes or ABI to another firmware.
- This integration branch is a first stationary OBSERVE trial candidate, not a vehicle-approved release. OFF, collector isolation and one-boot recovery are implemented; target execution, same-running-SM retries and phone acceptance remain unverified. Read docs/FIRST_TRIAL_KO.md and validation/INTEGRATION_2026-09-28.md.
- Keep live ASSIST disabled until documented sensor, provenance, recovery and phone/app gates are satisfied. SHADOW does not currently run live DR.
- Preserve OEM forwarding contracts, registers, errno, preload coexistence and installer token ownership. A proposed DROP mode needs an explicit different send/return contract.
- Keep observed receipt time separate from producer measurement time. Never promote a successful SMDB poll into a fresh/VALID sample.
- No driving-time CMU interaction. Observation must run automatically; configuration and collection occur while parked.
- Do not publish OEM binaries/maps, disassembly dumps, credentials, personal share links or unredacted vehicle logs. Do not weaken firmware guards to make tests pass.
- Changes belong in focused commits/PRs with problem, changed behavior, test evidence and remaining limitations. Report skipped tests explicitly.
- Use `make test` for host checks when dependencies are available. Exact-ARM checks use the pinned toolchain; see `docs/toolchain.md`. QEMU/synthetic success is not vehicle validation.
- Preserve the imported baseline provenance. Do not edit historical test counts into new test results. New verification belongs in a dated record.
- Do not claim Claude or a physical vehicle was used without actual execution evidence. Independent agent review is useful but does not replace target tests.
