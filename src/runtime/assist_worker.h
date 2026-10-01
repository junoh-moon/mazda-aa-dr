#ifndef MX5_RUNTIME_ASSIST_WORKER_H
#define MX5_RUNTIME_ASSIST_WORKER_H
#include "navigation/pipeline.h"

namespace mx5 { namespace runtime {

enum AssistInputKind {
    ASSIST_BEGIN=0, ASSIST_POSITION, ASSIST_ANCHOR,
    ASSIST_SPEED, ASSIST_YAW, ASSIST_REVERSE
};
// Owned, normalized values from a separately verified source. This is not a
// wire ABI. context binds every item to its source/session; only captured
// POSITION and verified ANCHOR controls advance prediction generation.
struct AssistInput {
    AssistInputKind kind;
    mx5_dr_context context;
    adapter::Observation observation;
    mx5_dr_anchor anchor;
    // ANCHOR: exact adapter POSITION callback this verified anchor supports.
    // Neither receipt time nor mode generation is a unique GPS-fix identity.
    uint64_t position_call_sequence;
    mx5_dr_evidence evidence;
    // BEGIN: when this new qualified lifetime was established. ANCHOR: its
    // original receipt time. Recovery within the same source/session requires
    // a BEGIN and anchor measured after its qualification loss. A separately
    // verified new epoch retains its own original BEGIN and measurement times.
    uint64_t received_ns;
    double value;
    int reverse;
    uint16_t raw_yaw, yaw_count;
    uint64_t window_start_ns, window_end_ns;
};
enum AssistPoll { ASSIST_INPUT=0, ASSIST_EMPTY, ASSIST_FAULT };
struct AssistReadiness {
    CoreBridgeQualification qualification;
    // Producer's complete-through boundary, not receipt time or an EMPTY poll.
    uint64_t watermark_ns, requested_until_ns;
};
// Worker callbacks must be bounded, nonblocking, allocation-free and noexcept.
// pop copies one owned item; readiness must qualify exactly the supplied now.
// No implementation currently qualifies the live firmware sensor/provider
// path. A null/incomplete source explicitly represents that missing backend.
struct AssistSource {
    AssistPoll (*pop)(void* user, AssistInput*);
    bool (*readiness)(void* user, uint64_t now_ns, AssistReadiness*);
    void* user;
};
enum AssistState {
    ASSIST_WAITING_SOURCE=0, ASSIST_WAITING_BEGIN, ASSIST_WAITING_INPUT,
    ASSIST_PUBLISHED, ASSIST_SOURCE_FAULT, ASSIST_INPUT_FAULT,
    ASSIST_CLOCK_FAULT, ASSIST_CONTEXT_CHANGED, ASSIST_BACKLOG, ASSIST_STOPPED
};
struct AssistStatus {
    AssistState state;
    uint64_t ticks, inputs, begins, published, withdrawn, ignored, unpaired_positions;
    navigation::PipelineResult pipeline_result;
    CoreBridgeResult bridge_result;
    // Last ready publication for diagnostics; state/withdrawn describe later
    // withdrawal. This is not a live view of the adapter's current candidate.
    adapter::DrSnapshot last_publication;
};

// Single owner on the existing journal worker, no thread or I/O of its own.
// This controller never enables ASSIST or supplies live provenance. Callback
// send selection still rechecks request identity, generation and the lease.
class AssistWorker {
public:
    static const size_t INPUT_BUDGET=128;
    AssistWorker(const mx5_dr_config&, const AssistSource&);
    ~AssistWorker();
    void tick(adapter::MonotonicClock, void* clock_user);
    void stop();
    const AssistStatus& status() const { return status_; }
private:
    navigation::Pipeline pipeline_;
    mx5_dr_config config_;
    AssistSource source_;
    AssistStatus status_;
    mx5_dr_context binding_, recovery_binding_;
    uint64_t last_now_ns_, recovery_after_ns_;
    uint32_t published_generation_;
    bool active_, stopped_, begun_, publication_live_;
    static uint64_t revoke_candidate(void*);
    bool readiness(uint64_t, AssistReadiness*);
    bool consume(const AssistInput&, uint64_t, const AssistReadiness&);
    void revoke(AssistState, uint64_t, bool require_begin);
    void withdraw();
};

} }
#endif
