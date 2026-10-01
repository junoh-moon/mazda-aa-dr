#ifndef MX5_ADAPTER_LDS_INSTALL_H
#define MX5_ADAPTER_LDS_INSTALL_H

#include "adapter.h"
#include "lds_hooks.h"

namespace mx5 { namespace adapter {

// Exact NA 74.00.324A LDS service, before GetServiceInterfaces/ServiceInit is
// exposed. The target's live dependency scope supplies every original address.
// Only verified non-executable data slots may change. No callback registration,
// OEM initialization or hardware operation is performed by the installer.
struct LdsInstallOptions {
    void* service_handle;
    VerifyFileHash verify_file_hash;
    bool verified_cold_start;
    bool (*begin_patch)();
    // Data-only publication/rollback never removes executable permission.
    // Forwarding targets remain mapped and immutable after any publication.
    void (*end_patch)(bool executable_restored);
    uint64_t (*clock)(void*);
    void (*emit)(const runtime::lds_sideband::Record&,void*);
    void* user;
};

InstallResult install_lds_v74(const LdsInstallOptions&);

} }
#endif
