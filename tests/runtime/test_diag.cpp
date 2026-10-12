// Diagnostic BETA build (validation/DIAG_BUILD_A20_S5_2026-10-12.md): arm
// assignment and the owner marker. Host test, no OEM code.
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
static void arm_cases() {
    // Deterministic and fixed for an episode index.
    for (uint32_t seed = 0; seed < 64; ++seed)
        for (uint32_t e = 0; e < 30; ++e)
            assert(R::diag_arm_for(seed * 2654435761u, e) == R::diag_arm_for(seed * 2654435761u, e));
    // Every block of three consecutive episodes holds each arm exactly once.
    unsigned perms_seen[6] = {0, 0, 0, 0, 0, 0};
    for (uint32_t seed = 0; seed < 200; ++seed) {
        const uint32_t s = R::diag_seed(("boot-" + std::to_string(seed)).c_str());
        for (uint32_t b = 0; b < 12; ++b) {
            unsigned count[4] = {0, 0, 0, 0}, block = 99, perm = 99;
            for (uint32_t k = 0; k < 3; ++k) {
                unsigned bl = 0, pe = 0;
                const A::DiagArm arm = R::diag_arm_for(s, b * 3 + k, &bl, &pe);
                assert(arm >= A::DIAG_ARM_CONTROL && arm <= A::DIAG_ARM_S5);
                ++count[arm];
                if (k == 0) { block = bl; perm = pe; }
                assert(bl == b && pe == perm && bl == block);
                if (k == 0) ++perms_seen[perm];
            }
            assert(count[A::DIAG_ARM_CONTROL] == 1 && count[A::DIAG_ARM_A20] == 1 && count[A::DIAG_ARM_S5] == 1);
        }
    }
    // All six permutations occur (the order within a block is not constant).
    for (unsigned p = 0; p < 6; ++p) assert(perms_seen[p] > 0);
    // Different boot ids give different sequences somewhere.
    bool differ = false;
    for (uint32_t e = 0; e < 12; ++e)
        if (R::diag_arm_for(R::diag_seed("aaaaaaaa-0000"), e) != R::diag_arm_for(R::diag_seed("bbbbbbbb-1111"), e)) differ = true;
    assert(differ);
    assert(!strcmp(A::diag_arm_name(A::DIAG_ARM_S5), "S5") && !strcmp(A::diag_arm_name(A::DIAG_ARM_A20), "A20"));
    assert(R::DIAG_A20_CAP_M == 20.0 && A::DIAG_SAT_IN_USE == 5 && A::DIAG_SAT_LENGTH == 864 && A::DIAG_SAT_TYPE == 21);
}
int main() {
    marker_cases();
    arm_cases();
    puts("diag tests passed");
    return 0;
}
