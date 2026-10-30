#define _GNU_SOURCE
#include "pxa/pxa_esp_font_source.h"
#include "pxa_esp_volumes.h"
#include "esp_vfs_fat.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYSTEM_H
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned random_state=1,allocations,read_calls;
static int allocation_failures;
static int reject_page_cache;
ssize_t __real_read(int fd,void *buffer,size_t count);
ssize_t __wrap_read(int fd,void *buffer,size_t count) {
    ++read_calls;return __real_read(fd,buffer,count);
}
void *heap_caps_malloc(size_t size,unsigned caps) {
    (void)caps;assert(size<16384); /* Never copy the font into a resource buffer. */
    if(allocation_failures) {--allocation_failures;return NULL;}
    if(reject_page_cache && size==8192)return NULL;
    void *p=malloc(size);if(p)++allocations;return p;
}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps) {
    (void)caps;void *p=calloc(n,size);if(p)++allocations;return p;
}
void heap_caps_free(void *p) {if(p) {assert(allocations);--allocations;free(p);}}
uint32_t esp_random(void) {random_state=random_state*1664525u+1013904223u;return random_state;}
esp_err_t esp_vfs_fat_info(const char *path,uint64_t *total,uint64_t *available) {
    (void)path;*total=UINT64_C(128)*1024*1024;*available=*total;return ESP_OK;
}
static pxa_status_t capacity(void *context,const char *path,uint64_t *total,uint64_t *available,uint8_t *known) {
    (void)context;(void)path;*total=UINT64_C(128)*1024*1024;*available=*total;*known=3;return PXA_STATUS_OK;
}
static uint64_t mount_volume(const char *path,const char *id) {
    pxa_esp_storage_volume_config_t config={0};uint64_t epoch;
    config.struct_size=sizeof(config);config.mount_path=path;config.name=id;config.device_identity=id;
    config.device_identity_bytes=strlen(id);config.capabilities=PXA_VOLUME_READ|PXA_VOLUME_WRITE|PXA_VOLUME_RANDOM_READ;
    config.max_file_bytes=INT32_MAX;config.allocation_unit=4096;config.link_free=1;config.query_space=capacity;
    assert(!pxa_esp_storage_register_volume(&config,&epoch));return epoch;
}
static void copy_file(const char *source,const char *path) {
    int from=open(source,O_RDONLY),to=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(from>=0 && to>=0);
    char buffer[4096];ssize_t count;
    while((count=read(from,buffer,sizeof(buffer)))>0)assert(write(to,buffer,(size_t)count)==count);
    assert(!count && !close(from) && !close(to));
}
int main(int argc,char **argv) {
    assert(argc==3);char mount[512],font[512],old_name[1024],nested[512],nested_font[512];
    assert(snprintf(mount,sizeof(mount),"%s/card",argv[1])<(int)sizeof(mount) && !mkdir(mount,0700));
    assert(snprintf(font,sizeof(font),"%s/font.ttf",mount)<(int)sizeof(font));copy_file(argv[2],font);
    uint64_t epoch=mount_volume(mount,"font-card");unsigned baseline=allocations;
    pxa_esp_font_source_t *source=NULL;
    assert(pxa_esp_font_source_acquire("relative.ttf",&source)==PXA_STATUS_INVALID_ARGUMENT && !source);
    allocation_failures=2;
    assert(pxa_esp_font_source_acquire(font,&source)==PXA_STATUS_RESOURCE_LIMIT && !source && allocations==baseline);
    assert(!pxa_esp_font_source_acquire(font,&source));
    strcpy(old_name,pxa_esp_font_source_path(source));assert(!strncmp(old_name,"pxa-font:",9));
    FT_Library library;FT_Face face=NULL;assert(!FT_Init_FreeType(&library));
    assert(!FT_New_Face(library,font,0,&face) && !FT_Done_Face(face));face=NULL;
    /* This uses the real FreeType parser and linker interposition, not a
     * mocked font factory or a direct call to the wrapper. */
    assert(!FT_New_Face(library,old_name,0,&face));
    assert(allocations==baseline+3 && !FT_Set_Pixel_Sizes(face,0,16));
    int reference=open(font,O_RDONLY);assert(reference>=0);
    const unsigned long offsets[]={0,4089,4096,8190,16391,face->stream->size-7,face->stream->size};
    for(size_t i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i) {
        uint8_t actual[64],expected[64];
        unsigned long bytes=face->stream->size-offsets[i];if(bytes>sizeof(actual))bytes=sizeof(actual);
        assert(face->stream->read(face->stream,offsets[i],actual,sizeof(actual))==bytes);
        assert(pread(reference,expected,bytes,(off_t)offsets[i])==(ssize_t)bytes && !memcmp(actual,expected,bytes));
    }
    read_calls=0;
    for(unsigned i=0;i<20;++i) {
        uint8_t actual[16],expected[16];unsigned long at=i%2?12317:8211;
        assert(face->stream->read(face->stream,at,actual,sizeof(actual))==sizeof(actual));
        assert(pread(reference,expected,sizeof(expected),(off_t)at)==sizeof(expected));
        assert(!memcmp(actual,expected,sizeof(actual)));
    }
    assert(read_calls<=2 && !close(reference));
    FT_UInt uncached=FT_Get_Char_Index(face,0x6c34);assert(uncached);
    assert(!FT_Load_Char(face,0x4e2d,FT_LOAD_RENDER) && face->glyph->bitmap.rows);
    assert(!pxa_esp_storage_drain_volume(epoch));
    assert(pxa_esp_storage_unregister_volume(epoch)==PXA_STATUS_BUSY);
    uint8_t buffer[16];assert(!face->stream->read(face->stream,0,buffer,sizeof(buffer)));
    assert(face->stream->read(face->stream,0,NULL,0)==1);
    assert(FT_Load_Glyph(face,uncached,FT_LOAD_RENDER)!=0);
    assert(!pxa_esp_font_source_release(&source) && !source);
    assert(pxa_esp_storage_unregister_volume(epoch)==PXA_STATUS_BUSY); /* Stream still pins its mount. */
    assert(!FT_Done_Face(face));face=NULL;
    assert(!pxa_esp_storage_unregister_volume(epoch));
    uint64_t next=mount_volume(mount,"font-card");assert(next!=epoch);
    assert(FT_New_Face(library,old_name,0,&face)!=0 && !face);
    assert(!pxa_esp_font_source_acquire(font,&source));
    assert(!FT_New_Face(library,pxa_esp_font_source_path(source),0,&face));
    assert(!FT_Set_Pixel_Sizes(face,0,20) && !FT_Load_Char(face,0x6c34,FT_LOAD_RENDER));
    assert(!FT_Done_Face(face) && !pxa_esp_font_source_release(&source));
    /* Caching is optional under memory pressure; parsing and glyph rendering
     * must still work through the original epoch-bound file stream. */
    assert(!pxa_esp_font_source_acquire(font,&source));
    reject_page_cache=1;
    assert(!FT_New_Face(library,pxa_esp_font_source_path(source),0,&face));
    reject_page_cache=0;
    assert(!FT_Set_Pixel_Sizes(face,0,20) && !FT_Load_Char(face,0x4e2d,FT_LOAD_RENDER));
    assert(!FT_Done_Face(face) && !pxa_esp_font_source_release(&source));
    /* Terminal links/special files are rejected without blocking or leaving
     * a physical-volume reference after FreeType reports an open failure. */
    char link[512];assert(snprintf(link,sizeof(link),"%s/link.ttf",mount)<(int)sizeof(link));
    assert(!symlink(font,link) && !pxa_esp_font_source_acquire(link,&source));
    assert(FT_New_Face(library,pxa_esp_font_source_path(source),0,&face)!=0);
    assert(!pxa_esp_font_source_release(&source) && !unlink(link));
    assert(!mkfifo(link,0600) && !pxa_esp_font_source_acquire(link,&source));
    assert(FT_New_Face(library,pxa_esp_font_source_path(source),0,&face)!=0);
    assert(!pxa_esp_font_source_release(&source) && !unlink(link));
    /* A new child mount cannot redirect a previously approved parent name. */
    assert(snprintf(nested,sizeof(nested),"%s/child",mount)<(int)sizeof(nested) && !mkdir(nested,0700));
    assert(snprintf(nested_font,sizeof(nested_font),"%s/font.ttf",nested)<(int)sizeof(nested_font));copy_file(argv[2],nested_font);
    assert(!pxa_esp_font_source_acquire(nested_font,&source));strcpy(old_name,pxa_esp_font_source_path(source));
    uint64_t child=mount_volume(nested,"child-card");
    assert(FT_New_Face(library,old_name,0,&face)!=0);
    assert(!pxa_esp_font_source_release(&source));
    assert(!pxa_esp_font_source_acquire(nested_font,&source));
    assert(!FT_New_Face(library,pxa_esp_font_source_path(source),0,&face));
    assert(!FT_Done_Face(face) && !pxa_esp_font_source_release(&source));
    assert(!pxa_esp_storage_drain_volume(child));
    assert(pxa_esp_font_source_acquire(nested_font,&source)==PXA_STATUS_UNAVAILABLE && !source);
    assert(!pxa_esp_storage_unregister_volume(child));
    assert(!pxa_esp_storage_drain_volume(next) && !pxa_esp_storage_unregister_volume(next));
    assert(!FT_Done_FreeType(library));
    assert(allocations==baseline-1); /* Only the registry workspace remains. */
    puts("Epoch-bound fonts: real FreeType CJK, owner/stream pins, drain errors, remount and child-mount isolation, allocation failure and link/FIFO refusal passed");
    return 0;
}
