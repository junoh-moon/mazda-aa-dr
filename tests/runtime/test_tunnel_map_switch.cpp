// The owner marker tunnelmap-off (trial menu 13,
// validation/TUNNEL_MAP_CONSTRAINT_2026-10-22.md): the map-constrained tunnel
// dead reckoning is ON by default inside the BETA tunnel replacement; while
// the marker exists (any type) the worker keeps the ordinary DR. Host test,
// no OEM code.
#include "runtime/tunnel_map_switch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace R = mx5::runtime;

static void write_file(const std::string& path, const char* text, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    assert(f);
    assert(fwrite(text, 1, n, f) == n);
    fclose(f);
}

int main(void) {
    char tmpl[] = "/tmp/mx5dr-tunnelmap-XXXXXX";
    assert(mkdtemp(tmpl));
    const std::string root = tmpl, path = root + "/tunnelmap-off";
    // No root, no marker: the constraint stays on (off_marker false).
    assert(!R::tunnel_map_off_marker(root.c_str()));
    // No root argument: fail closed towards OFF.
    assert(R::tunnel_map_off_marker(0));
    // Present as a regular file, empty file, dangling symlink or directory:
    // the constraint is off for this boot.
    write_file(path, "off\n", 4); assert(R::tunnel_map_off_marker(root.c_str()));
    write_file(path, "", 0);      assert(R::tunnel_map_off_marker(root.c_str()));
    unlink(path.c_str());
    assert(symlink((root + "/missing").c_str(), path.c_str()) == 0);
    assert(R::tunnel_map_off_marker(root.c_str()));
    unlink(path.c_str());
    assert(mkdir(path.c_str(), 0700) == 0);
    assert(R::tunnel_map_off_marker(root.c_str()));
    rmdir(path.c_str());
    assert(!R::tunnel_map_off_marker(root.c_str()));
    // A root that is not a directory: lstat fails with ENOTDIR, not ENOENT.
    write_file(root + "/file", "x", 1);
    assert(R::tunnel_map_off_marker((root + "/file").c_str()));
    // Cleanup.
    unlink((root + "/file").c_str());
    rmdir(root.c_str());
    printf("test_tunnel_map_switch ok\n");
    return 0;
}
