// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pxa/asset_object.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace espp4_a {
// The Core image API guarantees 64-byte payload alignment. Two immutable
// allocation offsets cover both possible cache-line classes on this P4.
// Always check the actual returned payload before passing it to DMA.
constexpr size_t kCameraImageDmaAlignment = 128;
static_assert(PXA_ASSET_IMAGE_ALIGNMENT == 64,
              "Review P4 preview allocation when the public image alignment changes");
struct CameraImageAllocator {
    size_t offset;
    void* (*allocate)(size_t);
    void (*release)(void*);
};
inline void* CameraImageAllocate(void* context, size_t bytes) {
    const auto* allocator = static_cast<const CameraImageAllocator*>(context);
    constexpr size_t extra = sizeof(void*) + kCameraImageDmaAlignment - 1 + PXA_ASSET_IMAGE_ALIGNMENT;
    if (!allocator || !allocator->allocate || !allocator->release || !bytes ||
        (allocator->offset != 0 && allocator->offset != PXA_ASSET_IMAGE_ALIGNMENT) ||
        bytes > std::numeric_limits<size_t>::max() - extra) return nullptr;
    void* raw = allocator->allocate(bytes + extra);
    if (!raw) return nullptr;
    const uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + sizeof(raw) +
        kCameraImageDmaAlignment - 1) & ~(uintptr_t(kCameraImageDmaAlignment) - 1);
    auto* object = reinterpret_cast<uint8_t*>(aligned + allocator->offset);
    // This prefix belongs to this allocation, outside the opaque Core object.
    std::memcpy(object - sizeof(raw), &raw, sizeof(raw));
    return object;
}
inline void CameraImageRelease(void* context, void* object) {
    if (!object) return;
    const auto* allocator = static_cast<const CameraImageAllocator*>(context);
    void* raw = nullptr;
    std::memcpy(&raw, static_cast<uint8_t*>(object) - sizeof(raw), sizeof(raw));
    allocator->release(raw);
}
inline bool CameraImageDmaSize(size_t bytes, size_t cache_alignment) {
    return bytes && cache_alignment && !(cache_alignment & (cache_alignment - 1)) &&
        cache_alignment <= kCameraImageDmaAlignment && !(bytes % cache_alignment);
}
inline bool CameraImageDmaReady(const void* payload, size_t bytes, size_t cache_alignment) {
    return payload && CameraImageDmaSize(bytes, cache_alignment) &&
        !(reinterpret_cast<uintptr_t>(payload) & (cache_alignment - 1));
}
} // namespace espp4_a
