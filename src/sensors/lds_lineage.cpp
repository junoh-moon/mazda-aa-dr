#include "sensors/lds_lineage.h"

namespace mx5 { namespace sensors { namespace lds_lineage {

Snapshot::Snapshot() : lifetime(0), write_sequence(0), fields(), owner_(0) {}

Ledger::Ledger() : current_(), lifetime_high_water_(0), exhausted_(false) {
    current_.owner_ = this;
}

bool Ledger::begin_lifetime(uint64_t lifetime) {
    current_ = Snapshot();
    current_.owner_ = this;
    exhausted_ = false;
    if (!lifetime || lifetime <= lifetime_high_water_) return false;
    lifetime_high_water_ = lifetime;
    current_.lifetime = lifetime;
    return true;
}

Snapshot Ledger::snapshot() const { return current_; }

void Ledger::clear_fields() {
    for (unsigned i = 0; i < FIELD_COUNT; ++i) {
        current_.fields[i].write_sequence = 0;
        current_.fields[i].observed_ns = 0;
    }
}

bool Ledger::valid_read(const Snapshot& read) const {
    if (read.owner_ != this || read.lifetime != current_.lifetime ||
        read.write_sequence > current_.write_sequence) return false;
    for (unsigned i = 0; i < FIELD_COUNT; ++i) {
        const FieldOrigin& origin = read.fields[i];
        if (origin.write_sequence > read.write_sequence ||
            (!origin.write_sequence && origin.observed_ns)) return false;
    }
    return true;
}

CommitResult Ledger::commit(const Snapshot* read, uint32_t mask,
                            uint64_t observed_ns) {
    if (!current_.lifetime) return INACTIVE;
    if (exhausted_ || current_.write_sequence == UINT64_MAX) {
        clear_fields();
        exhausted_ = true;
        return EXHAUSTED;
    }

    // Validate against the state before consuming this write's identity. In
    // particular, a read claiming the forthcoming sequence is still invalid.
    const bool known_read = read && valid_read(*read);
    ++current_.write_sequence;
    if (mask & ~ALL_FIELDS) {
        clear_fields();
        return UNKNOWN_MASK;
    }
    if (!known_read) {
        clear_fields();
        return UNKNOWN_READ;
    }
    for (unsigned i = 0; i < FIELD_COUNT; ++i) {
        if (mask & (uint32_t(1) << i)) {
            current_.fields[i].write_sequence = current_.write_sequence;
            current_.fields[i].observed_ns = observed_ns;
        } else {
            current_.fields[i] = read->fields[i];
        }
    }
    return COMMITTED;
}

} } }
