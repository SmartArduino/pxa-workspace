#ifndef PXA_ESP_IMAGE_BLOCKDEV_H
#define PXA_ESP_IMAGE_BLOCKDEV_H
#include "pxa/status.h"
#include "esp_blockdev.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Host-only backing device for a private LittleFS image on another volume.
 * The caller authenticates the app home and opens the regular image with
 * no-follow semantics. Its size is fixed, fully allocated and durable before
 * handing it off; this adapter never formats, resizes or reopens by pathname.
 * Exclusive FD ownership, including its native cursor, transfers on success.
 * Do not retain duplicates sharing that cursor. LittleFS borrows this device
 * (release callback is NULL); its caller always closes the owner after a
 * failed registration or successful unmount. LittleFS serializes I/O;
 * close all filesystem users before unmount. Every I/O checks the
 * original physical mount epoch, including reads through a retained fd. */
pxa_status_t pxa_esp_image_blockdev_take(int image_fd,uint64_t mount_epoch,
    esp_blockdev_handle_t *output);
pxa_status_t pxa_esp_image_blockdev_close(esp_blockdev_handle_t *owner);
/* Fill a newly created, EMPTY staging file with erased blocks and fsync it.
 * Never truncates an existing image. Capacity must already be admitted by
 * the Host plan. This borrows the fd, uses its cursor, and does not publish or
 * format the file. */
pxa_status_t pxa_esp_image_blockdev_prepare(int new_fd,uint64_t mount_epoch,
    uint64_t capacity_bytes);
#ifdef __cplusplus
}
#endif
#endif
