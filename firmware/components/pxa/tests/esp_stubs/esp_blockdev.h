#ifndef PXA_TEST_ESP_BLOCKDEV_H
#define PXA_TEST_ESP_BLOCKDEV_H
#include "pxa_test_platform.h"
enum {ESP_ERR_INVALID_ARG=10,ESP_ERR_INVALID_STATE,ESP_ERR_NOT_SUPPORTED};
#define ESP_BLOCKDEV_CMD_MARK_DELETED 0u
#define ESP_BLOCKDEV_CMD_ERASE_CONTENTS 1u
typedef struct {uint64_t start_addr;size_t erase_len;} esp_blockdev_cmd_arg_erase_t;
typedef struct {
    uint64_t disk_size;
    size_t read_size,write_size,erase_size;
    size_t recommended_write_size,recommended_read_size,recommended_erase_size;
} esp_blockdev_geometry_t;
typedef struct esp_blockdev esp_blockdev_t;
typedef esp_blockdev_t *esp_blockdev_handle_t;
typedef struct {
    esp_err_t (*read)(esp_blockdev_handle_t,uint8_t *,size_t,uint64_t,size_t);
    esp_err_t (*write)(esp_blockdev_handle_t,const uint8_t *,uint64_t,size_t);
    esp_err_t (*erase)(esp_blockdev_handle_t,uint64_t,size_t);
    esp_err_t (*sync)(esp_blockdev_handle_t);
    esp_err_t (*ioctl)(esp_blockdev_handle_t,uint8_t,void *);
    esp_err_t (*release)(esp_blockdev_handle_t);
} esp_blockdev_ops_t;
struct esp_blockdev {
    void *ctx;
    struct {unsigned read_only:1,encrypted:1,erase_before_write:1,and_type_write:1,default_val_after_erase:1;} device_flags;
    esp_blockdev_geometry_t geometry;
    const esp_blockdev_ops_t *ops;
};
#endif
