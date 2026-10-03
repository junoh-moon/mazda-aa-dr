// The stock CMU limits the default thread stack to 128 KiB (init_cmu sets RLIMIT_STACK
// soft = 131072 and glibc 2.11 uses it as the pthread default). The product worker keeps
// a frame of several hundred KiB, so it must start on a thread with an explicit stack.
// This test re-executes itself under that limit and checks three things: the premise
// (a default-attribute thread running such a frame dies), the product helper (it runs the
// same frame), and graceful failure (an invalid stack size returns false, no crash).
#include "runtime/worker_thread.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static const rlim_t STOCK_LIMIT=128*1024;
static volatile int done;
static void* big_frame(void*) {
    volatile unsigned char buf[700*1024]; // about the size of the worker frame
    for(size_t i=0;i<sizeof buf;i+=1024)buf[i]=(unsigned char)(i/1024+1);
    unsigned sum=0;
    for(size_t i=0;i<sizeof buf;i+=1024)sum+=buf[i];
    done=sum?1:2;
    return 0;
}
int main(int, char** argv) {
    struct rlimit rl;
    assert(!getrlimit(RLIMIT_STACK,&rl));
    if(rl.rlim_cur!=STOCK_LIMIT) {
        rl.rlim_cur=STOCK_LIMIT;
        assert(!setrlimit(RLIMIT_STACK,&rl));
        execv("/proc/self/exe",argv); // glibc reads the limit at process start
        perror("execv");
        return 2;
    }
    const pid_t pid=fork();
    assert(pid>=0);
    if(!pid) { // premise: the default thread cannot hold the frame
        struct rlimit no_core={0,0};
        setrlimit(RLIMIT_CORE,&no_core);
        pthread_t t;
        if(pthread_create(&t,0,big_frame,0))_exit(3);
        pthread_join(t,0);
        _exit(0);
    }
    int status=0;
    assert(waitpid(pid,&status,0)==pid);
    assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGSEGV);
    pthread_t thread;
    done=0;
    assert(mx5::runtime::create_thread(&thread,big_frame,0,mx5::runtime::WORKER_STACK_BYTES,false));
    assert(!pthread_join(thread,0));
    assert(done==1);
    done=0; // the production worker is detached
    assert(mx5::runtime::create_thread(&thread,big_frame,0,mx5::runtime::WORKER_STACK_BYTES,true));
    for(unsigned i=0;i<500&&!done;++i) { struct timespec ts={0,10000000};nanosleep(&ts,0); }
    assert(done==1);
    assert(!mx5::runtime::create_thread(&thread,big_frame,0,1,false)); // below PTHREAD_STACK_MIN
    puts("worker thread: explicit stack runs the worker-sized frame under the stock 128 KiB limit");
    return 0;
}
