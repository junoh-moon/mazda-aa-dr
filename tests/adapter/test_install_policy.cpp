// Which module may own the BLM's session slots without making the installer
// fail closed. Anything not listed here must stay fail-closed.
#include "adapter/install_policy.h"
#include <cassert>
using mx5::adapter::known_session_shim;
int main() {
    assert(known_session_shim("/data_persist/oem-aa-mod/libpatch-blmjciaapa.so"));
    assert(known_session_shim("/mnt/data_persist/oem-aa-mod/libpatch-blmjciaapa.so"));
    assert(known_session_shim("/tmp/mnt/data_persist/oem-aa-mod/libpatch-blmjciaapa.so"));
    assert(!known_session_shim(0));
    assert(!known_session_shim(""));
    // The BLM's own PLT (lazy binding) and the genuine interface are not shims.
    assert(!known_session_shim("/jci/aapa/blmjciaapa.so"));
    assert(!known_session_shim("/usr/lib/libaap_interface.so"));
    // Our own library, other libpatch files and look-alikes are not accepted.
    assert(!known_session_shim("/data_persist/mx5-aa-dr/libmx5dr.so"));
    assert(!known_session_shim("/data_persist/oem-aa-mod/libpatch-aap_service.so"));
    assert(!known_session_shim("/data_persist/oem-aa-mod/libpatch-blmjciaapa.so.bak"));
    assert(!known_session_shim("/data_persist/oem-aa-mod/sub/libpatch-blmjciaapa.so"));
    assert(!known_session_shim("/tmp/oem-aa-mod/libpatch-blmjciaapa.so"));
    assert(!known_session_shim("/data_persist/oem-aa-mod_x/libpatch-blmjciaapa.so"));
    assert(!known_session_shim("libpatch-blmjciaapa.so"));
    return 0;
}
