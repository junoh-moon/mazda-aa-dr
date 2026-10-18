#ifndef MX5_RUNTIME_ACC_CAP_H
#define MX5_RUNTIME_ACC_CAP_H
// Owner off switch of the tunnel reporting ceiling
// (validation/TUNNEL_REPORT_CAP_2026-10-12.md). Marker <root>/acc-cap-off,
// written by trial menu 10: present (any type, or an unreadable state) turns
// the ceiling off for the boot (beta.12 numbers: 40 m ceiling, stopped rule).
// Read once at worker start, like g3-off.
//
// The diagnostic arms of trial menu 9 (marker <root>/diag, CONTROL/CAP
// episodes) were removed for v1.0.0 (validation/V1_A_PRODUCT_HARDENING_2026-10-18.md):
// the runtime no longer reads a 'diag' file, so a stale one is ignored.
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

namespace mx5 { namespace runtime {

static const char ACC_CAP_OFF_NAME[]="acc-cap-off";

// Fail closed towards the beta.12 behaviour: anything except a definitely
// absent marker is present.
inline bool acc_cap_off_marker(const char* root) {
    char path[512];
    const int n=root?snprintf(path,sizeof path,"%s/%s",root,ACC_CAP_OFF_NAME):-1;
    if(n<=0 || size_t(n)>=sizeof path)return true;
    struct stat st;
    if(!lstat(path,&st))return true;
    return errno!=ENOENT;
}

} }
#endif
