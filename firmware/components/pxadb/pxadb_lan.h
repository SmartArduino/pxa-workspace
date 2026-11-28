#pragma once

#include <cstddef>
#include <cstdint>

namespace pxadb {

// Returns -2 exactly once when an authenticated connection is lost.
int ReadLan(uint8_t* destination, size_t size);
int WriteLan(const uint8_t* source, size_t size);

}  // namespace pxadb
