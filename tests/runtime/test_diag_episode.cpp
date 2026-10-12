// Diagnostic BETA build (validation/DIAG_BUILD_A20_S5_2026-10-12.md): the
// controller's episode boundaries and arm assignment, driven through the
// position stream only (no OEM code). Pairs with test_diag (pure assignment),
// the adapter case beta_diag_sat (satellite rewrite) and the A20 cap checks in
// test_beta.
#include "runtime/beta_controller.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
namespace A = mx5::adapter;
namespace R = mx5::runtime;
struct J { std::vector<std::string> rows; void line(const char* p) { rows.push_back(p); } void fail() { assert(false); } };
static uint64_t clockfn(void*) { return 1; }
static bool provenance(void*, const A::PositionContext&, A::Provenance*, void*) { return false; }
static void eventfn(void*, const char*) {}
static int32_t lower(void*, A::VehicleData*) { return 0; }
static void pos(R::BetaController& c, J& j, uint64_t t, A::PositionClass cls) {
    A::Observation o = A::Observation(); o.kind = A::Observation::POSITION; o.mono_ns = t;
    o.original_mode = cls == A::POSITION_LOST ? 0 : 1; o.position_class = cls;
    o.prediction_generation = A::generation(); c.position(j, o, t);
}
static const uint64_t S = 1000000000ULL;
static size_t count(const J& j, const char* needle) {
    size_t n = 0;
    for (size_t k = 0; k < j.rows.size(); ++k) if (j.rows[k].find(needle) != std::string::npos) ++n;
    return n;
}
int main(int argc, char** argv) {
    A::Options opt = A::Options(); opt.allow_beta = true; opt.clock = clockfn; opt.provenance = provenance;
    opt.max_snapshot_age_ns = 1; opt.beta_event = eventfn; assert(A::configure(lower, opt));
    R::BetaShared sh = {}; R::BetaController c(sh); J j;
    assert(c.set_diag(true, "episode-boot")); assert(c.enable(j, 1, 0));
    assert(c.diag_arm() == A::DIAG_ARM_NONE && c.diag_episodes() == 0);   // no episode before a GPS loss
    // Episode 1: a loss stays one episode with one arm.
    pos(c, j, 1 * S, A::POSITION_LOST); const A::DiagArm a1 = c.diag_arm();
    assert(c.diag_episodes() == 1 && a1 != A::DIAG_ARM_NONE);
    pos(c, j, 2 * S, A::POSITION_LOST); assert(c.diag_arm() == a1 && c.diag_episodes() == 1);
    // A reacquisition shorter than the rejoin window (10 s) continues it.
    pos(c, j, 3 * S, A::POSITION_FIX); assert(c.diag_arm() == A::DIAG_ARM_NONE);
    pos(c, j, 12999999999ULL, A::POSITION_LOST); assert(c.diag_arm() == a1 && c.diag_episodes() == 1);
    // Exactly the window (10 s after the end): a NEW episode.
    pos(c, j, 14 * S, A::POSITION_FIX);
    pos(c, j, 24 * S, A::POSITION_LOST); const A::DiagArm a2 = c.diag_arm(); assert(c.diag_episodes() == 2);
    pos(c, j, 25 * S, A::POSITION_FIX);
    pos(c, j, 35 * S, A::POSITION_LOST); const A::DiagArm a3 = c.diag_arm(); assert(c.diag_episodes() == 3);
    pos(c, j, 36 * S, A::POSITION_FIX);
    // The first block of three episodes holds each arm exactly once.
    assert(a1 != a2 && a2 != a3 && a1 != a3);
    // Rows: three assignments, one continuation, an end row per closed episode.
    assert(count(j, "\"kind\":\"diag_episode\"") >= 7);
    assert(count(j, "\"event\":\"assigned\"") == 3);
    assert(count(j, "\"event\":\"continued\"") == 1);
    assert(count(j, "\"event\":\"ended\"") >= 3);
    // The same boot id gives the same arms in a second controller (deterministic).
    { R::BetaShared sh2 = {}; R::BetaController d(sh2); J j2;
      assert(d.set_diag(true, "episode-boot")); assert(d.enable(j2, 1, 0));
      pos(d, j2, 1 * S, A::POSITION_LOST); assert(d.diag_arm() == a1);
      pos(d, j2, 2 * S, A::POSITION_FIX);
      pos(d, j2, 20 * S, A::POSITION_LOST); assert(d.diag_arm() == a2); }
    // Diagnostic off (the default): no episode, no row, no arm.
    { R::BetaShared sh3 = {}; R::BetaController e(sh3); J j3; assert(e.enable(j3, 1, 0));
      pos(e, j3, 1 * S, A::POSITION_LOST);
      assert(!e.diag() && e.diag_arm() == A::DIAG_ARM_NONE && e.diag_episodes() == 0);
      assert(count(j3, "diag_episode") == 0); }
    // set_diag only before enable().
    { R::BetaShared sh4 = {}; R::BetaController f(sh4); J j4; assert(f.enable(j4, 1, 0)); assert(!f.set_diag(true, "x")); }
    if (argc > 1 && !strcmp(argv[1], "rows"))   // the analyzer cross-check reads these
        for (size_t k = 0; k < j.rows.size(); ++k) puts(j.rows[k].c_str());
    else puts("diag episode tests passed");
    return 0;
}
