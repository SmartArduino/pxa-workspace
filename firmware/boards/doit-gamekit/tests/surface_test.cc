#include "doit_gamekit_surface.h"
#include <array>
#include <cassert>
#include <vector>
#include <cstring>

using namespace doit_gamekit;
static void CheckDirectEligibility() {
    std::array<uint16_t, kWidth * kHeight> pixels{};
    for (const int scale : {1, 2, 4}) {
        pxa_esp_surface_frame_t frame{};
        frame.pixels = reinterpret_cast<const uint8_t*>(pixels.data());
        frame.width = kWidth / scale; frame.height = kHeight / scale;
        frame.stride_bytes = frame.width * 2;
        frame.visible = 1; frame.format = PXA_SURFACE_FORMAT_RGB565;
        frame.flags = PXA_SURFACE_FLAG_PREFER_DIRECT_SCANOUT;
        assert(CanPresentDirectly(frame, false, false));
        assert(!CanPresentDirectly(frame, true, false)); // trusted modal
        assert(!CanPresentDirectly(frame, false, true)); // virtual mouse
        frame.stride_bytes += 4;
        assert(CanPresentDirectly(frame, false, false)); // padded rows supported
        auto rejected = frame;
        rejected.stride_bytes = frame.width * 2 - 1;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; ++rejected.stride_bytes;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; ++rejected.pixels;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; rejected.flags = 0;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; rejected.x = 1;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; --rejected.height;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; rejected.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
        assert(!CanPresentDirectly(rejected, false, false));
        rejected = frame; rejected.opaque_ui_region_count = 1;
        assert(!CanPresentDirectly(rejected, false, false));
        const uint8_t alpha = 255;
        frame.ui_alpha_plane = {.pixels = pixels.data(), .alpha = &alpha,
            .pixel_stride_bytes = 2, .alpha_stride_bytes = 1,
            .x = 0, .y = 0, .width = 1, .height = 1, .opacity = 255, .visible = 1, .revision = 0};
        assert(!CanPresentDirectly(frame, false, false));
        frame.suppress_guest_alpha = 1;
        assert(!CanPresentDirectly(frame, false, false)); // conservative fallback
        frame.system_alpha_plane = frame.ui_alpha_plane;
        frame.ui_alpha_plane.visible = 0;
        assert(!CanPresentDirectly(frame, false, false));
        frame.system_alpha_plane.opacity = 0;
        assert(CanPresentDirectly(frame, false, false));
    }
}

// Compare clipped row copies against the independent scalar pixel definition,
// including odd source alignment/stride, letterboxes and overlapping planes.
static void CheckRowComposition() {
    uint32_t random = 0x78f31;
    const auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    std::vector<uint8_t> source(647 * kHeight + 1);
    for (auto& v : source) v = uint8_t(next() >> 24);
    constexpr int pw = 67, ph = 39, ps = pw + 2, as = pw + 3;
    std::vector<uint16_t> colors(ps * ph);
    std::vector<uint8_t> alpha(as * ph);
    for (auto& v : colors) v = uint16_t(next() >> 16);
    for (size_t i = 0; i < alpha.size(); ++i) alpha[i] = i % 3 ? uint8_t(next() >> 24) : 255;
    for (const auto dims : {std::array{320,240}, std::array{160,144}, std::array{37,199}})
    for (const auto origin : {std::array{0,0}, std::array{-23,17}, std::array{319,-200}})
    for (int opacity : {0,1,128,255})
    for (bool suppress : {false,true}) {
        pxa_esp_surface_frame_t f{};
        f.pixels=source.data()+1; f.width=dims[0]; f.height=dims[1];
        f.stride_bytes=647; f.format=PXA_SURFACE_FORMAT_RGB565; f.visible=1;
        f.x=origin[0]; f.y=origin[1]; f.suppress_guest_alpha=suppress;
        f.ui_alpha_plane={.pixels=colors.data(), .alpha=alpha.data(),
            .pixel_stride_bytes=ps*2,.alpha_stride_bytes=as,
            .x=3,.y=39,.width=pw,.height=ph,.opacity=uint8_t(opacity),.visible=1,.revision=0};
        f.system_alpha_plane=f.ui_alpha_plane;
        f.system_alpha_plane.x=-7; f.system_alpha_plane.y=27;
        int ox=f.x, oy=f.y;
        if (!ox && !oy && (f.width<kWidth || f.height<kHeight)) {
            ox=(kWidth-f.width)/2; oy=(kHeight-f.height)/2;
        }
        for (int ty : {-9,0,26,38,47,50,77,233})
        for (int tx : {-8,0,2,37,68,79,158,311}) {
            constexpr int w=17,h=11,stride=23;
            std::array<uint16_t,stride*h+2> actual{}, expected{};
            actual.fill(0x07e0); expected=actual;
            for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
                const int lx=tx+x,ly=ty+y; uint16_t v=0x07e0;
                if(lx>=ox && ly>=oy && lx<ox+f.width && ly<oy+f.height) {
                    const auto* p=f.pixels+(ly-oy)*f.stride_bytes+(lx-ox)*2;
                    v=uint16_t(p[0]|p[1]<<8);
                }
                if(!suppress) v=BlendPlane(v,f.ui_alpha_plane,lx,ly);
                expected[1+y*stride+x]=BlendPlane(v,f.system_alpha_plane,lx,ly);
            }
            Compose(f,actual.data()+1,tx,ty,w,h,stride);
            assert(actual==expected);
        }
    }
}

int main() {
    CheckDirectEligibility();
    CheckRowComposition();
    std::array<uint16_t, 160 * 120> source{};
    source[0] = 0xf800; source[1] = 0x001f;
    pxa_esp_surface_frame_t frame{};
    frame.pixels = reinterpret_cast<const uint8_t*>(source.data());
    frame.width = 160; frame.height = 120; frame.stride_bytes = 320;
    frame.visible = 1; frame.format = PXA_SURFACE_FORMAT_RGB565;
    std::array<uint16_t, 12> tile;
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 4, 2, 6);
    assert(tile[0] == 0xf800 && tile[1] == 0xf800 && tile[2] == 0x001f && tile[3] == 0x001f);
    assert(tile[6] == 0xf800 && tile[7] == 0xf800 && tile[8] == 0x001f && tile[9] == 0x001f);
    assert(tile[4] == 0x07e0 && tile[5] == 0x07e0 && tile[10] == 0x07e0); // padded stride

    frame.opaque_ui_region_count = 1;
    frame.opaque_ui_regions[0] = {0, 0, 1, 1};
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 4, 2, 6);
    assert(tile[0] == 0x07e0 && tile[1] == 0x07e0 && tile[2] == 0x001f);
    const uint16_t white = 0xffff, black = 0;
    const uint8_t opaque = 255;
    frame.ui_alpha_plane = {.pixels = &white, .alpha = &opaque,
        .pixel_stride_bytes = 2, .alpha_stride_bytes = 1,
        .x = 0, .y = 0, .width = 1, .height = 1, .opacity = 255, .visible = 1, .revision = 0};
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 4, 2, 6);
    assert(tile[0] == white && tile[1] == 0x07e0);
    frame.suppress_guest_alpha = true;
    frame.system_alpha_plane = frame.ui_alpha_plane;
    frame.system_alpha_plane.pixels = &black;
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 4, 2, 6);
    assert(tile[0] == black && tile[1] == 0x07e0); // trusted overlay remains

    // A centered 10x10 Surface scales 4x to (140,100), leaving letterbox pixels intact.
    frame = {};
    frame.pixels = reinterpret_cast<const uint8_t*>(source.data());
    frame.width = 10; frame.height = 10; frame.stride_bytes = 320;
    frame.visible = 1; frame.format = PXA_SURFACE_FORMAT_RGB565;
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 139, 99, 3, 3, 4);
    assert(tile[0] == 0x07e0 && tile[4] == 0x07e0 && tile[5] == 0xf800 && tile[6] == 0xf800);

    std::array<uint32_t, 320 * 240> argb{};
    argb[321] = 0x80800000; // premultiplied half-red
    frame.pixels = reinterpret_cast<const uint8_t*>(argb.data());
    frame.width = 320; frame.height = 240; frame.stride_bytes = 320 * 4;
    frame.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
    frame.x = -1; frame.y = -1;
    tile.fill(0x001f);
    Compose(frame, tile.data(), 0, 0, 2, 2, 4);
    assert(tile[0] == 0x800f && tile[1] == 0x001f && tile[4] == 0x001f);
    frame.visible = 0;
    tile.fill(0x07e0);
    Compose(frame, tile.data(), 0, 0, 2, 2, 4);
    assert(tile[0] == 0x07e0);
}
