#ifndef MX5_RUNTIME_TUNNEL_MAP_SWITCH_H
#define MX5_RUNTIME_TUNNEL_MAP_SWITCH_H
// Owner off switch of the map-constrained tunnel dead reckoning
// (validation/TUNNEL_MAP_CONSTRAINT_2026-10-22.md). Marker <root>/tunnelmap-off,
// written by trial menu 13: present (any type, or an unreadable state) turns
// the centreline constraint off while the marker exists, leaving the ordinary
// BETA tunnel replacement. Read once at worker start, like acc-cap-off.
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

namespace mx5 { namespace runtime {

static const char TUNNEL_MAP_OFF_NAME[]="tunnelmap-off";

// Fail closed towards the current tunnel replacement: anything except a
// definitely absent marker is present.
inline bool tunnel_map_off_marker(const char* root) {
    char path[512];
    const int n=root?snprintf(path,sizeof path,"%s/%s",root,TUNNEL_MAP_OFF_NAME):-1;
    if(n<=0||size_t(n)>=sizeof path)return true;
    struct stat st;
    errno=0;
    if(lstat(path,&st)==0)return true;
    const int failed=errno;
    return failed!=ENOENT;
}

} }
#endif
