#ifndef PXA_STORE_HOST_PROFILE_H
#define PXA_STORE_HOST_PROFILE_H

#include <string.h>
#include <strings.h>

#define PXA_STORE_HOST_PROFILE_HEADER "X-PXA-Host-Profile"

/* A published PXA capability snapshot, NOT the board's firmware version.
 * Keep the snapshot stable across patches that do not change compatibility.
 * The official Host supplies Container 0.2, DEFLATE and Store install 0.8.
 * Unknown targets must not claim another target's capabilities. */
static const char *pxa_store_host_profile(void) {
#if defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
    return "esp32-s3-pxa-0.7.0";
#elif defined(CONFIG_IDF_TARGET_ESP32S31) && CONFIG_IDF_TARGET_ESP32S31
    return "esp32-s31-pxa-0.7.0";
#else
    return NULL;
#endif
}

/* Only the official HTTPS discovery API needs the declaration. In particular,
 * never attach it to arbitrary app URLs, download hosts or redirects. */
static int pxa_store_host_profile_url(const char *url) {
    static const char origin[] = "https://app.doit.am";
    const char *path;
    if (url == NULL || strncasecmp(url, origin, sizeof(origin) - 1) != 0)
        return 0;
    path = url + sizeof(origin) - 1;
    if (strncmp(path, ":443/", 5) == 0) path += 4;
    if (strncmp(path, "/api/v2/apps/", 13) == 0 && path[13] != '\0' &&
        path[13] != '?' && path[13] != '#')
        return 1;
    const char *routes[] = {"/api/v2/catalog", "/api/v2/device/update-check"};
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        size_t length = strlen(routes[i]);
        if (strncmp(path, routes[i], length) == 0 &&
            (path[length] == '\0' || path[length] == '?' || path[length] == '#'))
            return 1;
    }
    return 0;
}

#endif
