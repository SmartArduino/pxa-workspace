/* SPDX-License-Identifier: MIT */
#include "lfs.h"
#include "pxa_littlefs_usage.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BLOCK_SIZE = 4096, BLOCKS = 2304, FONT_BYTES = 2788052 };
static unsigned char flash[BLOCK_SIZE * BLOCKS];
static int fail_read, fail_alloc, locked, warnings;
static unsigned writes;
static size_t last_allocation;
typedef struct { struct lfs_config cfg; lfs_t *fs; } esp_littlefs_t;
static void sem_take(esp_littlefs_t *fs) { (void)fs; assert(!locked); locked = 1; }
static void sem_give(esp_littlefs_t *fs) { (void)fs; assert(locked); locked = 0; }
static void *esp_littlefs_calloc(size_t count, size_t size) {
    last_allocation = count * size;
    return fail_alloc ? NULL : calloc(count, size);
}
#define ESP_LITTLEFS_TAG "test"
#define ESP_LOGW(tag, format, result) do { \
    (void)(tag); (void)(format); assert((result) < 0); assert(!locked); warnings++; \
} while (0)

/* The runner injects the exact function emitted for the production driver. */
#include "generated_usage.c"

static int read_cb(const struct lfs_config *c, lfs_block_t b, lfs_off_t o,
                   void *p, lfs_size_t n) {
    (void)c; assert(b < BLOCKS && o + n <= BLOCK_SIZE);
    if (fail_read) return LFS_ERR_IO;
    memcpy(p, flash + b * BLOCK_SIZE + o, n); return 0;
}
static int prog_cb(const struct lfs_config *c, lfs_block_t b, lfs_off_t o,
                   const void *p, lfs_size_t n) {
    (void)c; assert(b < BLOCKS && o + n <= BLOCK_SIZE); writes++;
    for (unsigned i = 0; i < n; i++) {
        unsigned char value = ((const unsigned char *)p)[i];
        assert((flash[b * BLOCK_SIZE + o + i] & value) == value);
        flash[b * BLOCK_SIZE + o + i] = value;
    }
    return 0;
}
static int erase_cb(const struct lfs_config *c, lfs_block_t b) {
    (void)c; assert(b < BLOCKS); writes++;
    memset(flash + b * BLOCK_SIZE, 255, BLOCK_SIZE); return 0;
}
static int sync_cb(const struct lfs_config *c) { (void)c; return 0; }
static size_t used(esp_littlefs_t *fs) {
    size_t total = 0, occupied = 0;
    const unsigned before = writes;
    get_total_and_used_bytes(fs, &total, &occupied);
    assert(total == sizeof flash && occupied <= total && !locked);
    assert(writes == before); /* Statistics must never modify the filesystem. */
    return occupied;
}

int main(void) {
    struct lfs_config cfg = {
        .read=read_cb, .prog=prog_cb, .erase=erase_cb, .sync=sync_cb,
        .read_size=16, .prog_size=16, .block_size=BLOCK_SIZE, .block_count=BLOCKS,
        .block_cycles=500, .cache_size=512, .lookahead_size=256
    };
    lfs_t lfs;
    memset(flash, 255, sizeof flash);
    assert(!lfs_format(&lfs, &cfg)); assert(!lfs_mount(&lfs, &cfg));
    esp_littlefs_t driver = { .cfg=cfg, .fs=&lfs };
    lfs_file_t file;
    assert(!lfs_file_open(&lfs, &file, "font.ttf", LFS_O_WRONLY | LFS_O_CREAT));
    unsigned char buffer[4096]; memset(buffer, 0x5a, sizeof buffer);
    for (unsigned offset=0; offset<FONT_BYTES;) {
        unsigned count=FONT_BYTES-offset;
        if (count>sizeof buffer) count=sizeof buffer;
        assert(lfs_file_write(&lfs, &file, buffer, count)==(lfs_ssize_t)count);
        offset+=count;
    }
    assert(!lfs_file_close(&lfs, &file));
    const size_t baseline = used(&driver);
    assert(last_allocation == (BLOCKS+7u)/8u);
    lfs_file_t held, held2;
    assert(!lfs_file_open(&lfs, &held, "font.ttf", LFS_O_RDONLY));
    assert(!lfs_file_open(&lfs, &held2, "font.ttf", LFS_O_RDONLY));
    unsigned char *snapshot=malloc(sizeof flash); assert(snapshot);
    memcpy(snapshot, flash, sizeof flash);
    for (unsigned i=0; i<8; i++) {
        assert(!lfs_file_open(&lfs, &file, "font.ttf", LFS_O_RDONLY));
        assert(lfs_file_read(&lfs, &file, buffer, sizeof buffer)==sizeof buffer);
        assert(!lfs_file_close(&lfs, &file));
        assert((size_t)lfs_fs_size(&lfs)*BLOCK_SIZE > baseline);
        assert(used(&driver)==baseline);
        assert(!memcmp(snapshot, flash, sizeof flash));
    }
    /* Real copy-on-write blocks held by older readers must remain counted. */
    assert(!lfs_file_open(&lfs, &file, "font.ttf", LFS_O_RDWR));
    const unsigned char replacement=0xa5;
    assert(lfs_file_write(&lfs, &file, &replacement, 1)==1);
    assert(!lfs_file_close(&lfs, &file));
    const size_t with_old_readers=used(&driver);
    assert(with_old_readers>baseline);
    unsigned char old_byte=0;
    assert(lfs_file_read(&lfs, &held, &old_byte, 1)==1 && old_byte==0x5a);
    assert(!lfs_file_close(&lfs, &held));
    assert(used(&driver)==with_old_readers);
    assert(!lfs_file_close(&lfs, &held2));
    assert(used(&driver)<with_old_readers);
    assert(!lfs_file_open(&lfs, &file, "font.ttf", LFS_O_RDONLY));
    assert(lfs_file_read(&lfs, &file, &old_byte, 1)==1 && old_byte==replacement);
    assert(!lfs_file_close(&lfs, &file));

    fail_alloc=1; assert(used(&driver)==sizeof flash); fail_alloc=0;
    fail_read=1; assert(used(&driver)==sizeof flash); fail_read=0;
    assert(warnings==2);
    fail_alloc=1;
    size_t total=0;
    get_total_and_used_bytes(&driver, &total, NULL);
    get_total_and_used_bytes(&driver, NULL, NULL);
    assert(total==sizeof flash && warnings==2 && !locked);
    fail_alloc=0; assert(used(&driver)<sizeof flash);
    uint8_t bitmap=0;
    pxa_littlefs_usage_t bounds={ .seen=&bitmap, .capacity=1, .used=0 };
    assert(pxa_littlefs_usage_visit(&bounds, 1)==LFS_ERR_CORRUPT && !bitmap);
    assert(!lfs_unmount(&lfs)); free(snapshot);
    puts("LittleFS real readonly handles, retained COW versions, OOM/IO failure and non-mutating statistics passed");
    return 0;
}
