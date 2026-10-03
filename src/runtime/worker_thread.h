#ifndef MX5_RUNTIME_WORKER_THREAD_H
#define MX5_RUNTIME_WORKER_THREAD_H
#include <pthread.h>
#include <stddef.h>
namespace mx5 { namespace runtime {
// The stock CMU init sets RLIMIT_STACK (soft) to 128 KiB and glibc 2.11 uses that
// value as the default pthread stack. The journal/calculation worker keeps its
// objects (pipeline, holdout, receivers, line buffers) in one frame of several
// hundred KiB, so it must never run on a default-attribute thread: the first
// call overflows the stack before the boot record is written. The stack is
// reserved as virtual memory and committed only when touched.
const size_t WORKER_STACK_BYTES=4u<<20;
inline bool create_thread(pthread_t* thread,void* (*fn)(void*),void* arg,
                          size_t stack_bytes,bool detached) {
    pthread_attr_t attr;
    if(pthread_attr_init(&attr))return false;
    const bool ok=pthread_attr_setstacksize(&attr,stack_bytes)==0&&
        pthread_attr_setdetachstate(&attr,detached?PTHREAD_CREATE_DETACHED:
                                                    PTHREAD_CREATE_JOINABLE)==0&&
        pthread_create(thread,&attr,fn,arg)==0;
    pthread_attr_destroy(&attr);
    return ok;
}
} }
#endif
