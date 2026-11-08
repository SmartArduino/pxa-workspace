#include "pxa/pxa_esp_system_font.h"
#include "sdkconfig.h"
#include "mbedtls/md.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Factory resource identity, independent of untrusted removable filenames.
 * Update this digest with the factory resource when changing the font. */
#define FONT_NAME "noto_sans_cjk_common.ttf"
typedef struct {
    const char *name;
    const char *internal;
    size_t bytes;
    unsigned char sha256[32];
} FontResource;
static const FontResource resources[] = {
    {FONT_NAME, CONFIG_PXA_MOUNT_POINT "/system/fonts/" FONT_NAME, 2788052u,
     {0xad,0x9a,0xce,0x36,0x9f,0x9e,0xd2,0xa2,0xc4,0x40,0xe1,0x00,0x1c,0x18,0x68,0x61,
      0x97,0xe3,0x7b,0xbd,0x68,0x0a,0xe5,0xff,0x7e,0x42,0x14,0xa1,0xdc,0x9f,0x9b,0x0d}},
    {"pxa_sans_sc_common.woff", CONFIG_PXA_MOUNT_POINT "/system/fonts/pxa_sans_sc_common.woff", 1777356u,
     {0xef,0x15,0x76,0x4a,0x9b,0x40,0x34,0xc6,0xb6,0xd5,0x00,0x7b,0xba,0x7b,0x98,0x27,
      0x65,0x05,0x44,0x21,0xc3,0x34,0x71,0x23,0xc1,0x0f,0x95,0xbe,0x07,0x9d,0xcd,0x21}},
};
static char selected[384] = CONFIG_PXA_MOUNT_POINT "/system/fonts/" FONT_NAME;
static bool selected_external;

static bool verified(const char *path, const FontResource *resource) {
    unsigned char bytes[4096], hash[32]; size_t count, total = 0;
    FILE *f = fopen(path, "rb"); if (!f) return false;
    mbedtls_md_context_t md; mbedtls_md_init(&md);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = info && mbedtls_md_setup(&md, info, 0) == 0 && mbedtls_md_starts(&md) == 0;
    while (ok && (count = fread(bytes, 1, sizeof(bytes), f)) != 0) {
        total += count;
        ok = total <= resource->bytes && mbedtls_md_update(&md, bytes, count) == 0;
    }
    ok = ok && !ferror(f) && total == resource->bytes && mbedtls_md_finish(&md, hash) == 0 &&
         memcmp(hash, resource->sha256, sizeof(hash)) == 0;
    mbedtls_md_free(&md); fclose(f); return ok;
}
static bool directory(const char *path) {
    struct stat st;
    return (mkdir(path, 0775) == 0 || errno == EEXIST) &&
           stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool copy_resource(const char *source, const char *destination, const char *temporary) {
    unsigned char bytes[4096]; size_t count;
    FILE *in = fopen(source, "rb"); if (!in) return false;
    FILE *out = fopen(temporary, "wb");
    if (!out) { fclose(in); return false; }
    bool ok = true;
    while ((count = fread(bytes, 1, sizeof(bytes), in)) != 0)
        if (fwrite(bytes, 1, count, out) != count) { ok = false; break; }
    ok = ok && !ferror(in) && fflush(out) == 0 && fsync(fileno(out)) == 0;
    if (fclose(out) != 0) ok = false;
    fclose(in);
    if (ok) ok = rename(temporary, destination) == 0;
    if (!ok) (void)unlink(temporary);
    return ok;
}
const char *pxa_esp_system_font_path(void) {
    if (!selected_external) {
        struct stat st;
        strcpy(selected, resources[0].internal);
        for (size_t i = 0; i < sizeof(resources)/sizeof(resources[0]); ++i) {
            if (stat(resources[i].internal, &st) == 0 && S_ISREG(st.st_mode)) {
                strcpy(selected, resources[i].internal);
                break;
            }
        }
    }
    return selected;
}
bool pxa_esp_system_font_prepare(const char *mount, bool reclaim_internal) {
    char root[320], system[336], fonts[352], font[384], temporary[384];
    struct stat st;
    (void)pxa_esp_system_font_path();
    /* Never create the mount point: an absent card must not fill Flash. */
    if (!mount || mount[0] != '/' || stat(mount, &st) != 0 || !S_ISDIR(st.st_mode) ||
        snprintf(root, sizeof(root), "%s/PXA", mount) >= (int)sizeof(root) ||
        snprintf(system, sizeof(system), "%s/system", root) >= (int)sizeof(system) ||
        snprintf(fonts, sizeof(fonts), "%s/fonts", system) >= (int)sizeof(fonts) ||
        snprintf(temporary, sizeof(temporary), "%s/.font-copy", fonts) >= (int)sizeof(temporary))
        return false;
    for (size_t i = 0; i < sizeof(resources)/sizeof(resources[0]); ++i) {
        const FontResource *resource = &resources[i];
        if (snprintf(font, sizeof(font), "%s/%s", fonts, resource->name) >= (int)sizeof(font)) return false;
        bool trusted_internal = verified(resource->internal, resource);
        if (!verified(font, resource)) {
            /* Preserve a different resource rather than overwriting by name. */
            if (stat(font, &st) == 0 || !trusted_internal) continue;
            if (!directory(root) || !directory(system) || !directory(fonts) ||
                !copy_resource(resource->internal, font, temporary) || !verified(font, resource)) return false;
        }
        /* Licenses stay with the exact font on removable media. */
        const char *licenses[] = {"NOTICE", "OFL.txt", "OFL-Latin.txt"};
        for (size_t j = 0; j < sizeof(licenses)/sizeof(licenses[0]); ++j) {
            char source[384], destination[384];
            if (snprintf(source, sizeof(source), CONFIG_PXA_MOUNT_POINT "/system/fonts/%s", licenses[j]) >= (int)sizeof(source) ||
                snprintf(destination, sizeof(destination), "%s/%s", fonts, licenses[j]) >= (int)sizeof(destination)) return false;
            if (stat(destination, &st) != 0 && !copy_resource(source, destination, temporary)) return false;
        }
        strcpy(selected, font);
        selected_external = true;
        /* Reclaim only the known factory copy, after reopening and hashing SD.
         * An interrupted copy always leaves the internal copy available. */
        if (reclaim_internal && trusted_internal) (void)unlink(resource->internal);
        return true;
    }
    return false;
}
