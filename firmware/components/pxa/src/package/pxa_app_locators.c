#include "pxa_app_locators.h"
#include <stdio.h>
#include <string.h>
static int app_id_valid(const char *s) {
    if (!s || s[0] < 'a' || s[0] > 'z')
        return 0;
    size_t n = 0;
    for (; n < 65 && s[n]; ++n)
        if (!((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '.' ||
              s[n] == '_' || s[n] == '-'))
            return 0;
    return n > 0 && n < 65;
}
static int directory_valid(const char *s) { return app_id_valid(s) && s[strlen(s) - 1] != '.'; }
static int publisher_handle_valid(const char *s) {
    if (!directory_valid(s) || strlen(s) > 24)
        return 0;
    for (size_t i = 0; s[i]; ++i)
        if (s[i] == '.' || s[i] == '_' || (s[i] == '-' && (!i || s[i - 1] == '-' || !s[i + 1])))
            return 0;
    return 1;
}
static int reserved(const char *s) {
    const char *const words[] = {"con",  "prn",   "aux", "nul",   "system",  "app",
                                 "data", "media", "pxa", "admin", "official"};
    size_t n = strcspn(s, ".");
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if (strlen(words[i]) == n && !memcmp(s, words[i], n))
            return 1;
    return n == 4 && (!memcmp(s, "com", 3) || !memcmp(s, "lpt", 3)) && s[3] >= '1' && s[3] <= '9';
}
static void fingerprint(const uint8_t key[32], char out[53]) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    for (unsigned i = 0; i < 52; ++i) {
        unsigned bit = i * 5, byte = bit / 8, shift = 11 - bit % 8;
        unsigned value = (unsigned)key[byte] << 8;
        if (byte + 1 < 32)
            value |= key[byte + 1];
        out[i] = alphabet[(value >> shift) & 31];
    }
    out[52] = 0;
}
void pxa_app_locators_init(pxa_app_locators_t *r, void *context, pxa_locators_commit_fn commit) {
    memset(r, 0, sizeof(*r));
    r->context = context;
    r->commit = commit;
    r->ready = 1;
}
static int publisher_taken(const pxa_app_locators_t *r, const char *name) {
    for (size_t i = 0; i < r->publisher_count; ++i)
        if (!strcmp(r->publishers[i].directory, name))
            return 1;
    return 0;
}
static int app_taken(const pxa_app_locators_t *r, size_t publisher, const char *name) {
    for (size_t i = 0; i < r->app_count; ++i)
        if (r->apps[i].publisher == publisher && !strcmp(r->apps[i].directory, name))
            return 1;
    return 0;
}
static int available(const pxa_app_locators_t *r, size_t publisher, int pub, const char *name) {
    if (pub ? publisher_taken(r, name) : app_taken(r, publisher, name))
        return 0;
    return !r->available ||
           r->available(r->context, pub ? name : r->publishers[publisher].directory,
                        pub ? NULL : name) == 1;
}
static int choose(const pxa_app_locators_t *r, size_t publisher, int pub, const char *base,
                  const uint8_t key[32], char out[65]) {
    snprintf(out, 65, "%s", base);
    if (available(r, publisher, pub, out))
        return 1;
    char suffix[53];
    fingerprint(key, suffix);
    const unsigned lengths[] = {8, 12, 16, 24, 32, 40, 52};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        unsigned length = lengths[i];
        snprintf(out, 65, "%.*s--%.*s", (int)(64 - 2 - length), base, (int)length, suffix);
        if (available(r, publisher, pub, out))
            return 1;
    }
    return 0;
}
pxa_status_t pxa_app_locators_load(pxa_app_locators_t *r, uint64_t generation,
                                   const pxa_locator_publisher_t *pubs, size_t np,
                                   const pxa_locator_app_t *apps, size_t na) {
    if (!r)
        return PXA_STATUS_INVALID_ARGUMENT;
    r->ready = 0;
    if (np > PXA_LOCATOR_MAX_PUBLISHERS || na > PXA_LOCATOR_MAX_APPS || (np && !pubs) ||
        (na && !apps))
        return PXA_STATUS_PROTOCOL_ERROR;
    for (size_t i = 0; i < np; ++i) {
        if (!memchr(pubs[i].directory, 0, 65) || !directory_valid(pubs[i].directory) ||
            reserved(pubs[i].directory) || (pubs[i].kind != 1 && pubs[i].kind != 2))
            return PXA_STATUS_PROTOCOL_ERROR;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(pubs[i].directory, pubs[j].directory) ||
                (pubs[i].kind == pubs[j].kind && !memcmp(pubs[i].key, pubs[j].key, 32)))
                return PXA_STATUS_PROTOCOL_ERROR;
    }
    for (size_t i = 0; i < na; ++i) {
        if (apps[i].publisher >= np ||
            ((apps[i].flags & ~63u) || ((apps[i].flags >> 3) & ~apps[i].flags & 7u)) ||
            !memchr(apps[i].app_id, 0, 65) || !app_id_valid(apps[i].app_id) ||
            !memchr(apps[i].directory, 0, 65) || !directory_valid(apps[i].directory) ||
            reserved(apps[i].directory))
            return PXA_STATUS_PROTOCOL_ERROR;
        for (size_t j = 0; j < i; ++j)
            if ((!memcmp(apps[i].root, apps[j].root, 32) &&
                 !strcmp(apps[i].app_id, apps[j].app_id)) ||
                (apps[i].publisher == apps[j].publisher &&
                 !strcmp(apps[i].directory, apps[j].directory)))
                return PXA_STATUS_PROTOCOL_ERROR;
    }
    if (np)
        memmove(r->publishers, pubs, np * sizeof(*pubs));
    if (na)
        memmove(r->apps, apps, na * sizeof(*apps));
    /* Discard retired control bits without probing or migrating SD paths.
     * Existing device records keep their publisher/app bindings intact. */
    for (size_t i = 0; i < na; ++i)
        r->apps[i].flags &= PXA_LOCATOR_ACTIVE_FLAGS;
    r->publisher_count = np;
    r->app_count = na;
    r->generation = generation;
    r->ready = 1;
    return PXA_STATUS_OK;
}
pxa_status_t pxa_app_locators_reserve(pxa_app_locators_t *r, uint8_t kind, const uint8_t ns[32],
                                      const char *handle, const uint8_t root[32],
                                      const char *app_id, const uint8_t app_key[32],
                                      uint16_t flags) {
    if (!r || !ns || !root || !app_key || !app_id_valid(app_id) ||
        !publisher_handle_valid(handle) || reserved(handle) || (kind != 1 && kind != 2) ||
        ((flags & ~PXA_LOCATOR_ACTIVE_FLAGS) || ((flags >> 3) & ~flags & 3u)))
        return PXA_STATUS_INVALID_ARGUMENT;
    if (!r->ready)
        return PXA_STATUS_UNAVAILABLE;
    for (size_t i = 0; i < r->app_count; ++i)
        if (!memcmp(r->apps[i].root, root, 32) && !strcmp(r->apps[i].app_id, app_id))
            return PXA_STATUS_OK;
    if (r->app_count == PXA_LOCATOR_MAX_APPS || r->generation == UINT64_MAX)
        return PXA_STATUS_RESOURCE_LIMIT;
    size_t pub = 0;
    for (; pub < r->publisher_count; ++pub)
        if (r->publishers[pub].kind == kind && !memcmp(r->publishers[pub].key, ns, 32))
            break;
    if (pub == r->publisher_count && pub == PXA_LOCATOR_MAX_PUBLISHERS)
        return PXA_STATUS_RESOURCE_LIMIT;
    pxa_locator_publisher_t *p = &r->publishers[pub];
    if (pub == r->publisher_count) {
        memset(p, 0, sizeof(*p));
        p->kind = kind;
        memcpy(p->key, ns, 32);
        if (!choose(r, 0, 1, handle, ns, p->directory))
            return PXA_STATUS_RESOURCE_LIMIT;
    }
    pxa_locator_app_t *a = &r->apps[r->app_count];
    memset(a, 0, sizeof(*a));
    a->publisher = (uint16_t)pub;
    a->flags = flags;
    memcpy(a->root, root, 32);
    strcpy(a->app_id, app_id);
    char base[65];
    snprintf(base, sizeof(base), "%s", app_id);
    if (!directory_valid(base) || reserved(base)) {
        char hash[53];
        fingerprint(app_key, hash);
        snprintf(base, sizeof(base), "app--%.12s", hash);
    }
    if (!choose(r, pub, 0, base, app_key, a->directory))
        return PXA_STATUS_RESOURCE_LIMIT;
    size_t np = r->publisher_count + (pub == r->publisher_count), na = r->app_count + 1;
    pxa_status_t status =
        r->commit ? r->commit(r->context, r->generation + 1, r->publishers, np, r->apps, na)
                  : PXA_STATUS_BAD_STATE;
    if (status != PXA_STATUS_OK) {
        r->ready = 0;
        return status;
    } /* Reload after uncertain durability. */
    r->publisher_count = np;
    r->app_count = na;
    ++r->generation;
    return PXA_STATUS_OK;
}
pxa_status_t pxa_app_locators_find(const pxa_app_locators_t *r, const uint8_t root[32],
                                   const char *app_id, uint16_t *flags, char *out,
                                   size_t capacity) {
    if (!r || !root || !app_id || !out || !capacity)
        return PXA_STATUS_INVALID_ARGUMENT;
    out[0] = 0;
    if (flags)
        *flags = 0;
    if (!r->ready)
        return PXA_STATUS_UNAVAILABLE;
    for (size_t i = 0; i < r->app_count; ++i)
        if (!memcmp(r->apps[i].root, root, 32) && !strcmp(r->apps[i].app_id, app_id)) {
            const pxa_locator_app_t *a = &r->apps[i];
            int n = snprintf(out, capacity, "%s/%s", r->publishers[a->publisher].directory,
                             a->directory);
            if (n < 0 || (size_t)n >= capacity) {
                out[0] = 0;
                return PXA_STATUS_RESOURCE_LIMIT;
            }
            if (flags)
                *flags = a->flags;
            return PXA_STATUS_OK;
        }
    return PXA_STATUS_NOT_FOUND;
}

static uint32_t record_crc(const uint8_t *p, size_t n) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1)));
    }
    return ~crc;
}
static void put_le(uint8_t *p, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}
static uint64_t get_le(const uint8_t *p, unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
pxa_status_t pxa_app_locators_encode(uint64_t generation, const pxa_locator_publisher_t *pubs,
                                     size_t np, const pxa_locator_app_t *apps, size_t na,
                                     uint8_t *out, size_t capacity, size_t *size) {
    if (!out || !size || np > PXA_LOCATOR_MAX_PUBLISHERS || na > PXA_LOCATOR_MAX_APPS ||
        (np && !pubs) || (na && !apps))
        return PXA_STATUS_INVALID_ARGUMENT;
    *size = 24 + np * 98 + na * 166;
    if (capacity < *size)
        return PXA_STATUS_RESOURCE_LIMIT;
    memcpy(out, "PXAL\1\0\0\0", 8);
    put_le(out + 8, generation, 8);
    put_le(out + 16, np, 2);
    put_le(out + 18, na, 2);
    uint8_t *p = out + 20;
    for (size_t i = 0; i < np; ++i) {
        *p++ = pubs[i].kind;
        memcpy(p, pubs[i].key, 32);
        p += 32;
        memcpy(p, pubs[i].directory, 65);
        p += 65;
    }
    for (size_t i = 0; i < na; ++i) {
        put_le(p, apps[i].publisher, 2);
        put_le(p + 2, apps[i].flags & PXA_LOCATOR_ACTIVE_FLAGS, 2);
        p += 4;
        memcpy(p, apps[i].root, 32);
        p += 32;
        memcpy(p, apps[i].app_id, 65);
        p += 65;
        memcpy(p, apps[i].directory, 65);
        p += 65;
    }
    put_le(p, record_crc(out, *size - 4), 4);
    return PXA_STATUS_OK;
}
pxa_status_t pxa_app_locators_decode(pxa_app_locators_t *r, const uint8_t *data, size_t size) {
    if (!r)
        return PXA_STATUS_INVALID_ARGUMENT;
    r->ready = 0;
    if (!data || size < 24 || size > PXA_LOCATOR_ENCODED_MAX || memcmp(data, "PXAL\1\0\0\0", 8))
        return PXA_STATUS_PROTOCOL_ERROR;
    size_t np = (size_t)get_le(data + 16, 2), na = (size_t)get_le(data + 18, 2);
    if (np > PXA_LOCATOR_MAX_PUBLISHERS || na > PXA_LOCATOR_MAX_APPS ||
        size != 24 + np * 98 + na * 166 || get_le(data + size - 4, 4) != record_crc(data, size - 4))
        return PXA_STATUS_PROTOCOL_ERROR;
    const uint8_t *p = data + 20;
    for (size_t i = 0; i < np; ++i) {
        r->publishers[i].kind = *p++;
        memcpy(r->publishers[i].key, p, 32);
        p += 32;
        memcpy(r->publishers[i].directory, p, 65);
        p += 65;
    }
    for (size_t i = 0; i < na; ++i) {
        r->apps[i].publisher = (uint16_t)get_le(p, 2);
        r->apps[i].flags = (uint16_t)get_le(p + 2, 2);
        p += 4;
        memcpy(r->apps[i].root, p, 32);
        p += 32;
        memcpy(r->apps[i].app_id, p, 65);
        p += 65;
        memcpy(r->apps[i].directory, p, 65);
        p += 65;
    }
    return pxa_app_locators_load(r, get_le(data + 8, 8), r->publishers, np, r->apps, na);
}

pxa_status_t pxa_app_locators_set_flags(pxa_app_locators_t *r, const uint8_t root[32],
                                        const char *app_id, uint16_t flags) {
    if (!r || !root || !app_id || (flags & ~PXA_LOCATOR_ACTIVE_FLAGS) || ((flags >> 3) & ~flags & 3u))
        return PXA_STATUS_INVALID_ARGUMENT;
    if (!r->ready)
        return PXA_STATUS_UNAVAILABLE;
    for (size_t i = 0; i < r->app_count; ++i)
        if (!memcmp(root, r->apps[i].root, 32) && !strcmp(app_id, r->apps[i].app_id)) {
            if (r->apps[i].flags == flags)
                return PXA_STATUS_OK;
            if (!r->commit || r->generation == UINT64_MAX)
                return PXA_STATUS_RESOURCE_LIMIT;
            r->apps[i].flags = flags;
            pxa_status_t status = r->commit(r->context, r->generation + 1, r->publishers,
                                            r->publisher_count, r->apps, r->app_count);
            if (status != PXA_STATUS_OK) {
                r->ready = 0;
                return status;
            }
            ++r->generation;
            return PXA_STATUS_OK;
        }
    return PXA_STATUS_NOT_FOUND;
}

pxa_status_t pxa_app_locators_forget(pxa_app_locators_t *r,const uint8_t root[32],const char *app_id) {
    if(!r || !root || !app_id)return PXA_STATUS_INVALID_ARGUMENT;
    if(!r->ready)return PXA_STATUS_UNAVAILABLE;
    for(size_t i=0;i<r->app_count;++i) {
        if(memcmp(root,r->apps[i].root,32) || strcmp(app_id,r->apps[i].app_id))continue;
        if(r->apps[i].flags&(PXA_LOCATOR_PACKAGE_MOVING|PXA_LOCATOR_DATA_MOVING))return PXA_STATUS_BUSY;
        if(!r->commit || r->generation==UINT64_MAX)return PXA_STATUS_RESOURCE_LIMIT;
        pxa_locator_app_t removed=r->apps[i];size_t remaining=r->app_count-i-1;
        memmove(r->apps+i,r->apps+i+1,remaining*sizeof(*r->apps));
        pxa_status_t status=r->commit(r->context,r->generation+1,r->publishers,r->publisher_count,r->apps,r->app_count-1);
        if(status!=PXA_STATUS_OK) {
            /* Retaining the retired mapping is conservative even after an
             * uncertain deletion. Unlike allocation, this cannot give an
             * existing namespace to another publisher/application. */
            memmove(r->apps+i+1,r->apps+i,remaining*sizeof(*r->apps));r->apps[i]=removed;
            return status;
        }
        --r->app_count;memset(r->apps+r->app_count,0,sizeof(*r->apps));++r->generation;return PXA_STATUS_OK;
    }
    return PXA_STATUS_NOT_FOUND;
}
