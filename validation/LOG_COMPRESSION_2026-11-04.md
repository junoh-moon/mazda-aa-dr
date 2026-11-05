# Log compression (2026-11-04, v1.0.6 candidate)

Owner request 2026-11-04: compress logs instead of just enlarging them, and make
`SAVE LOGS` faster. Two problems from the 2026-11-04 vehicle export (v1.0.5,
boot 9258ddfb, ~96 min):

- **Retention.** Continuous GPS (collector `position_poll`, ~1 Hz) lives only
  in the collector ring of 2 × 1 MiB at ~717 B/s, about 48 min. The first 48 min
  of the drive, including an 18-min stop-free segment, had rotated out before
  the export. The trace keeps FIX positions only around events (quiet profile),
  so that segment had no heading reference for the yaw-zero test.
- **Export size.** The 15.4 MB tar held 7.6 MB of journals, 4.9 MB of
  installation (4.5 MB of it product binaries identical to the USB bundle),
  2.2 MB of OEM log copies and 0.7 MB of other diagnostics. Of the OEM copies,
  15 were 128 KiB *tails* of `.tar.gz` error logs larger than 128 KiB. The tail
  of a gzip stream cannot be decoded, so ~1.9 MB was unreadable.

## Change

1. **Collector gzip archives** (`src/collector/collector.cpp`,
   `src/runtime/zlib_dyn.h`).
   - Before a rotation overwrites `collector.1.jsonl`, the collector gzips it to
     `collector.2.jsonl.gz`. Older archives shift up to `collector.65.jsonl.gz`.
   - The archive set is bounded to 64 files and 2 MiB of compressed bytes. The
     newest archive is always kept.
   - Each attempt writes one `collector_archive` row. Its result is `ok`,
     `unavailable`, `space`, `failed`, `shift` or `rename`.
   - Any failure falls back to the old two-file ring: the plain file is
     overwritten exactly as before. Nothing else in the collector changed.
     Rows, envelopes, the live file and `collector.1` stay plain, so the parked
     check (`trial_status`) is unaffected.
   - The compression runs in the collector's own process. A worst-case (source
     size + 64 KiB) free-space check keeps the 8 MiB storage reserve.
2. **zlib source.**
   - 74.00.324A ships `/usr/lib/libz.so.1` (zlib 1.2.5). The 2026-11-04
     diagnostics show it already mapped by `L_jciAAPA`, `L_jciVBS`, `L_jciLDS`
     and `aap_service`; `/lib/libdl-2.11.1.so` is mapped too.
   - The toolchain has `zlib.h` but no libz, so nothing links against it. The
     collector `dlopen`s `libz.so.1`, falling back to `/usr/lib/libz.so.1.2.5`.
     Its only new NEEDED entry is `libdl.so.2`.
   - The AA runtime does not include this code.
3. **Export helper mode.**
   - `mx5dr-collector --gzip` / `--gunzip` streams stdin to stdout through
     the same zlib. Exit 69 means zlib is unavailable; 1 means the stream failed.
   - This mode reads no config, bus, logs or accounts.
   - `common.sh: prepare_gzip_helper` checks the USB copy against
     `mx5dr-collector.sha256`, copies it to `/tmp` (as the hash helper does) and
     probes it once.
4. **Export** (`packaging/export_logs.sh`).
   - The whole tar goes through one gzip stream into
     `mx5dr-logs-….tar.gz`. Without the helper the archive stays a plain
     `.tar`, as before. `collection.txt` records `archive_compression=`.
   - Because ash has no pipefail, tar's status is passed through a file. A
     tar or gzip failure keeps the `.partial` path as before.
   - Product binaries (`libmx5dr*.so`, `mx5dr-collector`, `guard/mx5dr-guard`,
     `tools/mx5dr-sha256`) whose sha256 matches the USB bundle are not
     archived. Instead `installation.<path>=bundle_copy_not_archived sha256:… bytes=…`
     is recorded. A binary that differs is archived.
   - `guard/` and `tools/` are listed one level deep, with their directory
     metadata recorded. Every other installation file is archived as before.
   - OEM `.gz`/`.tgz` logs larger than 128 KiB are recorded as
     `compressed_not_captured source_bytes=N` instead of an undecodable tail.
     Smaller ones are copied whole, as before.
5. **Prune** (`packaging/prune_logs.sh`).
   - It accepts `.tar` or `.tar.gz`. For `.tar.gz` the whole stream must decode
     through the helper ("does not decompress; nothing deleted"), and the
     listing comes from the decoded stream.
   - The collector archives are deletable journals and are checked with the
     same size rule.
   - `trial` passes `.tar.gz` paths.
6. **Install space check** (`common.sh`). It counts the 2 MiB archive budget
   and transfers ownership of existing archives with the collector files.
7. **Analyzer** (`tools/analyze_logs.py`).
   - It reads `collector.N.jsonl.gz`, as files and as members of `.tar`/`.tar.gz`
     archives, within the per-file byte budget.
   - Archives order before `collector.1`/`.0`.
   - `collector_archive` rows are counted in `collector.archive_results`.

## Measured on the 2026-11-04 vehicle data (offline)

- `collector.1.jsonl` (1,048,454 B) gzips to 71,806 B (14.6×). The
  `trace.0.jsonl` of the same export (5.4 MB) gzips to 512 KB (10.6×).
- **Retention.** At the drive's 717 B/s one 1 MiB collector file is ~24 min.
  The 2 MiB archive budget at ~72 KB per file holds ~29 archives (~11.5 h),
  plus the two plain files (~48 min). Without zlib it stays ~48 min. Short
  boots also rotate at start, so many short boots use archive slots; the
  64-file cap is reached only by more than 64 boots or rotations.
- **Export size.** The same export rebuilt without the bundle binaries and
  undecodable OEM tails, then gzipped (level 6), comes to 1,477,958 B instead of
  15.4 MB (10.4× less to write to the USB).
- **Analyzer.** It reports the same result for that export's collector file as
  plain `collector.1.jsonl` and as `collector.2.jsonl.gz`; only the file names
  differ.

## Tests (host)

- Collector (host build; this machine has no libdbus-dev, so the package
  headers were unpacked into the scratchpad and the host libdbus was linked):
  `make test-collector`, 49 Python tests OK, including the new
  `GzipHelperTests`.
  - Round trip against Python gzip, concatenated members, empty input.
  - Truncated, corrupt, trailing-junk and empty gzip all give 1; zlib
    unavailable gives 69; an extra argument gives 64.
- `test_collector_journal` (archive order and content, `collector_archive`
  rows, the zlib-unavailable fallback, the byte budget) and the five storage
  scenarios: OK.
- `tests/packaging/test_trial_menu.py`: 126 OK. Every TrialMenuTests case runs
  twice, plain and with the host helper (`CompressedExportTests`). New cases:
  - compressed export plus prune, including `collector.2`/`.17.jsonl.gz`;
  - prune refusing a `.tar.gz` that does not decompress;
  - bundle-identical binaries recorded and not archived, a changed one
    archived;
  - a large OEM `.tar.gz` listed and not tail-copied.
- `tests/tools/test_analyze_logs.py`: 38 OK. New: archives order before
  `.1`/`.0` (directory and `.tar.gz`); a cut archive is `input_error`.
- ARM build (`tools/build_arm.py`, pinned toolchain): verified. Collector
  NEEDED: libdbus-1.so.3, libdl.so.2, libpthread.so.0, librt.so.1, libm.so.6,
  libc.so.6, ld-linux.so.3.
- Full packaging suite: see the release record.

## Independent review (subagent, read-only) and fixes

Nothing serious was found. Five low or low-medium findings:

1. **Budget gaps.** When one archive went over the budget, a smaller, older
   one could still be kept. **Fixed:** from the first overflow on, every older
   archive is deleted, and the journal test asserts contiguous numbering.
2. **Note row after a stop row.** A `collector_archive` note could follow the
   final `collector_stop` when that row itself rotated. **Fixed:** the note is
   now written before the row that caused the rotation (tested).
3. **Duplicate archive after a power cut.** A power cut between the archive
   rename and the plain shift could archive the same rows twice. **Fixed:**
   `collector.1` is unlinked right after its archive is in place.
4. **Space check.** Install and arm now ask for 2 MiB more, also where zlib is
   missing. Accepted: this is the actual budget. The transient
   `collector.archive.tmp` falls within the 8 MiB reserve the writers keep.
5. **Gzip inside the poll loop.** A rotation gzips in the poll loop (1 MiB in
   the persistent profile, 4 MiB in one-boot trials). Accepted. On the host,
   1 MiB takes ~20 ms. On the CMU's Cortex-A9 it is expected to be well under
   a second but was not measured.

## ARM user-mode emulation (qemu-arm 8.2.2, unpacked without root)

- **Runtime used:** the pinned toolchain's sysroot (glibc 2.11, as the CMU's
  2.11.1) plus Debian armel zlib 1.2.7. That is the closest public build to the
  CMU's 1.2.5; the ABI is the same.
- **ARM `mx5dr-collector --gzip`/`--gunzip`:** a 300 KB vehicle collector
  slice round-trips byte-identically, Python gzip decodes the output, and the
  call returns 69 without zlib.
- **ARM `test_collector_journal`**, cross-built with the pinned toolchain:
  the archive order and content, the zlib fallback, the budget without gaps,
  the note order and the five storage scenarios all pass.
- **Shell idioms under Debian armel BusyBox 1.20.2** (the closest public
  static build; the CMU runs 1.19.2): the tar status through a file in a pipe
  works, and `tar -tvf -` from a pipe puts the size in column 3 and the path in
  the last column, as prune expects.

- **`tests/packaging/test_trial_menu.py` with every shell command a BusyBox
  1.20.2 applet under qemu** (sh, tar, awk, stat, flock, …): 126 OK.
- **The 65 `CompressedExportTests` with that BusyBox and the cross-built ARM
  `mx5dr-collector` as the gzip helper:** 65 OK. That helper is
  `MX5DR_TEST_GZIP_OVERRIDE`, a qemu wrapper.

## Not run

- Vehicle (no drive is requested).
- The CMU's own zlib 1.2.5 and BusyBox 1.19.2: only the closest public builds
  ran (zlib 1.2.7, BusyBox 1.20.2). If either differs on the vehicle, the
  fallbacks keep the earlier behaviour: a plain ring and a plain `.tar`.
- Stock BusyBox harnesses (G7–G9, G16): the private stock tree is absent.
