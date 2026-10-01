#include "sensors/lds_lineage.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <type_traits>

namespace mx5 { namespace sensors { namespace lds_lineage {
struct LdsLineageTestAccess {
    // Reach the real terminal counter state without adding a product setter.
    static void near_exhaustion(Ledger& ledger) {
        ledger.current_.write_sequence = UINT64_MAX - 1;
    }
};
} } }

namespace L = mx5::sensors::lds_lineage;
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "line %d: missing lineage behavior: %s\n", __LINE__, #expr); \
    exit(1); } } while (0)

static uint32_t bit(L::Field f) { return uint32_t(1) << unsigned(f); }

static void unknown_fields(const L::Snapshot& snapshot) {
    for (unsigned i = 0; i < L::FIELD_COUNT; ++i) {
        CHECK(snapshot.fields[i].write_sequence == 0);
        CHECK(snapshot.fields[i].observed_ns == 0);
    }
}

static void same_fields(const L::Snapshot& a, const L::Snapshot& b) {
    for (unsigned i = 0; i < L::FIELD_COUNT; ++i) {
        CHECK(a.fields[i].write_sequence == b.fields[i].write_sequence);
        CHECK(a.fields[i].observed_ns == b.fields[i].observed_ns);
    }
}

static void observed_assignment_is_not_unknown() {
    L::Ledger ledger;
    CHECK(ledger.begin_lifetime(7));
    const L::Snapshot read = ledger.snapshot();
    ledger.commit(&read, bit(L::LATITUDE) | bit(L::LONGITUDE), 100);
    const L::Snapshot response = ledger.snapshot();
    // This is an assignment identity, not a claim of fresh physical GPS.
    CHECK(response.fields[L::LATITUDE].write_sequence != 0);
    CHECK(response.lifetime == 7);
    CHECK(response.write_sequence == 1);
    CHECK(response.fields[L::LATITUDE].write_sequence == 1);
    CHECK(response.fields[L::LONGITUDE].write_sequence == 1);
    CHECK(response.fields[L::LATITUDE].observed_ns == 100);
    CHECK(response.fields[L::MODE].write_sequence == 0);
}

static void commit_inherits_the_read_copy_not_the_latest_cache() {
    L::Ledger ledger;
    CHECK(ledger.begin_lifetime(11));
    L::Snapshot read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 100) == L::COMMITTED);
    const L::Snapshot paused_read = ledger.snapshot();
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, bit(L::ALTITUDE), 200) == L::COMMITTED);
    const L::Snapshot intervening_response = ledger.snapshot();
    CHECK(intervening_response.fields[L::ALTITUDE].write_sequence == 2);
    CHECK(ledger.commit(&paused_read, bit(L::LATITUDE), 300) == L::COMMITTED);
    const L::Snapshot final_response = ledger.snapshot();
    CHECK(final_response.write_sequence == 3);
    CHECK(final_response.fields[L::ALTITUDE].write_sequence == 1);
    CHECK(final_response.fields[L::ALTITUDE].observed_ns == 100);
    CHECK(final_response.fields[L::LATITUDE].write_sequence == 3);
    CHECK(intervening_response.fields[L::ALTITUDE].write_sequence == 2);
    CHECK(paused_read.fields[L::LATITUDE].write_sequence == 1);
}

static void every_mask_inherits_only_unassigned_fields() {
    for (uint32_t mask = 0; mask <= L::ALL_FIELDS; ++mask) {
        L::Ledger ledger;
        CHECK(ledger.begin_lifetime(13));
        L::Snapshot read = ledger.snapshot();
        CHECK(ledger.commit(&read, L::ALL_FIELDS, 200) == L::COMMITTED);
        read = ledger.snapshot();
        // Observer time can go backward; it is not a physical freshness gate.
        CHECK(ledger.commit(&read, mask, 100) == L::COMMITTED);
        const L::Snapshot response = ledger.snapshot();
        CHECK(response.write_sequence == 2);
        for (unsigned i = 0; i < L::FIELD_COUNT; ++i) {
            const bool assigned = (mask & (uint32_t(1) << i)) != 0;
            CHECK(response.fields[i].write_sequence == (assigned ? 2U : 1U));
            CHECK(response.fields[i].observed_ns == (assigned ? 100U : 200U));
        }
    }
}

static void reassignment_and_queries_have_different_identity_behavior() {
    L::Ledger ledger;
    CHECK(ledger.begin_lifetime(17));
    L::Snapshot read = ledger.snapshot();
    const uint32_t mask = bit(L::MODE) | bit(L::ALTITUDE);
    CHECK(ledger.commit(&read, mask, 100) == L::COMMITTED);
    const L::Snapshot response = ledger.snapshot();
    const L::Snapshot same_response = ledger.snapshot();
    CHECK(same_response.write_sequence == response.write_sequence);
    same_fields(response, same_response);
    read = ledger.snapshot();
    // The API intentionally has no numeric values to compare. Repeating the
    // same assignments, even with an identical timestamp/value, is a new event.
    CHECK(ledger.commit(&read, mask, 100) == L::COMMITTED);
    const L::Snapshot reassigned = ledger.snapshot();
    CHECK(reassigned.fields[L::ALTITUDE].write_sequence == 2);
    CHECK(reassigned.fields[L::ALTITUDE].observed_ns == 100);
    CHECK(reassigned.fields[L::MODE].write_sequence == 2);
    CHECK(reassigned.fields[L::LATITUDE].write_sequence == 0);
    CHECK(response.fields[L::ALTITUDE].write_sequence == 1);
    // Overwriting caller storage after enqueue/commit cannot alter snapshots.
    read = L::Snapshot();
    CHECK(ledger.snapshot().fields[L::ALTITUDE].write_sequence == 2);
    CHECK(response.fields[L::ALTITUDE].write_sequence == 1);
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, bit(L::MODE), 0) == L::COMMITTED);
    CHECK(ledger.snapshot().fields[L::MODE].write_sequence == 3);
    CHECK(ledger.snapshot().fields[L::MODE].observed_ns == 0);
    // MODE says only that this callback assigned mode. It does not collapse
    // its potentially older GGA/GSA dependencies into one physical source.
    CHECK(ledger.snapshot().fields[L::ALTITUDE].write_sequence == 2);
}

static void verified_copy_only_write_restores_the_actual_read_lineage() {
    L::Ledger ledger;
    CHECK(ledger.begin_lifetime(19));
    L::Snapshot read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 100) == L::COMMITTED);
    const L::Snapshot older_read = ledger.snapshot();
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 200) == L::COMMITTED);
    CHECK(ledger.commit(&older_read, 0, 300) == L::COMMITTED);
    const L::Snapshot response = ledger.snapshot();
    CHECK(response.write_sequence == 3);
    same_fields(response, older_read);
}

static void missing_read_and_unsupported_writer_stay_unknown() {
    L::Ledger ledger;
    CHECK(ledger.commit(0, L::ALL_FIELDS, 1) == L::INACTIVE);
    CHECK(ledger.snapshot().lifetime == 0);
    unknown_fields(ledger.snapshot());
    CHECK(ledger.begin_lifetime(23));
    L::Snapshot read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 100) == L::COMMITTED);
    // A missing actual read must not become a guessed full assignment.
    CHECK(ledger.commit(0, L::ALL_FIELDS, 200) == L::UNKNOWN_READ);
    CHECK(ledger.snapshot().write_sequence == 2);
    unknown_fields(ledger.snapshot());
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, bit(L::ALTITUDE), 300) == L::COMMITTED);
    CHECK(ledger.snapshot().fields[L::ALTITUDE].write_sequence == 3);
    CHECK(ledger.snapshot().fields[L::LATITUDE].write_sequence == 0);
    // An unsupported actual writer is represented by missing proof, not mask0.
    CHECK(ledger.commit(0, 0, 400) == L::UNKNOWN_READ);
    CHECK(ledger.snapshot().write_sequence == 4);
    unknown_fields(ledger.snapshot());
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS | (uint32_t(1) << 9), 500)
          == L::UNKNOWN_MASK);
    CHECK(ledger.snapshot().write_sequence == 5);
    unknown_fields(ledger.snapshot());
}

static void foreign_expired_and_inconsistent_copies_are_unknown() {
    L::Ledger ledger;
    L::Ledger foreign;
    CHECK(ledger.begin_lifetime(29));
    CHECK(foreign.begin_lifetime(29));
    L::Snapshot read = foreign.snapshot();
    CHECK(foreign.commit(&read, L::ALL_FIELDS, 100) == L::COMMITTED);
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 100) == L::COMMITTED);
    // Equal lifetime/sequence values cannot merge two distinct Ledger owners.
    read = foreign.snapshot();
    CHECK(ledger.commit(&read, bit(L::MODE), 200) == L::UNKNOWN_READ);
    unknown_fields(ledger.snapshot());
    read = L::Snapshot();
    read.lifetime = 29;
    read.write_sequence = 2;
    CHECK(ledger.commit(&read, bit(L::MODE), 300) == L::UNKNOWN_READ);
    unknown_fields(ledger.snapshot());
    read = ledger.snapshot();
    read.lifetime = 28;
    CHECK(ledger.commit(&read, bit(L::MODE), 400) == L::UNKNOWN_READ);
    unknown_fields(ledger.snapshot());
    read = ledger.snapshot();
    ++read.write_sequence;
    CHECK(ledger.commit(&read, bit(L::MODE), 500) == L::UNKNOWN_READ);
    unknown_fields(ledger.snapshot());
    read = ledger.snapshot();
    read.fields[L::ALTITUDE].write_sequence = read.write_sequence + 1;
    CHECK(ledger.commit(&read, bit(L::MODE), 600) == L::UNKNOWN_READ);
    unknown_fields(ledger.snapshot());
    read = ledger.snapshot();
    read.fields[L::ALTITUDE].observed_ns = 10; // Unknown must be canonical.
    CHECK(ledger.commit(&read, bit(L::MODE), 700) == L::UNKNOWN_READ);
    unknown_fields(ledger.snapshot());
    L::Snapshot old_read = ledger.snapshot();
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, bit(L::MODE), 800) == L::COMMITTED);
    // A field cannot claim an assignment newer than its own read snapshot,
    // even when that assignment is not newer than the current global cache.
    old_read.fields[L::MODE] = ledger.snapshot().fields[L::MODE];
    CHECK(ledger.commit(&old_read, bit(L::ALTITUDE), 900) == L::UNKNOWN_READ);
    unknown_fields(ledger.snapshot());
}

static void reset_isolates_lifetimes_without_reusing_old_copies() {
    L::Ledger ledger;
    CHECK(ledger.begin_lifetime(31));
    L::Snapshot read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 100) == L::COMMITTED);
    const L::Snapshot old_response = ledger.snapshot();
    CHECK(ledger.begin_lifetime(32));
    CHECK(ledger.snapshot().write_sequence == 0);
    unknown_fields(ledger.snapshot());
    CHECK(ledger.commit(&old_response, L::ALL_FIELDS, 200) == L::UNKNOWN_READ);
    unknown_fields(ledger.snapshot());
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS, UINT64_MAX) == L::COMMITTED);
    CHECK(ledger.snapshot().lifetime == 32);
    CHECK(ledger.snapshot().fields[L::MODE].observed_ns == UINT64_MAX);
    CHECK(old_response.lifetime == 31);
    CHECK(old_response.fields[L::MODE].write_sequence == 1);
    CHECK(!ledger.begin_lifetime(32));
    CHECK(ledger.snapshot().lifetime == 0);
    unknown_fields(ledger.snapshot());
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 300) == L::INACTIVE);
    CHECK(!ledger.begin_lifetime(0));
    CHECK(!ledger.begin_lifetime(31));
    CHECK(ledger.begin_lifetime(33));
    CHECK(ledger.snapshot().lifetime == 33);
    unknown_fields(ledger.snapshot());
}

static void counters_do_not_wrap_or_reuse_identity() {
    L::Ledger ledger;
    CHECK(ledger.begin_lifetime(37));
    L::Snapshot read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 100) == L::COMMITTED);
    L::LdsLineageTestAccess::near_exhaustion(ledger);
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, bit(L::MODE), 200) == L::COMMITTED);
    const L::Snapshot last_response = ledger.snapshot();
    CHECK(last_response.write_sequence == UINT64_MAX);
    CHECK(last_response.fields[L::MODE].write_sequence == UINT64_MAX);
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 300) == L::EXHAUSTED);
    CHECK(ledger.snapshot().write_sequence == UINT64_MAX);
    unknown_fields(ledger.snapshot());
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 400) == L::EXHAUSTED);
    unknown_fields(ledger.snapshot());
    CHECK(last_response.fields[L::MODE].write_sequence == UINT64_MAX);
    CHECK(ledger.begin_lifetime(38));
    read = ledger.snapshot();
    CHECK(ledger.commit(&read, bit(L::MODE), 500) == L::COMMITTED);
    CHECK(ledger.snapshot().fields[L::MODE].write_sequence == 1);
    CHECK(ledger.snapshot().lifetime != last_response.lifetime);
    CHECK(ledger.begin_lifetime(UINT64_MAX));
    CHECK(!ledger.begin_lifetime(0));
    CHECK(!ledger.begin_lifetime(UINT64_MAX));
    CHECK(ledger.commit(&read, 0, 600) == L::INACTIVE);
    unknown_fields(ledger.snapshot());
}

static void calls_preserve_errno() {
    errno = EDOM;
    L::Ledger ledger;
    CHECK(errno == EDOM);
    CHECK(ledger.begin_lifetime(41));
    CHECK(errno == EDOM);
    L::Snapshot read = ledger.snapshot();
    CHECK(errno == EDOM);
    CHECK(ledger.commit(&read, L::ALL_FIELDS, 100) == L::COMMITTED);
    CHECK(errno == EDOM);
    CHECK(ledger.commit(0, L::ALL_FIELDS, 200) == L::UNKNOWN_READ);
    CHECK(errno == EDOM);
    CHECK(!ledger.begin_lifetime(41));
    CHECK(errno == EDOM);
}

int main() {
    // GCC 4.9's shipped libstdc++ lacks std::is_trivially_copyable.
    static_assert(__has_trivial_copy(L::Snapshot) &&
                  __has_trivial_assign(L::Snapshot) &&
                  __has_trivial_destructor(L::Snapshot),
                  "response lineage must be a simple owned value");
    static_assert(!std::is_copy_constructible<L::Ledger>::value,
                  "a ledger owner cannot be duplicated");
    observed_assignment_is_not_unknown();
    commit_inherits_the_read_copy_not_the_latest_cache();
    every_mask_inherits_only_unassigned_fields();
    reassignment_and_queries_have_different_identity_behavior();
    verified_copy_only_write_restores_the_actual_read_lineage();
    missing_read_and_unsupported_writer_stay_unknown();
    foreign_expired_and_inconsistent_copies_are_unknown();
    reset_isolates_lifetimes_without_reusing_old_copies();
    counters_do_not_wrap_or_reuse_identity();
    calls_preserve_errno();
    printf("LDS assignment lineage: %u checks passed\n", checks);
}
