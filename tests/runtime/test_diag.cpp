// Diagnostic BETA build, version 2 (validation/TUNNEL_REPORT_CAP_2026-10-12.md;
// version 1: validation/DIAG_BUILD_A20_S5_2026-10-12.md): arm assignment and
// the owner markers diag and acc-cap-off. Host test, no OEM code.
#include "runtime/diag_arms.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace R = mx5::runtime;
namespace A = mx5::adapter;

static void write_file(const std::string& path, const char* text, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    assert(f);
    assert(fwrite(text, 1, n, f) == n);
    fclose(f);
}
static void marker_cases() {
    char tmpl[] = "/tmp/mx5dr-diag-XXXXXX";
    assert(mkdtemp(tmpl));
    const std::string root = tmpl, path = root + "/diag";
    assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_ABSENT);
    assert(R::diag_marker(0) == R::DIAG_MARKER_INVALID);
    write_file(path, "on\n", 3);
    assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_ON);
    // Anything but exactly "on\n" in a regular file is not ON (fail closed).
    write_file(path, "on", 2);   assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_INVALID);
    write_file(path, "off\n", 4); assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_INVALID);
    write_file(path, "ON\n", 3);  assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_INVALID);
    write_file(path, "no\n", 3);  assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_INVALID);
    write_file(path, "on\r", 3);  assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_INVALID);
    write_file(path, "on\nx", 4); assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_INVALID);
    write_file(path, "", 0);      assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_INVALID);
    // A symlink (even to a valid marker) and a directory are refused.
    unlink(path.c_str());
    write_file(root + "/real", "on\n", 3);
    assert(symlink((root + "/real").c_str(), path.c_str()) == 0);
    assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_INVALID);
    unlink(path.c_str());
    assert(mkdir(path.c_str(), 0700) == 0);
    assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_INVALID);
    rmdir(path.c_str());
    assert(R::diag_marker(root.c_str()) == R::DIAG_MARKER_ABSENT);
    unlink((root + "/real").c_str());
    rmdir(root.c_str());
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
    // The diag marker and the off marker are independent names.
    write_file(root + "/diag", "on\n", 3);
    assert(!R::acc_cap_off_marker(root.c_str()) && R::diag_marker(root.c_str()) == R::DIAG_MARKER_ON);
    unlink((root + "/diag").c_str());
    rmdir(root.c_str());
}
static void arm_cases() {
    // Deterministic and fixed for an episode index.
    for (uint32_t seed = 0; seed < 64; ++seed)
        for (uint32_t e = 0; e < 30; ++e)
            assert(R::diag_arm_for(seed * 2654435761u, e) == R::diag_arm_for(seed * 2654435761u, e));
    // Every block of three consecutive episodes holds one CONTROL and two CAP;
    // perm is the position of CONTROL. The retired arms are never assigned.
    unsigned perms_seen[3] = {0, 0, 0}, control = 0, cap = 0;
    for (uint32_t seed = 0; seed < 200; ++seed) {
        const uint32_t s = R::diag_seed(("boot-" + std::to_string(seed)).c_str());
        for (uint32_t b = 0; b < 12; ++b) {
            unsigned c = 0, k_control = 99, perm = 99;
            for (uint32_t k = 0; k < 3; ++k) {
                unsigned bl = 0, pe = 0;
                const A::DiagArm arm = R::diag_arm_for(s, b * 3 + k, &bl, &pe);
                assert(arm == A::DIAG_ARM_CONTROL || arm == A::DIAG_ARM_CAP);
                assert(bl == b && pe <= 2);
                if (k == 0) perm = pe;
                assert(pe == perm);
                if (arm == A::DIAG_ARM_CONTROL) { ++c; k_control = k; ++control; } else ++cap;
            }
            assert(c == 1 && k_control == perm);
            ++perms_seen[perm];
        }
    }
    // CONTROL takes every position (the order within a block is not constant),
    // and over many blocks the arms are exactly one third / two thirds.
    for (unsigned p = 0; p < 3; ++p) assert(perms_seen[p] > 200);
    assert(cap == 2 * control);
    // Different boot ids give different sequences somewhere.
    bool differ = false;
    for (uint32_t e = 0; e < 30; ++e)
        if (R::diag_arm_for(R::diag_seed("aaaaaaaa-0000"), e) != R::diag_arm_for(R::diag_seed("bbbbbbbb-1111"), e)) differ = true;
    assert(differ);
    assert(!strcmp(A::diag_arm_name(A::DIAG_ARM_CAP), "CAP") && !strcmp(A::diag_arm_name(A::DIAG_ARM_CONTROL), "CONTROL"));
    // Retired beta.12 values keep their journal names (2: A20, 3: S5).
    assert(!strcmp(A::diag_arm_name(A::DiagArm(2)), "A20") && !strcmp(A::diag_arm_name(A::DiagArm(3)), "S5"));
    assert(!strcmp(A::diag_sat_outcome_name(A::DIAG_SAT_ARM_CAP), "arm_cap") &&
           !strcmp(A::diag_sat_outcome_name(A::DiagSatOutcome(1)), "applied"));
    assert(R::DIAG_VERSION == 2 && A::DIAG_SAT_LENGTH == 864 && A::DIAG_SAT_TYPE == 21);
}
int main() {
    marker_cases();
    acc_cap_marker_cases();
    arm_cases();
    puts("diag tests passed");
    return 0;
}
