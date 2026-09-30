#ifndef MX5_RUNTIME_JOURNAL_QUEUE_H
#define MX5_RUNTIME_JOURNAL_QUEUE_H

#include <atomic>
#include <stdint.h>
#include <type_traits>

namespace mx5 { namespace runtime {

// Bounded MPSC queue, exactly one consumer. Initialize before producers;
// destroy only after all users have stopped. Value copies must be bounded,
// noexcept and must not call back into this queue. Production uses an owned,
// trivially copyable Observation, never borrowed OEM storage.
//
// Producers reserve capacity before taking a ticket. A delayed reservation
// cannot be overwritten or mistaken for a completed close. FIFO is ticket
// order; overlapping producers have no earlier total ordering requirement.
// No source retry loop, allocation, mutex, I/O or sleep is used here. Atomic
// primitives are lock-free, not a wait-free or wall-clock guarantee.
template<class Value, unsigned Capacity>
class JournalQueue {
public:
    enum PushResult { QUEUED, FULL, STOPPED };
    // Observation is a literal value type. Constant initialization is required
    // for the preload's global queue: dlopen can be interposed before DSO
    // dynamic constructors have run, so no later constructor may reset it.
    constexpr JournalQueue() : state_(0), ticket_(0), head_(0), losses_(0), lost_(false), slots_{} {}
    PushResult push(const Value& value) {
        if (closed()) return STOPPED;
        const uint64_t before = state_.fetch_add(1, std::memory_order_acq_rel);
        if (before & CLOSED) {
            state_.fetch_sub(1, std::memory_order_release);
            return STOPPED;
        }
        if (before >= Capacity) {
            lost_.store(true, std::memory_order_relaxed);
            losses_.fetch_add(1, std::memory_order_relaxed);
            // Loss publication precedes returning this reservation. A final
            // drained() acquire therefore includes all pre-close failures.
            state_.fetch_sub(1, std::memory_order_release);
            return FULL;
        }
        // acq_rel also relays newer consumer releases through other ticket
        // owners when our capacity reservation predates a complete ring lap.
        const uint32_t ticket = ticket_.fetch_add(1, std::memory_order_acq_rel);
        Slot& slot = slots_[ticket % Capacity];
        slot.value = value;
        slot.ready.store(true, std::memory_order_release);
        return QUEUED;
    }
    bool pop(Value* out) {
        Slot& slot = slots_[head_ % Capacity];
        if (!slot.ready.load(std::memory_order_acquire)) return false;
        *out = slot.value;
        slot.ready.store(false, std::memory_order_release);
        ++head_;
        // The entire consumer copy ends before a producer can reuse its slot.
        state_.fetch_sub(1, std::memory_order_release);
        return true;
    }
    void close() { state_.fetch_or(CLOSED, std::memory_order_acq_rel); }
    // The pinned GCC 4.9.1 ARM backend places no trailing DMB after an
    // acquire-only 64-bit load. Use seq_cst for these loads; its generated
    // post-load barrier is required before reading the final sticky loss.
    bool closed() const { return (state_.load(std::memory_order_seq_cst) & CLOSED) != 0; }
    bool drained() const { return state_.load(std::memory_order_seq_cst) == CLOSED; }
    uint64_t dropped() const { return losses_.load(std::memory_order_seq_cst); }
    bool lost() const { return lost_.load(std::memory_order_acquire); }
private:
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must divide the uint32 ticket wrap");
    static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2 &&
                  ATOMIC_BOOL_LOCK_FREE == 2, "Queue requires lock-free control atomics");
    static_assert(std::is_nothrow_copy_assignable<Value>::value,
                  "A reserved value copy cannot throw");
    static const uint64_t CLOSED = uint64_t(1) << 63;
    // Low 63 bits count unconsumed successes and not-yet-returned failed
    // reservations. Overflow would require 2^63 simultaneous calls, beyond
    // the pinned 32-bit process address space. Failed FULL/STOPPED attempts
    // can temporarily occupy credits; this cannot create an initial loss
    // without a real capacity exhaustion or invent a successful enqueue.
    alignas(8) std::atomic<uint64_t> state_;
    std::atomic<uint32_t> ticket_;
    uint32_t head_; // Consumer-only, including natural unsigned wrap.
    std::atomic<uint64_t> losses_;
    std::atomic<bool> lost_;
    struct Slot {
        Value value;
        std::atomic<bool> ready;
        constexpr Slot() : value(), ready(false) {}
    } slots_[Capacity];
    JournalQueue(const JournalQueue&) = delete;
    JournalQueue& operator=(const JournalQueue&) = delete;
    friend struct JournalQueueTestAccess;
};

} }
#endif
