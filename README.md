# mazda-aa-dr

Experimental **Android Auto (AA) / Dead Reckoning (DR)** research for first-generation Mazda Connect, targeting **NA 74.00.324A** and a 2019 MX-5 ND2.

**The current public development pre-release is
[v0.3.11-shadow.2](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.11-shadow.2).**
It preserves lexical RMC A/V status through the LDS heading assignment into AA
observation records. The final shell-only ZIP passed three stock BusyBox paths,
and the published assets were downloaded and compared with the pinned candidate;
see [publication evidence](validation/RELEASE_V0311_SHADOW2_2026-10-02.md).
Installation requires an already authorized shell that remains accessible after
reboot; this ZIP does not provide shell entry. Whole normal SM/vehicle startup and
recovery, physical sensors and phone acceptance remain unverified; live ASSIST
stays disabled. The earlier v0.3.10-shadow.1 and .2 packages remain withdrawn.

**The local source is a SHADOW trial candidate.** OFF loader handling, separate polling and a one-use pre-Service-Manager gate are implemented. Host, ARM and partial OEM execution evidence are recorded separately. A vehicle trial was attempted, but its recovered archive contains no capture journals; startup and storage remain under investigation. Sensor operation and phone acceptance are unverified. Live ASSIST remains disabled; this is not a working tunnel-navigation solution.

On 2026-10-01 the user authorized one combined vehicle installation/test before
v1.0, after feasible firmware and offline verification. This updates the earlier
firmware-only restriction for that trial, which was used by the first installation
and drive with an empty capture. Publication does not authorize repeated visits
or phone/dongle bench tests. See the [combined trial](docs/FIELD_TRIAL_KO.md) and
[v1.0 criteria](docs/V1_READINESS_KO.md). Physical sensor behavior, recovery and
phone/app acceptance remain unverified.

See [integration evidence](validation/INTEGRATION_2026-09-28.md). The gate removes our preload from persistent service configurations and consumes one explicit authorization before exposing a trial. [Original-SM retry observations](validation/SM_RETRY_2026-09-29.md) cover explicit service restarts and delayed failure policy; physical watchdog recovery remains unverified.

[한국어](README_KO.md) · [Current status / handoff](docs/STATUS_KO.md) · [Review corrections](docs/REVIEW_2026-09-28_KO.md)

The withdrawn [v0.3.10-shadow.2 installation ZIP](https://github.com/junoh-moon/mazda-aa-dr/releases/tag/v0.3.10-shadow.2)
adds guarded startup and config-binding diagnostics to the LDS observer. The
observer carries callback/cache assignment lineage and response identity into
the bounded AA journal. Original-library execution reached
the actual AA worker and matched nine responses, with authored startup boundaries.
See the [product execution](validation/LDS_PRODUCT_RUNTIME_2026-10-02.md) and
[release verification](validation/RELEASE_V0310_SHADOW2_2026-10-02.md).
It retains the explicit CMU reboot request, before/after boot-ID checks and whole-installation
startup diagnostics exported to USB. Its instructions distinguish ACC, engine-off
ON and a running engine.
It also retains the previous observation features: it
records the transport server GUID and the client's unique name at the original
AA connection registration, then records whether the actual raw send used that
same connection binding. Missing identity does not discard raw position
observations. All nine original position fields, GPS holdout call/generation
references and MODEL comparisons remain, alongside anchor/callback and
discarded-queue fixes. Physical LDS capture remains unverified; the live
qualified input provider is unimplemented and unverified, so **ASSIST stays disabled**.

**Recovering a failed v0.3.9-shadow.1 export:** replace the USB files with this
hotfix and choose `3` after the command below, including after uninstalling.
No reinstall or new drive is needed. The old menu incorrectly rejected the
normal `/proc/mounts` symlink; see the [failure and fix](validation/TRIAL_EXPORT_HOTFIX_2026-10-02.md).
The follow-up export succeeded but contained no trace or collector journals.
The user confirmed ignition off/on and a drive with a wireless AA dongle and S25;
a new CMU Linux boot was not established. See the [empty-capture analysis](validation/FIELD_V039_EMPTY_CAPTURE_2026-10-02.md)
and [persistent-storage investigation](validation/EMPTY_CAPTURE_2026-10-02.md).
Successful export does not establish capture. The `.3` bundle includes guard-marker
diagnostics, menu `5` for the original CMU reboot command and expanded debug export.
Reboot-command acceptance and a new CMU boot are checked separately.

The [original release record](validation/RELEASE_V039_2026-10-01.md) contains the source pin,
ZIP hash, final host/ARM and stock BusyBox checks, published-download verification,
and the distinct scope of earlier original-library execution.
[v0.3.8](validation/RELEASE_V038_2026-10-01.md) and
[v0.3.7](validation/RELEASE_V037_2026-10-01.md) retain their historical evidence.

For a separately arranged trial with the current .2 shell-only ZIP, use the
[power-state and installation instructions](packaging/SHELL_START_KO.md).
They require an already authorized diagnostic shell that remains accessible
after the CMU reboot. The USB menu uses `1 → 5 → 2` while parked, followed by
the documented AA/USB switch and parked export. This publication does not grant
another vehicle trial. Do not enter commands or change USB devices while driving.

Historical packaging evidence for BusyBox 1.19.2, absent sha256sum, numeric UID 0,
stock storage aliases and read-only root remains in
[the earlier installation record](validation/USB_INSTALL_2026-09-29.md).
The [OEM account correction](validation/OEM_RUNTIME_2026-09-29.md) records that
stock `cmu` is UID 0 and the collector uses the existing non-root `service`
account. Normal whole-SM and vehicle startup remain unverified.

## Goal and current implementation

The goal is to let Android Auto navigation, initially Naver Map, benefit from vehicle motion during GNSS outages. Other apps and other firmware versions are separate compatibility questions.

The source baseline contains an offline DR core, version-specific ARM hooks, automatic observation, a bounded journal, an installer, and a PC log analyzer. SCRUB removes optional stale speed/bearing fields from selected mode-0 cached locations; it does not generate a new position. The earlier v0.2.0-observe.2 release has an observation-only SHADOW placeholder; v0.3.2-shadow.1 includes the MODEL calculation and request/session observations described below. Runtime ASSIST is blocked in code and configuration.

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
| How is an installation release built and published? | [Release procedure (Korean)](docs/RELEASING_KO.md) |

## Development

On a Linux development host with a C/C++ compiler, make, Python 3, Git, pkg-config and D-Bus development headers:

```sh
make test
```

The packaging identity tests require separately obtained matching stock firmware fixtures; they explicitly skip when fixtures are absent. Skips are not passes. See [contribution notes](CONTRIBUTING.md) for the expected path and [toolchain notes](docs/toolchain.md) for the pinned ARM build. Host tests and synthetic ARM QEMU tests do not establish CMU recovery or phone/app acceptance.

## Public contents

This repository contains authored source, tests, technical interpretation, and validation records. OEM firmware, extracted libraries/maps, disassembly dumps, personal download links, vehicle location logs, the toolchain, and prebuilt installation bundles are omitted. Firmware hashes and narrow compatibility signatures remain so the target can be identified. The historical archive and imported-source identities are recorded in `validation/source-import.json`.

No project license has been selected yet. See [provenance](docs/PROVENANCE.md) for referenced projects and the limits of the imported evidence.

## Merged source: live sensor SHADOW calculation

This branch adds an existing-VBS-callback tap, raw sensor decoding, bounded local transport, time alignment and actual MODEL-domain navigation through the shared DR core. SHADOW records derived position and LOCATION bytes while forwarding OEM data unchanged. It does not promote model assumptions or receipt timestamps into qualified ASSIST. See [implementation and limits](docs/LIVE_SHADOW_2026-09-29_KO.md) and [validation](validation/LIVE_SHADOW_2026-09-29.md). This is separate from the published `v0.2.0-observe.2` release and has not run on the vehicle.
