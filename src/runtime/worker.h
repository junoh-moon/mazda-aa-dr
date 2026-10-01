#ifndef MX5_RUNTIME_WORKER_H
#define MX5_RUNTIME_WORKER_H
namespace mx5 { namespace runtime {
class AssistWorker;
// Single-owner journal/calculation loop. A qualified source backend is not yet
// implemented: production startup passes null and keeps live ASSIST disabled.
void* run_worker(const char* root,const char* motion_channel,AssistWorker* assist);
} }
#endif
