#pragma once

#if defined(ESP_PLATFORM)

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t* spki_der;
    size_t spki_der_size;
} PxaTrustedPublisherKey;

// Product firmware may override the weak default. The table and its DER bytes
// must remain valid for the process lifetime; static const storage is expected.
size_t pxa_platform_trusted_publishers(
    const PxaTrustedPublisherKey** output_keys);

#ifdef __cplusplus
}
#endif

#endif
