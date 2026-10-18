// The owner marker acc-cap-off (trial menu 10, validation/TUNNEL_REPORT_CAP_2026-10-12.md).
// The diagnostic arms of menu 9 (marker 'diag') were removed for v1.0.0
// (validation/V1_A_PRODUCT_HARDENING_2026-10-18.md); a stale 'diag' file is
// not the off marker. Host test, no OEM code.
#include "runtime/acc_cap.h"
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
static void acc_cap_marker_cases() {
    // acc-cap-off: present (any type) or unknown = the ceiling is off.
    char tmpl[] = "/tmp/mx5dr-acccap-XXXXXX";
    assert(mkdtemp(tmpl));
    const std::string root = tmpl, path = root + "/acc-cap-off";
    assert(!R::acc_cap_off_marker(root.c_str()));
    assert(R::acc_cap_off_marker(0));
    write_file(path, "off\n", 4); assert(R::acc_cap_off_marker(root.c_str()));
    write_file(path, "", 0);      assert(R::acc_cap_off_marker(root.c_str()));
    unlink(path.c_str());
    assert(symlink((root + "/missing").c_str(), path.c_str()) == 0);   // dangling link
    assert(R::acc_cap_off_marker(root.c_str()));
    unlink(path.c_str());
    assert(mkdir(path.c_str(), 0700) == 0);
    assert(R::acc_cap_off_marker(root.c_str()));
    rmdir(path.c_str());
    assert(!R::acc_cap_off_marker(root.c_str()));
    // A root that is not a directory: lstat fails with ENOTDIR, not ENOENT.
    write_file(root + "/file", "x", 1);
    assert(R::acc_cap_off_marker((root + "/file").c_str()));
    unlink((root + "/file").c_str());
    // A stale menu 9 marker (the old ON content) is not the off marker.
    write_file(root + "/diag", "on\n", 3);
    assert(!R::acc_cap_off_marker(root.c_str()));
    unlink((root + "/diag").c_str());
    assert(!strcmp(R::ACC_CAP_OFF_NAME, "acc-cap-off"));
    rmdir(root.c_str());
}
int main() {
    acc_cap_marker_cases();
    puts("acc-cap-off marker tests passed");
    return 0;
}
