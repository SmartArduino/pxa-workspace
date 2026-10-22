#ifndef PXA_ESP_PACKAGE_LIMITS_H
#define PXA_ESP_PACKAGE_LIMITS_H
/* Product metadata bound, shared by Store preflight and cold disk recovery.
 * Check untrusted file sizes before allocating a signed manifest buffer. */
#define PXA_ESP_SIGNED_MANIFEST_MAX_BYTES 16384u
#endif
