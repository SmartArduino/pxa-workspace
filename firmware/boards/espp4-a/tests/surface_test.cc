// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_surface.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    // Opaque redraws must replace old guest pixels after a move or resize,
    // while clearing only the backdrop. Compare against a full clean buffer.
    for (const auto& [w,h] : {std::pair{7,11},std::pair{480,854},std::pair{854,480}}) {
        for (const auto& [sw,sh] : {std::pair{1,1},std::pair{3,5},std::pair{240,426},std::pair{320,240},std::pair{480,854}}) {
            std::vector<uint16_t> source(size_t(sw)*sh);
            for (size_t i=0;i<source.size();++i) source[i]=uint16_t(i*97+3);
            for (const auto& [x,y] : {std::pair{0,0},std::pair{2,3},std::pair{-2,-4},std::pair{w+1,h+1}}) {
                std::vector<uint16_t> actual(size_t(w)*h+2,0x1357), expected(size_t(w)*h,0x2468);
                pxa_esp_surface_frame_t f{};f.visible=1;f.pixels=reinterpret_cast<const uint8_t*>(source.data());
                f.width=sw;f.height=sh;f.stride_bytes=sw*2;f.format=PXA_SURFACE_FORMAT_RGB565;f.x=x;f.y=y;
                assert(espp4_a::FillOpaqueBackdrop(f,actual.data()+1,w,h,0x2468));
                // Leave the new guest area unfilled; this near-full frame has
                // only two uncovered scanlines on the real portrait panel.
                if(w==480 && h==854 && sw==240 && sh==426 && !x && !y) {
                    size_t clears=0;for(size_t i=1;i+1<actual.size();++i) clears+=actual[i]==0x2468;
                    assert(clears==960);
                }
                espp4_a::Compose(f,actual.data()+1,0,0,w,h,w,w,h);
                espp4_a::Compose(f,expected.data(),0,0,w,h,w,w,h);
                assert(actual.front()==0x1357 && actual.back()==0x1357);
                assert(std::equal(expected.begin(),expected.end(),actual.begin()+1));
            }
        }
    }

    // A letterboxed direct frame may use a fill only after the entire painted
    // background, including its final pixel, has been proven uniform.
    for (size_t count : {size_t(1),size_t(2),size_t(3),size_t(32),size_t(513)}) {
        std::vector<uint16_t> background(count + 2, 0x2468);
        uint16_t color = 0x9999;
        assert(espp4_a::UniformBackground(background.data() + 1, count, color));
        assert(color == 0x2468);
        if (count == 1) continue; // Every single-pixel background is uniform.
        for (size_t mismatch : {size_t(0), count / 2, count - 1}) {
            background[mismatch + 1] ^= 1; color = 0x9999;
            assert(!espp4_a::UniformBackground(background.data() + 1, count, color));
            assert(color == 0x9999);
            background[mismatch + 1] ^= 1;
        }
    }
    {
        uint16_t color = 0x9999;
        assert(!espp4_a::UniformBackground(nullptr, 1, color));
        assert(!espp4_a::UniformBackground(&color, 0, color));
        assert(color == 0x9999);
        constexpr unsigned sw = 320, sh = 240, dw = 480, dh = 854;
        std::vector<uint16_t> source(sw * sh), output(dw * dh + 2, 0x2468);
        for (size_t i = 0; i < source.size(); ++i) source[i] = uint16_t(i * 101u);
        pxa_esp_surface_frame_t frame{};
        frame.visible=1;frame.pixels=reinterpret_cast<const uint8_t*>(source.data());
        frame.width=sw;frame.height=sh;frame.stride_bytes=sw*2;frame.format=PXA_SURFACE_FORMAT_RGB565;
        assert(espp4_a::IsOpaqueSurface(frame));
        assert(!espp4_a::CoversDisplayOpaque(frame,dw,dh));
        espp4_a::Compose(frame,output.data()+1,0,0,dw,dh,dw,dw,dh);
        for (unsigned y=0;y<dh;++y) for (unsigned x=0;x<dw;++x) {
            const bool inside=x>=(dw-sw)/2 && x<(dw+sw)/2 && y>=(dh-sh)/2 && y<(dh+sh)/2;
            assert(output[1+y*dw+x]==(inside?source[(y-(dh-sh)/2)*sw+x-(dw-sw)/2]:0x2468));
        }
        assert(output.front()==0x2468 && output.back()==0x2468);
        frame.system_alpha_plane.visible=1;assert(!espp4_a::IsOpaqueSurface(frame));
        assert(!espp4_a::FillOpaqueBackdrop(frame,output.data()+1,dw,dh,0));
        frame.system_alpha_plane.visible=0;frame.ui_alpha_plane.visible=1;
        assert(!espp4_a::IsOpaqueSurface(frame));
        frame.ui_alpha_plane.visible=0;frame.opaque_ui_region_count=1;
        assert(!espp4_a::IsOpaqueSurface(frame));
    }
    // Odd source widths/strides alternate 32-bit alignment. Compare the full
    // panel against nearest-neighbor math, including unaligned fallback and
    // sentinel pixels surrounding both source and destination buffers.
    for (unsigned source_offset : {0u,1u}) for (unsigned output_offset : {0u,1u}) {
        constexpr unsigned sw=427,sh=240,pitch=429,dw=sw*2,dh=sh*2;
        std::vector<uint16_t> source(pitch*sh+2),output(dw*dh+2,0x5abc);
        for(size_t i=0;i<source.size();++i)source[i]=uint16_t(i*1009u^(i>>5));
        pxa_esp_surface_frame_t f{};
        f.visible=1;f.pixels=reinterpret_cast<const uint8_t*>(source.data()+source_offset);
        f.width=sw;f.height=sh;f.stride_bytes=pitch*2;f.format=PXA_SURFACE_FORMAT_RGB565;
        espp4_a::Compose(f,output.data()+output_offset,0,0,dw,dh,dw,dw,dh);
        for(unsigned y=0;y<dh;++y)for(unsigned x=0;x<dw;++x)
            assert(output[output_offset+y*dw+x]==source[source_offset+(y/2)*pitch+x/2]);
        if(output_offset)assert(output.front()==0x5abc);
        assert(output.back()==0x5abc);
    }
    // Skipping LVGL paint is safe only when opaque guest pixels replace
    // every pixel, with no trusted chrome, alpha or unfilled edge to retain.
    {
        uint16_t pixel = 0;
        pxa_esp_surface_frame_t f{};
        f.visible=1;f.pixels=reinterpret_cast<const uint8_t*>(&pixel);
        f.width=427;f.height=240;f.stride_bytes=856;f.format=PXA_SURFACE_FORMAT_RGB565;
        assert(espp4_a::CoversDisplayOpaque(f,854,480));
        assert(!espp4_a::CoversDisplayOpaque(f,480,854));
        f.ui_alpha_plane.visible=1;assert(!espp4_a::CoversDisplayOpaque(f,854,480));
        f.suppress_guest_alpha=1;assert(espp4_a::CoversDisplayOpaque(f,854,480));
        f.system_alpha_plane.visible=1;assert(!espp4_a::CoversDisplayOpaque(f,854,480));
        f.system_alpha_plane.visible=0;f.opaque_ui_region_count=1;
        assert(!espp4_a::CoversDisplayOpaque(f,854,480));
        f.opaque_ui_region_count=0;f.width=426;
        assert(!espp4_a::CoversDisplayOpaque(f,854,480)); // Two missing columns.
        f.width=427;f.x=1;assert(!espp4_a::CoversDisplayOpaque(f,854,480));
        f.x=0;f.format=PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
        assert(!espp4_a::CoversDisplayOpaque(f,854,480));
        f.format=PXA_SURFACE_FORMAT_RGB565;f.stride_bytes=853;
        assert(!espp4_a::CoversDisplayOpaque(f,854,480));
        f.width=854;f.height=480;f.stride_bytes=1712;
        assert(espp4_a::CoversDisplayOpaque(f,854,480));
        f.visible=0;assert(!espp4_a::CoversDisplayOpaque(f,854,480));
    }
    // Independent scalar reference: overlapping/clipped masks must preserve
    // native UI rows even when the neighboring guest row can be replicated.
    for (int scale : {1,2,4}) for (int offset : {0,1}) {
        std::array<uint16_t,9*7> source;
        for (size_t i=0;i<source.size();++i) source[i]=uint16_t(i*1009u);
        pxa_esp_surface_frame_t frame{};
        frame.visible=1; frame.width=7; frame.height=5; frame.stride_bytes=18;
        frame.pixels=reinterpret_cast<const uint8_t*>(source.data());
        frame.format=PXA_SURFACE_FORMAT_RGB565; frame.x=2; frame.y=3;
        frame.opaque_ui_region_count=4;
        frame.opaque_ui_regions[0]={2,1,4,3};
        frame.opaque_ui_regions[1]={0,2,7,1};
        frame.opaque_ui_regions[2]={1,0,3,2};
        frame.opaque_ui_regions[3]={6,3,9,7};
        const int w=7*scale+4,h=5*scale+6,pitch=w+3;
        std::vector<uint16_t> actual(size_t(pitch)*h+2),expected(actual.size());
        for (size_t i=0;i<actual.size();++i) actual[i]=expected[i]=uint16_t(i*37u);
        const uint16_t overlay=0xffff; const uint8_t alpha=173;
        frame.system_alpha_plane={&overlay,&alpha,2,1,3,4,1,1,219,1,0};
        espp4_a::Compose(frame,actual.data()+offset,0,0,w,h,pitch,w,h);
        for (int y=0;y<h;++y) for(int x=0;x<w;++x) {
            auto& out=expected[offset+y*pitch+x];
            if(x>=2 && y>=3 && x<2+7*scale && y<3+5*scale) {
                const int sx=(x-2)/scale,sy=(y-3)/scale;
                bool masked=false;
                for(unsigned i=0;i<frame.opaque_ui_region_count;++i) {
                    const auto& r=frame.opaque_ui_regions[i];
                    masked|=sx>=r.x && sy>=r.y && sx<r.x+r.width && sy<r.y+r.height;
                }
                if(!masked)out=source[sy*9+sx];
            }
            if(x==3 && y==4)out=pxa_esp_surface_blend_alpha_pixel(out,overlay,alpha,219);
        }
        assert(actual==expected);
        frame.opaque_ui_region_count=PXA_SURFACE_MAX_OPAQUE_UI_REGIONS+1;
        auto guard=actual;
        espp4_a::Compose(frame,actual.data()+offset,0,0,w,h,pitch,w,h);
        assert(actual==guard);
    }
    // Padded source rows and a tile crossing the integer-scale boundary.
    const std::array<uint16_t, 8> source{0xf800, 0x07e0, 0xdead, 0xbeef,
                                      0x001f, 0xffff, 0xdead, 0xbeef};
    pxa_esp_surface_frame_t frame{};
    frame.visible = 1; frame.pixels = reinterpret_cast<const uint8_t*>(source.data());
    frame.width = 2; frame.height = 2; frame.stride_bytes = 8;
    frame.format = PXA_SURFACE_FORMAT_RGB565; frame.x = 10; frame.y = 20;
    std::array<uint16_t, 12> tile;
    tile.fill(0x1234);
    espp4_a::Compose(frame, tile.data(), 13, 23, 2, 2, 6);
    assert(tile[0] == 0xf800 && tile[1] == 0x07e0);
    assert(tile[6] == 0x001f && tile[7] == 0xffff);
    assert(tile[2] == 0x1234 && tile[8] == 0x1234);
    frame.opaque_ui_region_count = 1;
    frame.opaque_ui_regions[0] = {0, 0, 1, 1};
    tile.fill(0x1234);
    espp4_a::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == 0x1234);
    frame.opaque_ui_region_count = 0;
    const uint16_t guest = 0, system = 0xffff;
    const uint8_t opaque = 255;
    frame.ui_alpha_plane = {&guest, &opaque, 2, 1, 10, 20, 1, 1, 255, 1, 0};
    frame.system_alpha_plane = {&system, &opaque, 2, 1, 10, 20, 1, 1, 255, 1, 0};
    espp4_a::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == system);
    frame.system_alpha_plane.visible = 0;
    espp4_a::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == guest);
    frame.suppress_guest_alpha = 1;
    espp4_a::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == source[0]);
    const uint32_t argb = 0x80808080;
    frame.ui_alpha_plane.visible = 0;
    frame.pixels = reinterpret_cast<const uint8_t*>(&argb);
    frame.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
    frame.width = frame.height = 1; frame.stride_bytes = 4;
    tile.fill(0);
    espp4_a::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == 0x8410); // half white over black, RGB565
    frame.stride_bytes = 3; tile.fill(0x1234);
    espp4_a::Compose(frame, tile.data(), 10, 20, 1, 1, 6);
    assert(tile[0] == 0x1234);
    // Unscaled RGB565 row path: centered viewport, padded source/destination,
    // clipping outside the guest and trusted alpha composition.
    std::vector<uint16_t> rows(243 * 428, 0xdead);
    for (unsigned y = 0; y < 428; ++y)
        for (unsigned x = 0; x < 241; ++x) rows[y * 243 + x] = uint16_t(y * 241 + x);
    frame = {};
    frame.visible = 1; frame.pixels = reinterpret_cast<const uint8_t*>(rows.data());
    frame.width = 241; frame.height = 428; frame.stride_bytes = 486;
    frame.format = PXA_SURFACE_FORMAT_RGB565;
    tile.fill(0x1234);
    espp4_a::Compose(frame, tile.data(), 118, 214, 4, 2, 6);
    assert(tile[0] == 0x1234 && tile[1] == 241 && tile[3] == 243);
    assert(tile[6] == 0x1234 && tile[7] == 482 && tile[9] == 484);
    assert(tile[4] == 0x1234 && tile[10] == 0x1234);
    frame.system_alpha_plane = {&system, &opaque, 2, 1, 119, 214, 1, 1, 255, 1, 0};
    espp4_a::Compose(frame, tile.data(), 118, 214, 4, 2, 6);
    assert(tile[1] == system && tile[2] == 242);
    frame.system_alpha_plane.visible = 0;
    frame.opaque_ui_region_count = 1;
    frame.opaque_ui_regions[0] = {0, 1, 1, 1};
    tile.fill(0x1234);
    espp4_a::Compose(frame, tile.data(), 118, 214, 4, 2, 6);
    assert(tile[1] == 0x1234 && tile[2] == 242); // Preserve trusted opaque UI.
    // Full portrait 2x expansion, including vertically repeated rows and a
    // trusted plane covering only the second physical row of a guest pixel.
    std::vector<uint16_t> scaled_source(242 * 427, 0xdead), expanded(482 * 854, 0x1234);
    for (unsigned y = 0; y < 427; ++y)
        for (unsigned x = 0; x < 240; ++x) scaled_source[y * 242 + x] = uint16_t(y * 240 + x);
    frame = {}; frame.visible = 1;
    frame.pixels = reinterpret_cast<const uint8_t*>(scaled_source.data());
    frame.width = 240; frame.height = 427; frame.stride_bytes = 484;
    frame.format = PXA_SURFACE_FORMAT_RGB565;
    frame.system_alpha_plane = {&system, &opaque, 2, 1, 0, 1, 1, 1, 255, 1, 0};
    espp4_a::Compose(frame, expanded.data(), 0, 0, 480, 854, 482);
    for (unsigned y = 0; y < 854; ++y) {
        for (unsigned x = 0; x < 480; ++x)
            assert(expanded[y * 482 + x] ==
                (y == 1 && x == 0 ? system : uint16_t(y / 2 * 240 + x / 2)));
        assert(expanded[y * 482 + 480] == 0x1234 && expanded[y * 482 + 481] == 0x1234);
    }
    // Cropped doubled rows exercise both aligned pair stores and the scalar
    // fallback, including odd pitches and guard pixels outside each tile.
    frame.system_alpha_plane = {};
    for (int left : {0,1,2,3,237,477}) for(int tile_width : {1,2,3,17,31}) {
        std::vector<uint16_t> output((tile_width+3)*5+2,0x3abc);
        espp4_a::Compose(frame,output.data()+1,left,3,tile_width,5,tile_width+3);
        for(int y=0;y<5;++y) for(int x=0;x<tile_width+3;++x) {
            const auto expected=x<tile_width && left+x<480
                ? uint16_t((y+3)/2*240+(left+x)/2) : uint16_t(0x3abc);
            assert(output[1+y*(tile_width+3)+x]==expected);
        }
        assert(output.front()==0x3abc&&output.back()==0x3abc);
    }
    // Unscaled ARGB spans: overlapping/out-of-order trusted masks, clipping,
    // mixed premultiplied alpha and padded rows. Compare individual pixels to
    // the composition contract, including planes outside the guest viewport.
    std::vector<uint32_t> colors(248 * 428);
    uint32_t random = 0x12345678;
    auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    for (auto& pixel : colors) {
        unsigned a = next() >> 24;
        pixel = (a << 24) | (((next() >> 24) * a / 255) << 16) |
                (((next() >> 24) * a / 255) << 8) | ((next() >> 24) * a / 255);
    }
    for (unsigned attempt = 0; attempt < 250; ++attempt) {
        frame = {}; frame.visible = 1; frame.x = 10; frame.y = 20;
        frame.width = 241; frame.height = 428; frame.stride_bytes = 248 * 4;
        frame.pixels = reinterpret_cast<const uint8_t*>(colors.data());
        frame.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
        frame.opaque_ui_region_count = attempt % 9;
        for (unsigned i = 0; i < frame.opaque_ui_region_count; ++i)
            frame.opaque_ui_regions[i] = {uint16_t(next() % 241), uint16_t(next() % 428), 67, 31};
        const int tx = next() % 270, ty = next() % 450;
        std::array<uint16_t, 37 * 13> actual, expected;
        for (size_t i = 0; i < actual.size(); ++i) actual[i] = expected[i] = uint16_t(next());
        std::array<uint16_t, 7 * 5> overlay;
        std::array<uint8_t, 7 * 5> alpha;
        for (size_t i = 0; i < overlay.size(); ++i) { overlay[i] = uint16_t(next()); alpha[i] = next() >> 24; }
        frame.system_alpha_plane = {overlay.data(), alpha.data(), 14, 7, tx+2, ty+3, 7, 5, 197, 1, 0};
        espp4_a::Compose(frame, actual.data(), tx, ty, 33, 13, 37);
        for (int y = 0; y < 13; ++y) for (int x = 0; x < 33; ++x) {
            auto& bg = expected[y * 37 + x];
            const int sx = tx+x-10, sy = ty+y-20;
            bool blocked = false;
            for (unsigned i = 0; i < frame.opaque_ui_region_count; ++i) {
                const auto& r = frame.opaque_ui_regions[i];
                blocked |= sx>=r.x && sy>=r.y && sx<r.x+r.width && sy<r.y+r.height;
            }
            if (!blocked && sx>=0 && sx<241 && sy>=0 && sy<428) {
                const auto c = colors[sy * 248 + sx]; const unsigned a = c >> 24;
                const uint16_t f = ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 31);
                if (a == 255) bg = f;
                else if (a) bg = uint16_t(((f>>11) + (((bg>>11)*(255-a)+128)>>8)) << 11) |
                    uint16_t((((f>>5)&63) + ((((bg>>5)&63)*(255-a)+128)>>8)) << 5) |
                    uint16_t((f&31) + (((bg&31)*(255-a)+128)>>8));
            }
            if (x>=2 && x<9 && y>=3 && y<8) {
                const size_t at = (y-3)*7 + x-2;
                bg = pxa_esp_surface_blend_alpha_pixel(bg, overlay[at], alpha[at], 197);
            }
        }
        assert(actual == expected);
    }
    puts("ESPP4-A: Surface stride, scaling, premultiplied ARGB spans and trusted overlays passed");
}
