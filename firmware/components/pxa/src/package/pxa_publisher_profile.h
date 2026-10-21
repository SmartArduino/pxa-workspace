#ifndef PXA_PUBLISHER_PROFILE_H
#define PXA_PUBLISHER_PROFILE_H
#include <stddef.h>
#include <stdint.h>
#include "pxa/status.h"
#define PXA_PUBLISHER_PROFILE_PATH "assets/pxa-publisher.json"
#define PXA_PUBLISHER_PROFILE_BYTES 1024u
/* Display claims only. A caller must authenticate the containing manifest/file
 * before storing or presenting this as signer-provided metadata. No authority. */
typedef struct {
    char handle[25];
    char display_name[129];
} pxa_publisher_profile_t;
pxa_status_t pxa_publisher_profile_validate(const char *, const char *, pxa_publisher_profile_t *);
pxa_status_t pxa_publisher_profile_parse(const uint8_t *json, size_t bytes,
                                         pxa_publisher_profile_t *profile);
#endif
