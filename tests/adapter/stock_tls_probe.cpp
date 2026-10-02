// Authored QEMU-user boundary probe. The TLS matrix leaves
// MX5DR_OEM_AA_PATH unset; the optional dlopen path is a separate control.
// This neither starts an OEM service nor invokes product hooks.
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void* load_aa(void*) {
    (void)write(1, "thread_enter\n", sizeof("thread_enter\n") - 1);
    const char* path = getenv("MX5DR_OEM_AA_PATH");
    if (!path) return 0;
    (void)write(1, "before_dlopen\n", sizeof("before_dlopen\n") - 1);
    void* handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        const char* error = dlerror();
        fprintf(stderr, "dlopen_error=%s\n", error ? error : "unknown");
        return reinterpret_cast<void*>(3);
    }
    (void)write(1, "after_dlopen\n", sizeof("after_dlopen\n") - 1);
    if (!getenv("MX5DR_SKIP_DLCLOSE")) dlclose(handle);
    return 0;
}

int main() {
    (void)write(1, "main_ready\n", sizeof("main_ready\n") - 1);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return 4;
    const size_t stack_size = getenv("MX5DR_PROBE_STACK_24") ? 24 * 1024 : 16 * 1024;
    if (pthread_attr_setstacksize(&attr, stack_size)) return 5;
    pthread_t thread;
    (void)write(1, "before_pthread_create\n", sizeof("before_pthread_create\n") - 1);
    if (pthread_create(&thread, &attr, load_aa, 0)) return 6;
    (void)write(1, "after_pthread_create\n", sizeof("after_pthread_create\n") - 1);
    pthread_attr_destroy(&attr);
    void* result = 0;
    if (pthread_join(thread, &result)) return 7;
    printf("thread_result=%ld\n", static_cast<long>(reinterpret_cast<long>(result)));
    return result ? 8 : 0;
}
