#ifndef MX5_RUNTIME_WORKER_H
#define MX5_RUNTIME_WORKER_H
#include <sys/types.h>
namespace mx5 { namespace runtime {
class AssistWorker;
// Single-owner journal/calculation loop. A qualified source backend is not yet
// implemented: production startup passes null and keeps live ASSIST disabled.
void* run_worker(const char* root,const char* motion_channel,AssistWorker* assist);
// Dependency seam for isolated channel tests; normal startup always uses the
// exact-firmware LDS channel and UID through the three-argument entry above.
void* run_worker_channels(const char* root,const char* motion_channel,const char* lds_channel,
                          uid_t lds_uid,AssistWorker* assist);
} }
#endif
