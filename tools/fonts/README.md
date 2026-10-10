# System font generation and checks

The default asset keeps its legacy `noto_sans_cjk_common.ttf` pathname.
Its internal family is PXA Sans SC, using mainland Chinese sans-serif
outlines. It preserves the previous font's character mappings and outlines,
adds Big5 level-one Chinese characters and the acceptance corpus, and keeps
an outlined `.notdef` for characters outside the subset. It is not a complete
Unicode font. Coverage is generated offline; there is no additional runtime
fallback face, atlas, or whole-book buffer.

Use an OFL-licensed Noto Sans CJK Regular TTC. **SC is face 2** in the upstream
collection; face 0 is Japanese. The generator checks the selected face name.
The output is a TrueType outline font rather than a CFF font; the latter's
face-loading overhead was substantially higher. Original glyphs are copied,
only exact outline/metric duplicates are shared, and a compact cmap reduces
font metadata. Runtime rendering already disables auto-hinting, and the
native probe must use the same flags when measuring memory or comparing
glyphs. Keep the license and NOTICE beside the distributed font.

From the workspace root, with Python fontTools and Pillow installed:

```sh
# Extract the previous base asset from the repository commit that contained it.
git show 5e511db02ed7eb578ae64a40b4b17c93d1332b4b:factory/base/system/fonts/noto_sans_cjk_common.ttf > /tmp/pxa-font-base.ttf
python3 tools/fonts/build_common_font.py \
  --base /tmp/pxa-font-base.ttf \
  --source /usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc --face 2 \
  --corpus local/reader-typography-20261010/xiyou.txt \
  --output /tmp/pxa-system-font.ttf --report /tmp/pxa-font-build.json
python3 -m unittest discover -s tools/fonts -p 'test_*.py'
python3 local/pxa-apps/novel-reader/tests/font_coverage_test.py \
  --font /tmp/pxa-system-font.ttf --book local/reader-typography-20261010/xiyou.txt
cc tools/fonts/ft_font_probe.c -I/usr/include/freetype2 -lfreetype \
  -Wall -Wextra -Werror -o /tmp/pxa-font-probe
/tmp/pxa-font-probe /tmp/pxa-system-font.ttf local/reader-typography-20261010/xiyou.txt
# A UTF-8 file containing every original mapped character verifies their bitmaps:
/tmp/pxa-font-probe compare /tmp/pxa-font-base.ttf /tmp/pxa-system-font.ttf /tmp/pxa-original-characters.txt
```

`--traditional all` is an optional larger subset, not the default asset.
Additional corpora can be supplied with repeated `--corpus`. Recheck Flash
space, actual load/paint results and memory before deploying another subset.
The native probe reports requested FreeType allocation bytes in a new process;
it excludes allocator headers, LVGL glyph caches, display buffers and device
SRAM/PSRAM. These results are not a substitute for whole-device measurements.

The final build/coverage/bitmap/memory records and five-screen screenshots are
in the reader's `docs/2026-10-11-font-coverage.zh-CN.md` report. Font replacement
must be atomic, and the Host must restart to release old faces. A running
FreeType face keeps the old file open; updating only the reader package does
not update the system font. Never erase application data to install a font.
