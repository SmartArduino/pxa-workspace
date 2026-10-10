/* Native font regression: actual LVGL load flags, ink, and FreeType allocation.
 * Build with the platform FreeType headers and library. This measures requested
 * FreeType heap bytes, not LVGL caches, libc overhead, or a device's whole heap.
 */
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYSTEM_H
#include FT_MODULE_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t live_bytes, peak_bytes;
typedef union { max_align_t align; size_t size; } Allocation;
static void *allocate(FT_Memory memory, long size) {
    (void)memory;
    if(size < 0 || (size_t)size > SIZE_MAX-sizeof(Allocation)) return NULL;
    Allocation *p=malloc(sizeof(*p)+(size_t)size);
    if(!p) return NULL;
    p->size=(size_t)size; live_bytes+=p->size;
    if(live_bytes>peak_bytes) peak_bytes=live_bytes;
    return p+1;
}
static void release(FT_Memory memory, void *ptr) {
    (void)memory;
    if(ptr) { Allocation *p=(Allocation *)ptr-1; live_bytes-=p->size; free(p); }
}
static void *resize(FT_Memory memory,long old_size,long size,void *ptr) {
    (void)old_size;
    void *next=allocate(memory,size);
    if(next) {
        if(ptr) { size_t old=((Allocation *)ptr-1)->size;
            memcpy(next,ptr,old<(size_t)size?old:(size_t)size); }
        release(memory,ptr);
    }
    return next;
}
static uint32_t read_codepoint(FILE *file) {
    int ch=fgetc(file);
    if(ch==EOF) return UINT32_MAX;
    uint32_t cp=(uint32_t)ch;
    int following=0;
    if((ch&0xe0)==0xc0) { cp=ch&31; following=1; }
    else if((ch&0xf0)==0xe0) { cp=ch&15; following=2; }
    else if((ch&0xf8)==0xf0) { cp=ch&7; following=3; }
    while(following--) {
        ch=fgetc(file);
        if(ch==EOF || (ch&0xc0)!=0x80) { fprintf(stderr,"Invalid UTF-8\n"); exit(6); }
        cp=(cp<<6)|(ch&63);
    }
    return cp;
}
static int whitespace(uint32_t cp) { return cp<=32 || cp==0x3000 || cp==0xfeff; }
static FT_Error render(FT_Face face,FT_UInt gid) {
    return FT_Load_Glyph(face,gid,
        FT_LOAD_COLOR|FT_LOAD_RENDER|FT_LOAD_TARGET_NORMAL|FT_LOAD_NO_AUTOHINT);
}
static int same_bitmap(FT_GlyphSlot a,FT_GlyphSlot b) {
    if(a->advance.x!=b->advance.x || a->bitmap_left!=b->bitmap_left ||
       a->bitmap_top!=b->bitmap_top || a->bitmap.rows!=b->bitmap.rows ||
       a->bitmap.width!=b->bitmap.width || a->bitmap.pixel_mode!=b->bitmap.pixel_mode)
        return 0;
    for(unsigned y=0;y<a->bitmap.rows;++y)
        if(memcmp(a->bitmap.buffer+y*a->bitmap.pitch,
                  b->bitmap.buffer+y*b->bitmap.pitch,a->bitmap.width)) return 0;
    return 1;
}
int main(int argc,char **argv) {
    int compare=argc==5 && strcmp(argv[1],"compare")==0;
    if(!compare && argc!=3) {
        fprintf(stderr,"Usage: probe FONT UTF8_TEXT | probe compare BEFORE AFTER UTF8_TEXT\n");
        return 2;
    }
    struct FT_MemoryRec_ memory={NULL,allocate,release,resize};
    FT_Library library; FT_Face face,other=NULL;
    if(FT_New_Library(&memory,&library)) return 3;
    FT_Add_Default_Modules(library);
    if(FT_New_Face(library,argv[compare?2:1],0,&face)) return 4;
    if(compare && FT_New_Face(library,argv[3],0,&other)) return 4;
    size_t opened=live_bytes, opened_peak=peak_bytes;
    FILE *text=fopen(argv[compare?4:2],"rb");
    if(!text) return 5;
    unsigned count=0,missing=0,blank=0,mismatch=0;
    const unsigned sizes[]={12,16,28,56};
    uint64_t checksum=0;
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        if(FT_Set_Pixel_Sizes(face,0,sizes[i]) ||
           (compare && FT_Set_Pixel_Sizes(other,0,sizes[i]))) return 7;
        rewind(text); uint32_t cp;
        while((cp=read_codepoint(text))!=UINT32_MAX) {
            if(whitespace(cp)) continue;
            FT_UInt gid=FT_Get_Char_Index(face,cp);
            ++count; if(!gid) ++missing;
            if(render(face,gid)) return 7;
            uint64_t ink=0;
            for(unsigned y=0;y<face->glyph->bitmap.rows;++y)
                for(unsigned x=0;x<face->glyph->bitmap.width;++x)
                    ink+=face->glyph->bitmap.buffer[y*face->glyph->bitmap.pitch+x];
            if(!ink) ++blank;
            checksum+=ink;
            if(compare) {
                FT_UInt replacement=FT_Get_Char_Index(other,cp);
                if(render(other,replacement)) return 7;
                if(!replacement || !same_bitmap(face->glyph,other->glyph)) {
                    if(mismatch++<8) fprintf(stderr,"Changed glyph U+%04X size=%u\n",cp,sizes[i]);
                }
            }
        }
    }
    fclose(text);
    printf("{\"open_live\":%zu,\"open_peak\":%zu,\"render_live\":%zu,"
           "\"render_peak\":%zu,\"glyph_count\":%u,\"missing\":%u,"
           "\"blank\":%u,\"bitmap_mismatch\":%u,\"checksum\":%llu",
           opened,opened_peak,live_bytes,peak_bytes,count,missing,blank,mismatch,
           (unsigned long long)checksum);
    if(other) FT_Done_Face(other);
    FT_Done_Face(face); FT_Done_Library(library);
    printf(",\"after_close\":%zu}\n",live_bytes);
    return live_bytes || mismatch;
}
