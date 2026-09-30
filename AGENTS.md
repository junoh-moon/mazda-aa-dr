# Project guidance for automated contributors

Read `docs/STATUS_KO.md`, `docs/REVIEW_2026-09-28_KO.md`, and the relevant source before proposing a change. Those current status documents supersede historical design and installation GO statements.

For release work, read `docs/RELEASING_KO.md`. Review/Ready/LGTM, merge and release publication are separate requested actions; do not infer one from another. A request that already authorizes publication does not require duplicate approval. Pin the commit and verify the published installation ZIP; source merging alone is not deployment.

- Target: first-generation Mazda Connect NA 74.00.324A. Do not generalize offsets, hashes or ABI to another firmware.
- The published OBSERVE package and newer SHADOW source are trial candidates, not vehicle-approved releases. OFF, collector isolation and one-boot recovery are implemented; target execution, same-running-SM retries and phone acceptance remain unverified. docs/FIELD_TRIAL_KO.md and docs/FIRST_TRIAL_KO.md are historical trial procedures, not the current work plan. See docs/V1_READINESS_KO.md for current completion criteria and validation/INTEGRATION_2026-09-28.md for the OBSERVE baseline.
- Keep live ASSIST disabled until documented sensor, provenance, recovery and phone/app gates are satisfied. This draft adds actual MODEL-domain SHADOW calculation from an existing VBS callback tap; it is not vehicle-validated and must never promote receipt time or model assumptions into qualified ASSIST. Read docs/LIVE_SHADOW_2026-09-29_KO.md and validation/LIVE_SHADOW_2026-09-29.md.
- For stationary calibration/GPS holdout work, read `docs/SHADOW_CALIBRATION_KO.md`. Keep learned zeros MODEL-only, apply only at new anchors, and keep held-out GPS fields out of prediction. GPS differences are not ground-truth accuracy.
- Preserve OEM forwarding contracts, registers, errno, preload coexistence and installer token ownership. A proposed DROP mode needs an explicit different send/return contract.
- Keep observed receipt time separate from producer measurement time. Never promote a successful SMDB poll into a fresh/VALID sample.
- No driving-time CMU interaction. Observation must run automatically; configuration and collection occur while parked.
- Current user constraint (2026-09-30): work with the supplied firmware files only. Do not request experimental vehicle installations, driving trials, or phone/dongle bench tests. The intended result is a finished v1.0 installed once. Finish feasible original-runtime execution, implementation and offline verification; keep physical sensors, phone/app acceptance and physical recovery explicitly unverified when files cannot establish them. This constraint supersedes earlier one-or-two-visit plans; it does not authorize weakening qualification gates or claiming v1.0 is complete. Preserve raw inputs and failure reasons even when calculation is disabled, while keeping faulty sessions inconclusive.
- Do not publish OEM binaries/maps, disassembly dumps, credentials, personal share links or unredacted vehicle logs. Do not weaken firmware guards to make tests pass.
- Changes belong in focused commits/PRs with problem, changed behavior, test evidence and remaining limitations. Report skipped tests explicitly.
- Use `make test` for host checks when dependencies are available. Exact-ARM checks use the pinned toolchain; see `docs/toolchain.md`. QEMU/synthetic success is not vehicle validation.
- Preserve the imported baseline provenance. Do not edit historical test counts into new test results. New verification belongs in a dated record.
- Do not claim Claude or a physical vehicle was used without actual execution evidence. Independent agent review is useful but does not replace target tests.
