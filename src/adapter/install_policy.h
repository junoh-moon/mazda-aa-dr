#ifndef MX5_ADAPTER_INSTALL_POLICY_H
#define MX5_ADAPTER_INSTALL_POLICY_H
#include <string.h>

namespace mx5 { namespace adapter {

// The only third-party preload known to interpose the AA session API. It is
// the user's AA touch/HUD/km-L patch (oem-aa-mod). Every published release
// (0.1.0-0.10.0) exports aap_create_session and aap_destroy_session so the BLM's
// PLT slots resolve to it instead of libaap_interface; the product's session
// observation, which calls the original libaap_interface entry points directly,
// would bypass that shim and switch the user's features off. When (and only
// when) a session slot is owned by this exact library, session observation is
// skipped and the position/send hook is still installed. Anything else stays
// fail-closed.
inline bool known_session_shim(const char* path) {
    static const char* const prefixes[] = {
        "/data_persist/oem-aa-mod/", "/mnt/data_persist/oem-aa-mod/",
        "/tmp/mnt/data_persist/oem-aa-mod/"};
    static const char name[] = "libpatch-blmjciaapa.so";
    if (!path) return false;
    for (unsigned i = 0; i < sizeof prefixes / sizeof *prefixes; ++i) {
        const size_t n = strlen(prefixes[i]);
        if (!strncmp(path, prefixes[i], n) && !strcmp(path + n, name)) return true;
    }
    return false;
}

} }
#endif
