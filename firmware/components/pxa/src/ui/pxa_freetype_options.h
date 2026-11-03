/* SPDX-License-Identifier: MIT */
#ifndef PXA_FREETYPE_OPTIONS_H
#define PXA_FREETYPE_OPTIONS_H

#include <freetype/config/ftoption.h>

/* FreeType's gray rasterizer allocates this pool on its caller's stack.
 * Font-glyph requests run on the 12 KiB runtime stack. Its default 16 KiB
 * pool exceeds that entire stack; use FreeType's minimum supported pool.
 * The rasterizer subdivides vertical bands when the pool fills, preserving
 * coverage and reentrant rendering without changing task stack budgets. */
#undef FT_RENDER_POOL_SIZE
#define FT_RENDER_POOL_SIZE 2048L

#endif
