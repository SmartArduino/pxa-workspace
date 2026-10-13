#ifndef PXADB_FS_POLICY_H
#define PXADB_FS_POLICY_H

#include <string_view>

namespace pxadb {
constexpr bool protected_system_directory(std::string_view path) {
    return path == "system/fonts" || path == "system/pxa";
}

// Creating a missing protected parent is needed for initial provisioning.
// Replacement, removal and uploads still use the stricter write policy.
constexpr bool may_create_directory(std::string_view path, bool writable,
                                    bool system_unlocked) {
    return writable || (system_unlocked && protected_system_directory(path));
}
}

#endif
