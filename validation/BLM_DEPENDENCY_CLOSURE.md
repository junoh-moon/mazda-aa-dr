# NA74.00.324A stock BLM static dependency check

The target BLM's strong undefined dynamic symbols have matching exports in its stock dependency closure. When the stock `sm_svclauncher` dependency closure is also considered as a candidate process-global scope, every strong undefined symbol in the combined set has a compatible exported name and requested GNU symbol version. This is static compatibility evidence, not a successful loader/preload execution test.

## Inputs and reproducibility

- Exact target: `fix_inputs/evidence/stock_reference/jci/aapa/blmjciaapa.so`.
- SHA-256: `10e7235bfce075b44c1a8ffc99bbc9b63af85d9262df868ca1874ec36d8d3b71`. It exactly matches the restored firmware rootfs BLM.
- Restored rootfs: `fix_inputs/fullfirmware/rootfs`; original archive links are resolved virtually using `rootfs-symlink-manifest.json`, without creating host symlinks.
- `resources/files.ini.gz` was parsed as CSV. All 1,184 `Copy` destinations were indexed as a virtual overlay. No needed library resolved to this overlay, so no resources payload had to be materialized. No updater instructions were run.
- Authored checker: `tools/check_blm_dependency_closure.py`.
- Detailed authored metadata: `fix_inputs/blm_dependency_closure.json`.

From repository root, run with your own authorized firmware copy. The `--base` directory must contain `rootfs/`, `rootfs-symlink-manifest.json` (a list of objects with `path` and `target`) and `decrypted.zip` from the firmware restoration. These private inputs and generated detailed metadata are not distributed here:

```sh
python3 tools/check_blm_dependency_closure.py \
  --base /path/to/private/restored-firmware \
  --target /path/to/private/stock/blmjciaapa.so \
  --output /path/to/private/blm_dependency_closure.json
```

The script uses host `readelf`, Python's standard library and the previously decrypted stock archive. It never executes firmware binaries. It checks GLOBAL/UNIQUE undefined dynamic symbols against GLOBAL/WEAK/UNIQUE visible exports. A versioned import requires the same symbol version; an unversioned import requires an unversioned or default-version export. Required GNU version-definition names are separately checked on the named dependency file. Weak undefined symbols are excluded from the strong-import result.

## Results: scopes kept separate

| Static set | Objects | Strong undefined records | Unmatched records |
|---|---:|---:|---:|
| BLM itself, checked against BLM dependency closure | 1 target / 48 available objects | 244 | 0 |
| Whole BLM dependency closure | 48 | 3,465 | 24 |
| BLM + stock launcher dependency closures | 50 | 3,641 | 0 |

BLM has 36 direct `DT_NEEDED` entries. Its recursive closure contains 261 dependency edges with zero missing library files. All 174 GNU version-need checks pass. All 48 objects are ELF32 ARM, and none has RPATH/RUNPATH. The sole duplicate library candidate, `libjciaapaudio_client.so` under `/jci/lib` and `/usr/lib`, has identical SHA-256 at both locations.

The 24 unmatched records in the BLM-only closure are 12 `COMMON_UTIL_List_*` functions, each imported by both `libjcirm_consumer.so` and `libjciupdatea_client.so`: Delete, GetIndex, PopBack, PopFront, PushBack, New, GetBack, PushIndex, GetFront, PushFront, PopIndex and Size. They are exported by `libjcicommon_util.so`, which is outside BLM's own recursive `DT_NEEDED` set.

`/jci/sm/sm_svclauncher` directly declares `libjcicommon_util.so` in `DT_NEEDED`. Its own dependency closure has 13 objects, zero missing dependency files and 48 passing version checks. Adding that launcher closure to the BLM candidate scope resolves all 24 records through `/jci/lib/libjcicommon_util.so`; no strong unmatched records remain anywhere in the combined set. The launcher itself does not need to export those list functions: its directly linked library exports them.

## Version evidence

- **libdbus 1.6.4:** the actual file is `/usr/lib/libdbus-1.so.3.7.2`, SONAME `libdbus-1.so.3`. `dbus_get_version` at virtual address `0x150e0` conditionally writes immediate values 1, 6 and 4 to its three non-null output pointers. The script reads and verifies this ARM instruction pattern without executing it. The filename's `3.7.2` is the shared-library ABI filename, not the upstream D-Bus release version.
- **glibc 2.11.1:** `/lib/libc-2.11.1.so` contains an explicit GNU C Library release banner naming 2.11.1. The virtual `/lib/libc.so.6` link points to that file. `/lib/ld-linux.so.3` points to `ld-2.11.1.so`.
- BLM requires `GLIBC_2.4` from libc, pthread and librt; `GCC_3.5`; and `GLIBCXX_3.4`, `GLIBCXX_3.4.14`, `CXXABI_1.3`, `CXXABI_ARM_1.3.3`. All requested definitions and BLM imported symbol/version pairs have stock providers.

## Scope limits

The modeled search order follows the fixed directories in stock `/etc/profile` followed by `/lib` and `/usr/lib`: `/jci/lib`, `/jci/opera/3rdpartylibs/freetype`, `/usr/lib/imx-mm/audio-codec`, `/usr/lib/imx-mm/parser`, `/data_persist/dev/lib`, then standard directories. The profile appends an inherited `LD_LIBRARY_PATH`; its actual runtime value is unknown and is not modeled. Neither `ld.so.cache`, existing preload libraries nor previously loaded objects are simulated.

The launcher closure is a **candidate process-global scope**, based on its actual `DT_NEEDED` metadata. This check does not establish that this exact executable/path is the vehicle's active process, nor inspect its observed loaded-object map, `dlopen` flags, runtime interposition order, relocations or constructors. Symbol type/size/behavior, hardware ABI and C++ semantic compatibility are not established by name/version matching. The existing touch preload and the proposed hook's coexistence, ordering and runtime behavior remain separate validation work. No firmware or updater was executed, no project branch was changed, and no OEM binary is included in this authored report.
