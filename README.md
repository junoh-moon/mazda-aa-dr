# mazda-aa-dr

Experimental **Android Auto (AA) / Dead Reckoning (DR)** research for first-generation Mazda Connect, targeting **NA 74.00.324A** and a 2019 MX-5 ND2.

**This integration branch is a 0.2 candidate for a first stationary OBSERVE trial.** OFF loader handling, separate polling and a one-use pre-Service-Manager gate are implemented and checked on host and synthetic ARM/QEMU fixtures. No vehicle or phone validation has been performed. Live ASSIST remains disabled; this is not a working tunnel-navigation solution.

See [first stationary trial](docs/FIRST_TRIAL_KO.md) and [integration evidence](validation/INTEGRATION_2026-09-28.md). The gate removes our preload from persistent service configurations and consumes one explicit authorization before exposing a trial. Same-running-SM retry behavior is not established.

[한국어](README_KO.md) · [Current status / handoff](docs/STATUS_KO.md) · [Review corrections](docs/REVIEW_2026-09-28_KO.md)

## Goal and current implementation

The goal is to let Android Auto navigation, initially Naver Map, benefit from vehicle motion during GNSS outages. Other apps and other firmware versions are separate compatibility questions.

The source baseline contains an offline DR core, version-specific ARM hooks, automatic observation, a bounded journal, an installer, and a PC log analyzer. SCRUB removes optional stale speed/bearing fields from selected mode-0 cached locations; it does not generate a new position. SHADOW currently behaves as observation only. Runtime ASSIST is blocked in code and configuration.

The original 0.1 baseline is retained in Git history. The subsequent loader, collector and recovery changes have separate PRs and verification records. Branch contents do not imply that those PRs have been merged into master.

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
