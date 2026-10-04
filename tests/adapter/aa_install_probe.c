/* Offline probe for tests/adapter/run_aa_install_probe.sh: loads the real AA BLM the way
 * sm_svclauncher does (dlopen RTLD_NOW) so the preloaded product's dlopen interposer and installer
 * run, then lingers so the worker thread can write the boot row. Linked against the same stock
 * libraries as the launcher so the BLM's undefined symbols resolve as they do on the CMU. */
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    void *h = dlopen("/jci/aapa/blmjciaapa.so", RTLD_NOW);
    printf("probe dlopen=%s\n", h ? "ok" : dlerror());
    fflush(stdout);
    sleep(5);
    return h ? 0 : 3;
}
