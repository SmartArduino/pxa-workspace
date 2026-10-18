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
static const unsigned char font_sha256[32] = {
    0x12,0x1c,0xb2,0x06,0xd8,0x26,0xaf,0x52,0x59,0xcf,0xb7,0x84,0x18,0xde,0xfd,0xe6,
    0x51,0xa9,0xda,0xa8,0x0f,0xa1,0xd7,0xd5,0x1d,0xe3,0xee,0xa7,0x16,0xc9,0x34,0xa4};
#define FONT_NAME "noto_sans_cjk_common.ttf"
static const char internal[] = CONFIG_PXA_MOUNT_POINT "/system/fonts/" FONT_NAME;
static char selected[384] = CONFIG_PXA_MOUNT_POINT "/system/fonts/" FONT_NAME;

static bool verified(const char *path) {
    unsigned char bytes[4096], hash[32]; size_t count, total = 0;
    FILE *f = fopen(path, "rb"); if (!f) return false;
    mbedtls_md_context_t md; mbedtls_md_init(&md);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    bool ok = info && mbedtls_md_setup(&md, info, 0) == 0 && mbedtls_md_starts(&md) == 0;
    while (ok && (count = fread(bytes, 1, sizeof(bytes), f)) != 0) {
        total += count;
        ok = total <= 2777480u && mbedtls_md_update(&md, bytes, count) == 0;
    }
    ok = ok && !ferror(f) && total == 2777480u && mbedtls_md_finish(&md, hash) == 0 &&
         memcmp(hash, font_sha256, sizeof(hash)) == 0;
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
const char *pxa_esp_system_font_path(void) { return selected; }
bool pxa_esp_system_font_prepare(const char *mount, bool reclaim_internal) {
    char root[320], system[336], fonts[352], font[384], temporary[384];
    struct stat st;
    /* Never create the mount point: an absent card must not fill Flash. */
    if (!mount || mount[0] != '/' || stat(mount, &st) != 0 || !S_ISDIR(st.st_mode) ||
        snprintf(root, sizeof(root), "%s/PXA", mount) >= (int)sizeof(root) ||
        snprintf(system, sizeof(system), "%s/system", root) >= (int)sizeof(system) ||
        snprintf(fonts, sizeof(fonts), "%s/fonts", system) >= (int)sizeof(fonts) ||
        snprintf(font, sizeof(font), "%s/" FONT_NAME, fonts) >= (int)sizeof(font) ||
        snprintf(temporary, sizeof(temporary), "%s/.font-copy", fonts) >= (int)sizeof(temporary))
        return false;
    bool trusted_internal = verified(internal);
    if (!verified(font)) {
        /* Preserve a different resource rather than overwriting by name. */
        if (stat(font, &st) == 0 || !trusted_internal || !directory(root) ||
            !directory(system) || !directory(fonts) ||
            !copy_resource(internal, font, temporary) || !verified(font)) return false;
    }
    /* Licenses stay with the exact font on removable media. */
    const char *licenses[] = {"NOTICE", "OFL.txt"};
    for (size_t i = 0; i < sizeof(licenses)/sizeof(licenses[0]); ++i) {
        char source[384], destination[384];
        if (snprintf(source, sizeof(source), CONFIG_PXA_MOUNT_POINT "/system/fonts/%s", licenses[i]) >= (int)sizeof(source) ||
            snprintf(destination, sizeof(destination), "%s/%s", fonts, licenses[i]) >= (int)sizeof(destination)) return false;
        if (stat(destination, &st) != 0 && !copy_resource(source, destination, temporary)) return false;
    }
    strcpy(selected, font);
    /* Reclaim only the known factory copy, after reopening and hashing SD.
     * An interrupted copy always leaves the internal copy available. */
    if (reclaim_internal && trusted_internal) (void)unlink(internal);
    return true;
}
