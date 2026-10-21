#include "pxa_publisher_binding.h"
#include "pxa/wire.h"
#include <string.h>
pxa_status_t pxa_publisher_binding_verify(const uint8_t *data, size_t size, uint64_t now,
                                          void *context, pxa_publisher_authority_fn authority,
                                          pxa_package_signature_verify_fn verify,
                                          pxa_publisher_binding_t *out) {
    if (!out)
        return PXA_STATUS_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!data || size < 180 || size > PXA_PUBLISHER_BINDING_MAX || !authority || !verify ||
        memcmp(data, "PXPB\1\0\0\0", 8))
        return PXA_STATUS_INVALID_ARGUMENT;
    size_t app_bytes = data[112], handle_bytes = data[113], name_bytes = pxa_read_u16(data + 114);
    if (!app_bytes || app_bytes > 64 || !handle_bytes || handle_bytes > 24 || !name_bytes ||
        name_bytes > 128 || size != 116 + app_bytes + handle_bytes + name_bytes + 64 ||
        memchr(data + 116, 0, app_bytes + handle_bytes + name_bytes))
        return PXA_STATUS_INVALID_ARGUMENT;
    const uint8_t *app = data + 116;
    if (app[0] < 'a' || app[0] > 'z')
        return PXA_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < app_bytes; ++i)
        if (!((app[i] >= 'a' && app[i] <= 'z') || (app[i] >= '0' && app[i] <= '9') ||
              app[i] == '.' || app[i] == '_' || app[i] == '-'))
            return PXA_STATUS_INVALID_ARGUMENT;
    uint64_t revision = pxa_read_u64(data + 88), issued = pxa_read_u64(data + 96),
             expires = pxa_read_u64(data + 104);
    if (!revision || !issued || expires <= issued || expires - issued > UINT64_C(31) * 86400)
        return PXA_STATUS_INVALID_ARGUMENT;
    if (!now)
        return PXA_STATUS_UNAVAILABLE; /* no trusted wall clock yet */
    if (issued > now && issued - now > 300)
        return PXA_STATUS_DENIED;
    if (expires <= now)
        return PXA_STATUS_DENIED;
    uint8_t aggregate = 0;
    for (size_t i = 0; i < 16; ++i)
        aggregate |= data[40 + i];
    if (!aggregate)
        return PXA_STATUS_INVALID_ARGUMENT;
    uint8_t trusted_authority[32];
    pxa_status_t status = authority(context, data + 8, trusted_authority);
    if (status != PXA_STATUS_OK)
        return status;
    const char domain[] = PXA_PUBLISHER_BINDING_DOMAIN;
    status = verify(context, (pxa_bytes_t){data + 8, 32},
                    (pxa_bytes_t){(const uint8_t *)domain, sizeof(domain)},
                    (pxa_bytes_t){data, size - 64}, (pxa_bytes_t){data + size - 64, 64});
    if (status != PXA_STATUS_OK)
        return status;
    char handle[25], name[129];
    memcpy(handle, app + app_bytes, handle_bytes);
    handle[handle_bytes] = 0;
    memcpy(name, app + app_bytes + handle_bytes, name_bytes);
    name[name_bytes] = 0;
    status = pxa_publisher_profile_validate(handle, name, &out->profile);
    if (status != PXA_STATUS_OK)
        return status;
    memcpy(out->root, data + 56, 32);
    memcpy(out->authority, trusted_authority, 32);
    memcpy(out->namespace_id, data + 40, 16);
    memcpy(out->app_id, app, app_bytes);
    out->app_id[app_bytes] = 0;
    out->revision = revision;
    out->issued_at = issued;
    out->expires_at = expires;
    return PXA_STATUS_OK;
}
