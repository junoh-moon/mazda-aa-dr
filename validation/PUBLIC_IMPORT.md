# Public source import validation — 2026-09-28

Scope: transfer the reviewed experimental 0.1 source into a public repository with current status, review corrections and provenance. No runtime, adapter, core, installer or test behavior was changed. This is not a new firmware patch release.

## Integrity and publication checks

- Original archive SHA-256 matches `e2faf183de1c71c32b0afc3580e86571e589a1f5b4952581099ae73261423c7b`.
- All 42 imported files under `src/`, `tests/`, `tools/`, `packaging/`, plus `Makefile`, match the original hashes recorded in `source-import.json` (42 files total across these groups).
- No OEM firmware/library/map, disassembly dump, prebuilt project library, installation archive, private share URL, access credential or vehicle location log is included in the selected public files. Synthetic test data remains.
- Workspace prefixes in historical build output and this import's test output are replaced by `<WORKSPACE>`.
- README/current-status relative links were checked. Historical references to omitted evidence/bundle files are identified as historical/private inputs, not promised repository downloads.
- A separate Astra agent reviewed publication boundaries and source-critical status. No Claude or vehicle validation was performed.

## Newly executed host subset

```sh
make test-core test-adapter test-integration test-tools test-packaging build/test_runtime
build/test_runtime
```

Output: [public-import-host-tests.txt](public-import-host-tests.txt).

| Check | Result |
| --- | --- |
| C99 core | 1,425 synthetic checks passed |
| CSV replay | Determinism, stale/reacquire, sentinel and malformed inputs passed |
| Adapter | 11 cases passed |
| Core → bridge → fake OEM | Eight exactly-once sends and rejection checks passed |
| Log analyzer | 18 tests passed |
| Runtime config/SHA support | 27 checks passed |
| Packaging identity tests | All 11 skipped: matching stock fixtures absent |

The full `make test` suite was **not** run: this import environment lacks pkg-config/D-Bus development dependencies required for the journal test. ARM rebuild/QEMU, target BLM loader tests, real CMU startup/recovery, and phone/app acceptance were **not** repeated or performed. Historical ARM and journal records remain in their original dated validation documents.

These results establish source-preserving import and the listed host behavior only. They do not resolve the OFF, polling or reboot-recovery issues, and do not lift the vehicle installation hold.
