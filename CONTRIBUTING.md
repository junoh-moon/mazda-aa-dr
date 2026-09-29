# Contributing and reviewing

Start with [current status](docs/STATUS_KO.md). Review one problem at a time, linking the affected source and acceptance criteria in [the review record](docs/REVIEW_2026-09-28_KO.md).

## Host validation

Linux requirements: C99/C++11 compiler, make, Python 3, pkg-config, D-Bus development headers/library. On Debian/Ubuntu these typically come from `build-essential`, `python3`, `pkg-config`, and `libdbus-1-dev`.

```sh
make test
```

The unchanged 0.1 packaging tests look for stock files at `../design_inputs/evidence/stock_reference/` relative to the checkout. They require the paths listed in `packaging/firmware.sha256`, plus `jci/version.ini`, `jci/sm/sm.conf`, and `jci/sm/sm_WCP.conf`. Obtain matching inputs independently and keep them outside Git. Those tests inspect/copy fixtures and never execute OEM binaries. Missing fixtures cause explicit skips.

For a host without D-Bus headers, the limited subset below is available; it is **not** the full suite:

```sh
make test-core test-adapter test-integration test-tools test-packaging
```

Pinned ARM compiler and QEMU instructions are in [toolchain notes](docs/toolchain.md). An ARM smoke test of non-target `dlopen` calls does not cover the OEM target's OFF/NOW/fallback behavior.

## Changes and evidence

For installation ZIPs and GitHub Releases, follow [the release procedure](docs/RELEASING_KO.md): pin the source commit, build and validate the actual artifacts, package checksums, publish a tagged pre-release, and verify the downloaded assets. Merging source does not update an existing release.

PRs should state the observed problem, behavior before/after, checks actually run, skipped checks, and what remains uncertain. Source, design and current status must agree. Keep review discussion attached to a commit when it depends on exact source. Use issue acceptance criteria rather than treating a general test PASS as resolution.

Do not attach location traces without removing personal routes/identifiers. Prefer synthetic reproduction inputs. Do not upload stock firmware or extracted OEM components. No project license is selected yet; preserve external attribution and do not apply a blanket license to third-party materials.
