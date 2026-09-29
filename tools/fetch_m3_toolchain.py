#!/usr/bin/env python3
"""Fetch the pinned Mazda GCC 4.9.1 build subset and verify every Git blob.

No root privileges or third-party Python packages are required. This downloads
host-executable compiler binaries from the named upstream; it does not run them.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import tempfile
import time
import urllib.parse
import urllib.request

COMMIT = "61ec0343de84f6fc7c46840056df1d600d44be8a"
REPOSITORY = "https://github.com/lmagder/m3-toolchain"
RAW = "https://raw.githubusercontent.com/lmagder/m3-toolchain/" + COMMIT + "/"
TREE_URL = ("https://api.github.com/repos/lmagder/m3-toolchain/git/trees/" +
            COMMIT + "?recursive=1")


def wanted(path):
    if path.startswith(("bin/", "lib/", "libexec/")):
        return True
    if "/include/c++/" in path:
        return True
    if path.startswith("arm-") and "/bin/" in path and "/sysroot/" not in path:
        return True
    if "/sysroot/lib/" in path:
        return True
    if "/sysroot/usr/include/" in path:
        suffix = path.split("/sysroot/usr/include/", 1)[1]
        return "/" not in suffix or suffix.split("/", 1)[0] in (
            "bits", "sys", "asm", "asm-generic", "linux", "gnu", "arpa",
            "net", "netinet", "rpc", "rpcsvc", "dbus-1.0")
    if "/sysroot/usr/lib/" in path:
        suffix = path.split("/sysroot/usr/lib/", 1)[1]
        libraries = ("libc.", "libc_", "libstdc++", "libgcc", "libpthread",
                     "libm.", "libdl.", "librt.", "libdbus", "libexpat",
                     "crt", "Scrt", "Mcrt", "libatomic")
        return (("/" not in suffix and suffix.startswith(libraries)) or
                suffix.startswith(("dbus-1.0/", "pkgconfig/dbus-1")))
    target = "arm-cortexa9_neon-linux-gnueabi"
    return path in ("README.md", "ct-ng.config", target + "/lib",
                    target + "/lib32", target + "/lib64",
                    target + "/sysroot/lib32", target + "/sysroot/lib64",
                    target + "/sysroot/usr/lib32", target + "/sysroot/usr/lib64")


def get(url):
    for attempt in range(4):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "mx5-aa-dr-build"})
            with urllib.request.urlopen(request, timeout=45) as response:
                return response.read()
        except Exception:
            if attempt == 3:
                raise
            time.sleep(attempt + 1)


def matches(data, entry):
    header = b"blob " + str(len(data)).encode("ascii") + b"\0"
    return hashlib.sha1(header + data).hexdigest() == entry["sha"]


def fetch_entry(root, entry):
    relative = Path(entry["path"])
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("Invalid upstream path: " + str(relative))
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        data = os.readlink(str(path)).encode("utf-8")
    elif path.is_file():
        data = path.read_bytes()
    else:
        data = b""
    if not matches(data, entry):
        data = get(RAW + urllib.parse.quote(entry["path"]))
        if not matches(data, entry):
            raise ValueError("Git blob checksum mismatch: " + entry["path"])
        temporary = path.with_name(path.name + ".download-tmp")
        if temporary.is_symlink():
            temporary.unlink()
        if entry["mode"] == "120000":
            if temporary.exists():
                temporary.unlink()
            temporary.symlink_to(data.decode("utf-8"))
        else:
            temporary.write_bytes(data)
            temporary.chmod(0o755 if entry["mode"] == "100755" else 0o644)
        os.replace(str(temporary), str(path))
    # A cached correct binary may still lack its execute bit. Always restore it.
    if entry["mode"] != "120000":
        path.chmod(0o755 if entry["mode"] == "100755" else 0o644)
    return entry["path"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", nargs="?", type=Path,
                        default=Path(__file__).resolve().parent / "m3-toolchain")
    parser.add_argument("--jobs", type=int, default=16,
                        help="parallel downloads (1-64, default: 16)")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        parser.error("--jobs must be between 1 and 64")
    root = args.destination.resolve()
    root.mkdir(parents=True, exist_ok=True)
    cache = root / "source_tree.json"
    if cache.exists():
        tree = json.loads(cache.read_text())
        if tree.get("source_commit") != COMMIT:
            raise ValueError("Cached source tree belongs to a different commit")
    else:
        tree = json.loads(get(TREE_URL).decode("utf-8"))
        tree["source_commit"] = COMMIT
        if tree.get("truncated") or "tree" not in tree:
            raise ValueError("GitHub returned an incomplete source tree")
        cache.write_text(json.dumps(tree, indent=2) + "\n")
    if tree.get("truncated"):
        raise ValueError("Cached source tree is incomplete")
    selected = [entry for entry in tree["tree"]
                if entry["type"] == "blob" and wanted(entry["path"])]
    # Linux headers include distinct names such as xt_CONNMARK.h/xt_connmark.h.
    # On the default macOS filesystem these overwrite one another, including
    # through a Docker bind mount. Download into Linux storage instead.
    with tempfile.TemporaryDirectory(prefix=".case-check-", dir=root) as probe:
        lower = Path(probe) / "lower"
        lower.write_bytes(b"case probe")
        if lower.with_name("LOWER").exists():
            raise ValueError("The pinned toolchain requires a case-sensitive filesystem; "
                             "use Linux container storage, not a macOS bind mount")
    print("Verifying/downloading {} files from {}".format(len(selected), COMMIT), flush=True)
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(fetch_entry, root, entry): entry for entry in selected}
        for count, future in enumerate(concurrent.futures.as_completed(futures), 1):
            try:
                future.result()
            except Exception as error:
                errors.append((futures[future]["path"], str(error)))
                print("FAILED {}: {}".format(*errors[-1]), flush=True)
            if count % 100 == 0:
                print("{}/{}".format(count, len(selected)), flush=True)
    if errors:
        raise SystemExit("{} file(s) failed; rerun to resume".format(len(errors)))
    (root / "SOURCE_COMMIT").write_text(COMMIT + "\n")
    (root / "SUBSET_MANIFEST.json").write_text(json.dumps(selected, indent=2) + "\n")
    print("Ready: " + str(root), flush=True)


if __name__ == "__main__":
    main()
