#ifndef MX5_RUNTIME_DIAG_ARMS_H
#define MX5_RUNTIME_DIAG_ARMS_H
// Diagnostic BETA build (v1.0.0-beta.12 candidate,
// validation/DIAG_BUILD_A20_S5_2026-10-12.md). Owner marker, arm assignment
// and the constants of the two diagnostic arms. Worker thread only.
//
// Marker: <root>/diag, written by trial menu 9 as the 3 bytes "on\n". Only a
// regular file (not a symlink) with exactly that content enables the
// diagnostic; absent, any other type or any other content is OFF, and then
// the product is byte-identical to v1.0.0-beta.11. Read once at worker start.
//
// Arms: every GPS-lost episode gets one of CONTROL (beta.11 behaviour), A20
// (reported accuracy capped at 20 m) and S5 (GPS_SATELLITE number_in_use 5
// after a BETA LOCATION replacement of the same call). Balanced block
// randomization: episodes 3b, 3b+1, 3b+2 of one boot are a permutation of the
// three arms, chosen from the six permutations by a hash of (boot id, b). The
// arm is fixed for the whole episode. The owner is not told the arm.
#include "adapter/adapter.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mx5 { namespace runtime {

static const char DIAG_MARKER_NAME[]="diag";
// A20: the reporting ceiling of an A20 episode (metres).
static const double DIAG_A20_CAP_M=20.0;
// Episode identity: a GPS loss that starts again less than this after the
// previous episode of the same boot ended keeps that episode (index and arm).
// 10 s is the BETA profile's settle time (BetaProfile::anchor_settle_ns of
// beta_profile(): consecutive increasing fixes before a legacy anchor); a
// reacquisition shorter than that is not treated as the end of the tunnel.
static const uint64_t DIAG_REJOIN_NS=10000000000ULL;
static const unsigned DIAG_BLOCK=3;

enum DiagMarker { DIAG_MARKER_ABSENT=0, DIAG_MARKER_ON, DIAG_MARKER_INVALID };
inline const char* diag_marker_name(DiagMarker m) {
    return m==DIAG_MARKER_ON?"on":m==DIAG_MARKER_INVALID?"invalid":"absent";
}
// Fail closed: only a regular file holding exactly "on\n" is ON.
inline DiagMarker diag_marker(const char* root) {
    char path[512];
    const int n=root?snprintf(path,sizeof path,"%s/%s",root,DIAG_MARKER_NAME):-1;
    if(n<=0 || size_t(n)>=sizeof path)return DIAG_MARKER_INVALID;
    struct stat st;
    if(lstat(path,&st)) return errno==ENOENT?DIAG_MARKER_ABSENT:DIAG_MARKER_INVALID;
    if(!S_ISREG(st.st_mode) || st.st_size!=3)return DIAG_MARKER_INVALID;
    const int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0)return DIAG_MARKER_INVALID;
    char buf[4]={0};
    const ssize_t got=read(fd,buf,sizeof buf);
    close(fd);
    return got==3 && !memcmp(buf,"on\n",3)?DIAG_MARKER_ON:DIAG_MARKER_INVALID;
}

// FNV-1a over the boot id text (36 characters, or "unknown").
inline uint32_t diag_seed(const char* boot_id) {
    uint32_t h=2166136261u;
    for(const char* p=boot_id?boot_id:"";*p;++p) { h^=uint8_t(*p);h*=16777619u; }
    return h;
}
// Avalanche mix (lowbias32) of the seed and the block index.
inline uint32_t diag_mix(uint32_t x) {
    x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;
    return x;
}
// Arm of the episode with 0-based index `episode` in this boot. block/perm
// (when given) report the block number and the chosen permutation (0..5).
inline adapter::DiagArm diag_arm_for(uint32_t seed,uint32_t episode,unsigned* block=0,unsigned* perm=0) {
    static const uint8_t perms[6][3]={
        {adapter::DIAG_ARM_CONTROL,adapter::DIAG_ARM_A20,adapter::DIAG_ARM_S5},
        {adapter::DIAG_ARM_CONTROL,adapter::DIAG_ARM_S5,adapter::DIAG_ARM_A20},
        {adapter::DIAG_ARM_A20,adapter::DIAG_ARM_CONTROL,adapter::DIAG_ARM_S5},
        {adapter::DIAG_ARM_A20,adapter::DIAG_ARM_S5,adapter::DIAG_ARM_CONTROL},
        {adapter::DIAG_ARM_S5,adapter::DIAG_ARM_CONTROL,adapter::DIAG_ARM_A20},
        {adapter::DIAG_ARM_S5,adapter::DIAG_ARM_A20,adapter::DIAG_ARM_CONTROL}};
    const uint32_t b=episode/DIAG_BLOCK;
    const unsigned p=diag_mix(seed^(b*0x9e3779b9u))%6u;
    if(block)*block=b;
    if(perm)*perm=p;
    return adapter::DiagArm(perms[p][episode%DIAG_BLOCK]);
}

} }
#endif
