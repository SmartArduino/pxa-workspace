#include "pxa_publisher_profile.h"
#include "cJSON.h"
#include <string.h>
static int reserved(const char *s) {
    static const char *const names[] = {"system",   "app", "data", "media", "pxa", "admin",
                                        "official", "con", "prn",  "aux",   "nul"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(s, names[i]))
            return 1;
    return strlen(s) == 4 && ((!memcmp(s, "com", 3)) || (!memcmp(s, "lpt", 3))) && s[3] >= '1' &&
           s[3] <= '9';
}
static int display_valid(const unsigned char *p, size_t *begin, size_t *end) {
    /* Unicode 16.0.0 Cc/Cf/Cs, matching the shared package/server rules. */
    static const unsigned forbidden[][2] = {
        {0x0, 0x1f},        {0x7f, 0x9f},       {0xad, 0xad},       {0x600, 0x605},
        {0x61c, 0x61c},     {0x6dd, 0x6dd},     {0x70f, 0x70f},     {0x890, 0x891},
        {0x8e2, 0x8e2},     {0x180e, 0x180e},   {0x200b, 0x200f},   {0x202a, 0x202e},
        {0x2060, 0x2064},   {0x2066, 0x206f},   {0xd800, 0xdfff},   {0xfeff, 0xfeff},
        {0xfff9, 0xfffb},   {0x110bd, 0x110bd}, {0x110cd, 0x110cd}, {0x13430, 0x1343f},
        {0x1bca0, 0x1bca3}, {0x1d173, 0x1d17a}, {0xe0001, 0xe0001}, {0xe0020, 0xe007f}};
    const unsigned char *base = p;
    *begin = SIZE_MAX;
    *end = 0;
    while (*p) {
        const unsigned char *at = p;
        unsigned cp = *p++, n = 0, min = 0;
        if (cp < 128) {
        } else if (cp >= 0xc2 && cp <= 0xdf) {
            n = 1;
            cp &= 31;
            min = 128;
        } else if (cp >= 0xe0 && cp <= 0xef) {
            n = 2;
            cp &= 15;
            min = 2048;
        } else if (cp >= 0xf0 && cp <= 0xf4) {
            n = 3;
            cp &= 7;
            min = 65536;
        } else
            return 0;
        for (unsigned i = 0; i < n; ++i) {
            if (!*p || (*p & 0xc0) != 0x80)
                return 0;
            cp = (cp << 6) | (*p++ & 63);
        }
        if (cp < min || cp > 0x10ffff)
            return 0;
        for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i)
            if (cp >= forbidden[i][0] && cp <= forbidden[i][1])
                return 0;
        int space = cp == 32 || cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) ||
                    cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
        if (!space) {
            if (*begin == SIZE_MAX)
                *begin = (size_t)(at - base);
            *end = (size_t)(p - base);
        }
    }
    return *begin != SIZE_MAX;
}
/* Bound cJSON's allocations before parsing: this profile has exactly three
 * string/string pairs, no arrays, nesting, numeric values or embedded NUL. */
static int bounded_object(const uint8_t *p, size_t bytes) {
    unsigned quotes = 0, opens = 0, closes = 0;
    int string = 0;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned c = p[i];
        if (string && c == '\\') {
            if (i + 1 >= bytes)
                return 0;
            if (p[i + 1] == 'u' && i + 5 < bytes && !memcmp(p + i + 2, "0000", 4))
                return 0;
            ++i;
            continue;
        }
        if (c == '"') {
            string = !string;
            if (++quotes > 12)
                return 0;
            continue;
        }
        if (string)
            continue;
        if (c == '{') {
            if (++opens > 1)
                return 0;
        } else if (c == '}') {
            if (++closes > 1)
                return 0;
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != ',' && c != ':')
            return 0;
    }
    return !string && quotes == 12 && opens == 1 && closes == 1;
}
pxa_status_t pxa_publisher_profile_parse(const uint8_t *data, size_t bytes,
                                         pxa_publisher_profile_t *out) {
    if (!out)
        return PXA_STATUS_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!data || !bytes || bytes > PXA_PUBLISHER_PROFILE_BYTES || memchr(data, 0, bytes) ||
        !bounded_object(data, bytes))
        return PXA_STATUS_INVALID_ARGUMENT;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts((const char *)data, bytes, &end, 0);
    if (!root)
        return PXA_STATUS_INVALID_ARGUMENT;
    pxa_status_t status = PXA_STATUS_INVALID_ARGUMENT;
    if (!cJSON_IsObject(root) || end < (const char *)data || end > (const char *)data + bytes)
        goto done;
    while (end < (const char *)data + bytes &&
           (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
        ++end;
    if (end != (const char *)data + bytes)
        goto done;
    const char *schema = NULL, *handle = NULL, *name = NULL;
    for (const cJSON *it = root->child; it; it = it->next) {
        if (!it->string || !cJSON_IsString(it) || !it->valuestring)
            goto done;
        if (!strcmp(it->string, "schema")) {
            if (schema)
                goto done;
            schema = it->valuestring;
        } else if (!strcmp(it->string, "handle")) {
            if (handle)
                goto done;
            handle = it->valuestring;
        } else if (!strcmp(it->string, "display_name")) {
            if (name)
                goto done;
            name = it->valuestring;
        } else
            goto done;
    }
    if (schema && !strcmp(schema, "pxa.publisher-profile/1"))
        status = pxa_publisher_profile_validate(handle, name, out);
done:
    cJSON_Delete(root);
    return status;
}

pxa_status_t pxa_publisher_profile_validate(const char *handle, const char *name,
                                            pxa_publisher_profile_t *out) {
    if (!out)
        return PXA_STATUS_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    size_t name_begin = 0, name_end = 0;
    if (!handle || !name || !*name || strlen(handle) > 24 || strlen(name) > 128 ||
        reserved(handle) || handle[0] < 'a' || handle[0] > 'z' ||
        !display_valid((const unsigned char *)name, &name_begin, &name_end))
        return PXA_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; handle[i]; ++i)
        if (!((handle[i] >= 'a' && handle[i] <= 'z') || (handle[i] >= '0' && handle[i] <= '9') ||
              (handle[i] == '-' && i && handle[i - 1] != '-' && handle[i + 1])))
            return PXA_STATUS_INVALID_ARGUMENT;
    memcpy(out->handle, handle, strlen(handle) + 1);
    memcpy(out->display_name, name + name_begin, name_end - name_begin);
    out->display_name[name_end - name_begin] = 0;
    return PXA_STATUS_OK;
}
