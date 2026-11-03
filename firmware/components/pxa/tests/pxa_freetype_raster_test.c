/* SPDX-License-Identifier: MIT */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t hash = UINT64_C(14695981039346656037);
static void add(uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        hash = (hash ^ (value & 255)) * UINT64_C(1099511628211);
        value >>= 8;
    }
}
int main(int argc, char **argv) {
    static const unsigned sizes[] = {8, 10, 12, 16, 24, 32, 48, 72, 96, 128};
    static const unsigned cjk[] = {0x4e2d, 0x6587, 0x5b57, 0x4f53, 0x6d4b,
        0x8bd5, 0x9f8d, 0x9f98, 0x7e41, 0x9ad4, 0x56fd, 0x9ad8, 0x597d,
        0x5bc6, 0x5ea6, 0x3002, 0xff0c};
    FT_Library library;
    FT_Face face;
    unsigned count = 0;
    if (argc != 2 || FT_Init_FreeType(&library) ||
        FT_New_Face(library, argv[1], 0, &face)) return 1;
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(*sizes); ++s) {
        if (FT_Set_Pixel_Sizes(face, 0, sizes[s])) return 2;
        for (unsigned i = 0; i < 95 + sizeof(cjk) / sizeof(*cjk); ++i) {
            unsigned cp = i < 95 ? i + 32 : cjk[i - 95];
            if (!FT_Get_Char_Index(face, cp)) continue;
            if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) return 3;
            const FT_GlyphSlot g = face->glyph;
            const FT_Bitmap *b = &g->bitmap;
            if (b->pixel_mode != FT_PIXEL_MODE_GRAY || b->pitch < 0) return 4;
            add(cp); add(sizes[s]); add(g->advance.x); add(g->advance.y);
            add(g->bitmap_left); add(g->bitmap_top); add(b->width); add(b->rows);
            for (unsigned y = 0; y < b->rows; ++y)
                for (unsigned x = 0; x < b->width; ++x) add(b->buffer[y * b->pitch + x]);
            ++count;
        }
    }
    FT_Done_Face(face); FT_Done_FreeType(library);
    printf("glyphs=%u coverage=%016" PRIx64 "\n", count, hash);
    return count >= 1000 ? 0 : 5;
}
