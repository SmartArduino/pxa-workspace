#ifndef PXA_TEST_ESP_LITTLEFS_H
#define PXA_TEST_ESP_LITTLEFS_H
#include "pxa_test_platform.h"
#include "esp_blockdev.h"
typedef struct {
    const char *base_path,*partition_label;
    esp_blockdev_handle_t blockdev;
    bool format_if_mount_failed,read_only,dont_mount,grow_on_mount;
} esp_vfs_littlefs_conf_t;
esp_err_t esp_littlefs_info(const char *, size_t *, size_t *);
esp_err_t esp_littlefs_format_blockdev(esp_blockdev_handle_t);
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *);
esp_err_t esp_vfs_littlefs_unregister_blockdev(esp_blockdev_handle_t);
bool esp_littlefs_blockdev_mounted(esp_blockdev_handle_t);
#endif
