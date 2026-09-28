# Provenance and public-import boundaries

The implementation baseline is the previously generated `MX5_v74_AA_DR_Experimental_0.1.zip`, SHA-256 `e2faf183de1c71c32b0afc3580e86571e589a1f5b4952581099ae73261423c7b`. The authored source was developed for this project with AI assistance and independent Astra reviews. Claude and a physical vehicle were not used in that implementation.

`validation/source-import.json` records the original archive file hashes. `src/`, `tests/`, `tools/`, `packaging/`, and `Makefile` are imported byte-for-byte. README and status documents are updated; historical documents receive supersession notices. Historical `validation/arm-build.txt` has its transient workspace prefix replaced by `<WORKSPACE>`; its commands/results otherwise retain their historical meaning. The archive's original SHA256SUMS is not presented as a checksum of this modified public tree.

The historical detailed design is preserved in `docs/archive/DESIGN_V1_KO.md`. References to private evidence bundles are descriptions of the original analysis inputs, not downloadable files included in this repository. The same distinction applies to historical `bundle/` references. The compiled project library is omitted as a generated artifact, not misidentified as an OEM library.

## Referenced projects

- [VitaliyKurokhtin/oem-aa-mod](https://github.com/VitaliyKurokhtin/oem-aa-mod/tree/00122139bf3a27b79bb99f53e80dad8c15f7c56e): prior AA hook/lifecycle reference and the project's touchscreen-mod starting point. This does not identify the exact shim installed on a vehicle.
- [Bijan-A/oem-aa-mod-installer](https://github.com/Bijan-A/oem-aa-mod-installer): related installation context.
- [lmagder/m3-toolchain](https://github.com/lmagder/m3-toolchain/tree/61ec0343de84f6fc7c46840056df1d600d44be8a): pinned build-toolchain source; not redistributed here.
- [Trevelopment/headunit Mazda GPS client](https://github.com/Trevelopment/headunit/blob/15ec8ecaa1a74dd95362463068005becab425258/mazda/gps/mzd_gps.cpp): historical protocol reference; its units/timing/reverse behavior are not automatically adopted.

No new claim of having re-audited those upstream projects or their licenses is made by this import. Attribution is not a substitute for license review. No project license has been selected, and no blanket license is applied to Mazda or third-party components.

## Evidence limits

Stock firmware was used as input to earlier static analysis. This public source import does not include that firmware, its extracted libraries/maps or disassembly dumps. It retains hashes, version-specific offsets, narrow hook verification signatures, and authored descriptions so reviewers can inspect the design and reproduce claims with matching independently available inputs.

Historical test records are not new executions. Public-import checks are separately recorded in `validation/PUBLIC_IMPORT.md`. Test fixtures and synthetic location CSVs are not real driving data.
