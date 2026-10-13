#include "pxadb_fs_policy.h"
#include <cassert>

int main() {
    for (const auto path : {"system/fonts", "system/pxa"}) {
        assert(!pxadb::may_create_directory(path, false, false));
        assert(pxadb::may_create_directory(path, false, true));
        assert(pxadb::protected_system_directory(path));
    }
    for (const auto path : {"system", "systematic/pxa", "pxa-state/packages", "../system/pxa"})
        assert(!pxadb::may_create_directory(path, false, true));
    assert(pxadb::may_create_directory("pxa-state/inbox/test", true, false));
}
