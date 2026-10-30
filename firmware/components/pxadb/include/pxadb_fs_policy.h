#ifndef PXADB_FS_POLICY_H
#define PXADB_FS_POLICY_H

#include <string_view>

namespace pxadb {
inline constexpr size_t kFsPathCapacity = 240;

// Validate the decoded bytes, including any embedded NUL, before using a path
// as a C string. The capacity includes the terminating NUL added by the caller.
inline bool valid_fs_path(std::string_view path) {
    if (path.empty() || path.size() >= kFsPathCapacity) return false;
    if (path == ".") return true;
    size_t segment = 0;
    for (size_t index = 0; index <= path.size(); ++index) {
        if (index == path.size() || path[index] == '/') {
            const auto part = path.substr(segment, index - segment);
            if (part.empty() || part == "." || part == "..") return false;
            segment = index + 1;
            continue;
        }
        const auto value = static_cast<unsigned char>(path[index]);
        if (value < 0x80) {
            if (value < 0x20 || value == 0x7f || value == '\\' || value == ':')
                return false;
            continue;
        }
        // Decode a whole UTF-8 scalar so invalid/overlong encodings cannot
        // disguise separators or NULs. Do not depend on the C locale or char
        // signedness: Chinese names and other Unicode names use these bytes.
        unsigned codepoint, minimum;
        size_t continuation;
        if (value >= 0xc2 && value <= 0xdf) {
            codepoint = value & 0x1f; minimum = 0x80; continuation = 1;
        } else if (value >= 0xe0 && value <= 0xef) {
            codepoint = value & 0x0f; minimum = 0x800; continuation = 2;
        } else if (value >= 0xf0 && value <= 0xf4) {
            codepoint = value & 0x07; minimum = 0x10000; continuation = 3;
        } else {
            return false;
        }
        if (continuation > path.size() - index - 1) return false;
        while (continuation-- != 0) {
            const auto next = static_cast<unsigned char>(path[++index]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    }
    return true;
}

constexpr bool protected_system_directory(std::string_view path) {
    return path == "system/fonts" || path == "system/pxa";
}

// Creating a missing protected parent is needed for initial provisioning.
// Replacement, removal and uploads still use the stricter write policy.
constexpr bool may_create_directory(std::string_view path, bool writable,
                                    bool system_unlocked) {
    return writable || (system_unlocked &&
                        (path == "system" || protected_system_directory(path)));
}
}

#endif
