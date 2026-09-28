# mazda-aa-dr

Experimental **Android Auto (AA) / Dead Reckoning (DR)** research for first-generation Mazda Connect, targeting **NA 74.00.324A** and a 2019 MX-5 ND2.

**Vehicle installation is on hold.** The imported 0.1 code has unresolved loader and crash-recovery defects, including a possible reboot loop. No vehicle or phone validation has been performed. Live ASSIST is disabled; this is not a working tunnel-navigation solution.

[한국어](README_KO.md) · [Current status / handoff](docs/STATUS_KO.md) · [Review corrections](docs/REVIEW_2026-09-28_KO.md)

## Goal and current implementation

The goal is to let Android Auto navigation, initially Naver Map, benefit from vehicle motion during GNSS outages. Other apps and other firmware versions are separate compatibility questions.

The source baseline contains an offline DR core, version-specific ARM hooks, automatic observation, a bounded journal, an installer, and a PC log analyzer. SCRUB removes optional stale speed/bearing fields from selected mode-0 cached locations; it does not generate a new position. SHADOW currently behaves as observation only. Runtime ASSIST is blocked in code and configuration.

The baseline preserves the implementation reviewed as experimental 0.1. **The subsequent OFF, polling, and recovery fixes have not been applied.** Each should receive its own change and validation record.

## Start here

| Question | Entry point |
| --- | --- |
| What is established, uncertain, or blocked? | [Current status](docs/STATUS_KO.md) |
| What did reviewers find, and how will fixes be judged? | [Review and acceptance criteria](docs/REVIEW_2026-09-28_KO.md) |
| How does the current implementation work? | [Implementation review](docs/IMPLEMENTATION_REVIEW_KO.md), then `src/` |
| What did the original design propose? | [Historical design v1](docs/archive/DESIGN_V1_KO.md), superseded where noted |
| What was actually tested? | [Historical validation](docs/VALIDATION.md) and [public-import checks](validation/PUBLIC_IMPORT.md) |
| How were native NNG DR and sensor limits investigated? | [Native DR analysis](docs/native_dr_followup.md) |
| Why were decisions made? | [Decision log](docs/DECISIONS_KO.md) |

## Development

On a Linux development host with a C/C++ compiler, make, Python 3, pkg-config and D-Bus development headers:

```sh
make test
```

The packaging identity tests require separately obtained matching stock firmware fixtures; they explicitly skip when fixtures are absent. Skips are not passes. See [contribution notes](CONTRIBUTING.md) for the expected path and [toolchain notes](docs/toolchain.md) for the pinned ARM build. Host tests and synthetic ARM QEMU tests do not establish CMU recovery or phone/app acceptance.

## Public contents

This repository contains authored source, tests, technical interpretation, and validation records. OEM firmware, extracted libraries/maps, disassembly dumps, personal download links, vehicle location logs, the toolchain, and prebuilt installation bundles are omitted. Firmware hashes and narrow compatibility signatures remain so the target can be identified. The historical archive and imported-source identities are recorded in `validation/source-import.json`.

No project license has been selected yet. See [provenance](docs/PROVENANCE.md) for referenced projects and the limits of the imported evidence.
