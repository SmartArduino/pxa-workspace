#include <cassert>
#include <cstdio>
#include <vector>

#include "esp_mosaico_config.h"
#include "esp_mosaico_transfer.h"
#include "esp_mosaico_audio_transfer.h"
#include "esp_mosaico_wake_touch.h"
#include "esp_mosaico_touch_events.h"
#include "esp_mosaico_touch_state.h"
#include "esp_mosaico_touch_slots.h"
#include "esp_mosaico_touch_queue.h"
#include "../src/esp_mosaico_pxa_surface.cc"

static_assert(mosaico_board::DecodeHardwareConfig(0x0100)->display_reset == 42);
static_assert(mosaico_board::DecodeHardwareConfig(0x0100)->display_clock == 44);
static_assert(mosaico_board::DecodeHardwareConfig(0x0100)->i2c_data == 0);
static_assert(mosaico_board::DecodeHardwareConfig(0x0100)->i2c_clock == 1);
static_assert(mosaico_board::DecodeHardwareConfig(0x0100)->codec_power == 56);
static_assert(mosaico_board::DecodeHardwareConfig(0x0100)->status_led == 3);
static_assert(mosaico_board::DecodeHardwareConfig(0x0101)->display_reset == 44);
static_assert(mosaico_board::DecodeHardwareConfig(0x0101)->display_clock == 42);
static_assert(mosaico_board::DecodeHardwareConfig(0x0101)->i2c_data == 56);
static_assert(mosaico_board::DecodeHardwareConfig(0x0101)->i2c_clock == 3);
static_assert(mosaico_board::DecodeHardwareConfig(0x0101)->codec_power == -1);
static_assert(mosaico_board::DecodeHardwareConfig(0x0101)->status_led == -1);
static_assert(mosaico_board::DecodeHardwareConfig(0x0102)->display_reset == 44);
static_assert(mosaico_board::DecodeHardwareConfig(0x0102)->display_clock == 42);
static_assert(mosaico_board::DecodeHardwareConfig(0x0102)->i2c_data == 56);
static_assert(mosaico_board::DecodeHardwareConfig(0x0102)->i2c_clock == 3);
static_assert(mosaico_board::DecodeHardwareConfig(0x0102)->codec_power == -1);
static_assert(mosaico_board::DecodeHardwareConfig(0x0102)->status_led == -1);
static_assert(!mosaico_board::DecodeHardwareConfig(0));
static_assert(!mosaico_board::DecodeHardwareConfig(0xffff));
static_assert(!mosaico_board::DecodeHardwareConfig(0x0103));
static_assert(!mosaico_board::DecodeHardwareConfig(0x0200));

namespace {

pxa_esp_surface_frame_t g_frame = {};
bool g_has_frame = false;
bool g_released = false;
bool g_ppa_fail = false;
unsigned g_ppa_calls = 0;

void PrepareFrame(const void* pixels, uint16_t width, uint16_t height,
                   uint32_t stride, uint16_t format = PXA_SURFACE_FORMAT_RGB565) {
    g_frame = {};
    g_frame.pixels = static_cast<const uint8_t*>(pixels);
    g_frame.width = width;
    g_frame.height = height;
    g_frame.stride_bytes = stride;
    g_frame.format = format;
    g_frame.visible = 1;
    g_frame.lease = 42;
    g_frame.input_timestamp_us = 123;
    g_has_frame = true;
    g_released = false;
}

void TestRgb565(uint8_t scale) {
    const uint16_t width = 480 / scale;
    const size_t stride = width + 2;
    std::vector<uint16_t> source(stride * width);
    for (size_t pixel_y = 0; pixel_y < width; ++pixel_y) {
        for (size_t pixel_x = 0; pixel_x < width; ++pixel_x)
            source[pixel_y * stride + pixel_x] = pixel_y * 9 + pixel_x * 17;
    }
    PrepareFrame(source.data(), width, width, stride * sizeof(uint16_t));
    const lv_area_t area = {201, 207, 209, 213};
    const size_t output_stride = 12;
    uint16_t output[12 * 7];
    for (uint16_t& pixel : output) pixel = 0x5aa5;
    assert(mosaico_pxa_surface::ComposeFlushArea(
        &area, reinterpret_cast<uint8_t*>(output),
        output_stride * sizeof(uint16_t)) == 123);
    assert(g_released);
    for (int32_t pixel_y = area.y1; pixel_y <= area.y2; ++pixel_y) {
        for (int32_t pixel_x = area.x1; pixel_x <= area.x2; ++pixel_x) {
            const uint16_t expected = source[(pixel_y / scale) * stride + pixel_x / scale];
            const size_t offset = (pixel_y - area.y1) * output_stride + pixel_x - area.x1;
            assert(output[offset] == __builtin_bswap16(expected));
        }
        for (size_t padding = 9; padding < output_stride; ++padding)
            assert(output[(pixel_y - area.y1) * output_stride + padding] == 0x5aa5);
    }
}

void TestAreaAlignment() {
    for (int32_t pixel_y = 0; pixel_y < 480; ++pixel_y) {
        for (int32_t pixel_x = 0; pixel_x < 480; ++pixel_x) {
            lv_area_t area = {pixel_x, pixel_y, pixel_x, pixel_y};
            mosaico_board::RoundDisplayArea(&area);
            assert(mosaico_board::ValidDisplayArea(area));
            assert(area.x1 <= pixel_x && area.x2 >= pixel_x);
            assert(area.y1 <= pixel_y && area.y2 >= pixel_y);
            assert(area.x1 % 4 == 0 && (area.x2 + 1) % 4 == 0);
            assert(area.y1 % 2 == 0 && (area.y2 + 1) % 2 == 0);
        }
    }
    lv_area_t clipped = {-4, -2, 482, 484};
    mosaico_board::RoundDisplayArea(&clipped);
    assert(clipped.x1 == 0 && clipped.y1 == 0);
    assert(clipped.x2 == 479 && clipped.y2 == 479);
    mosaico_board::RoundDisplayArea<lv_area_t>(nullptr);
}

void TestDirectEligibility() {
    std::vector<uint16_t> pixels(480 * 480);
    for (uint16_t resolution : {480, 240, 120}) {
        PrepareFrame(pixels.data(), resolution, resolution, resolution * 2);
        assert(mosaico_pxa_surface::DirectFrameEligible(g_frame));
    }
    PrepareFrame(pixels.data(), 480, 480, 960);
    g_frame.x = 1;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.x = 0;
    g_frame.y = -1;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.y = 0;
    g_frame.visible = 0;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.visible = 1;
    g_frame.opaque_ui_region_count = 1;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.opaque_ui_region_count = 0;
    g_frame.ui_alpha_plane.visible = 1;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.ui_alpha_plane.visible = 0;
    g_frame.system_alpha_plane.visible = 1;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.system_alpha_plane.visible = 0;
    g_frame.stride_bytes = 958;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.stride_bytes = 961;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.stride_bytes = 960;
    g_frame.format = PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.format = PXA_SURFACE_FORMAT_RGB565;
    g_frame.width = 300;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_frame.width = 480;
    g_frame.pixels = nullptr;
    assert(!mosaico_pxa_surface::DirectFrameEligible(g_frame));
    g_has_frame = false;
}

void TestOptimizedComposition() {
    const lv_area_t areas[] = {{0, 0, 479, 479}, {203, 209, 218, 222},
                               {0, 0, 6, 8}, {473, 471, 479, 479}};
    for (uint8_t scale : {1, 2, 4}) {
        const uint16_t width = 480 / scale;
        const size_t source_stride = width + 3;
        std::vector<uint16_t> source(source_stride * width);
        for (size_t index = 0; index < source.size(); ++index)
            source[index] = static_cast<uint16_t>(index * 37);
        for (const lv_area_t& area : areas) {
            PrepareFrame(source.data(), width, width, source_stride * 2);
            const uint32_t stride = (lv_area_get_width(&area) + 4) * 2;
            const size_t count = stride / 2 * (area.y2 - area.y1 + 1);
            std::vector<uint16_t> optimized(count, 0x5aa5);
            std::vector<uint16_t> reference(count, 0x5aa5);
            assert(mosaico_pxa_surface::ComposeFrame(g_frame, &area,
                reinterpret_cast<uint8_t*>(optimized.data()), stride) == 123);
            g_frame.ui_alpha_plane.visible = 1;
            assert(mosaico_pxa_surface::ComposeFrame(g_frame, &area,
                reinterpret_cast<uint8_t*>(reference.data()), stride) == 123);
            assert(optimized == reference);
        }
    }
    std::vector<uint16_t> source(300 * 100, 0xf800);
    PrepareFrame(source.data(), 300, 100, 600);
    for (int32_t origin : {-12, 0, 400}) {
        g_frame.x = origin;
        g_frame.y = origin;
        g_frame.ui_alpha_plane.visible = 0;
        std::vector<uint16_t> optimized(480 * 480, 0xaaaa);
        std::vector<uint16_t> reference(480 * 480, 0xaaaa);
        const lv_area_t area = {0, 0, 479, 479};
        mosaico_pxa_surface::ComposeFrame(g_frame, &area,
            reinterpret_cast<uint8_t*>(optimized.data()));
        g_frame.ui_alpha_plane.visible = 1;
        mosaico_pxa_surface::ComposeFrame(g_frame, &area,
            reinterpret_cast<uint8_t*>(reference.data()));
        assert(optimized == reference);
    }
    g_has_frame = false;
}

void TestPpaDirectAndFallback() {
    using namespace mosaico_pxa_surface;
    g_ppa_srm = nullptr;
    assert(!TryAcceleratedDirectFrame(g_frame, nullptr));
    InitializeAcceleration();
    const lv_area_t full = {0, 0, 479, 479};
    for (uint8_t scale : {1, 2, 4}) {
        const uint16_t width = 480 / scale;
        // Padded input rows must use pic_w as stride, not block width.
        std::vector<uint16_t> source((width + 4) * width, 0x5aa5);
        for (size_t y = 0; y < width; ++y)
            for (size_t x = 0; x < width; ++x)
                source[y * (width + 4) + x] = static_cast<uint16_t>(x * 131 + y * 37);
        PrepareFrame(source.data(), width, width, (width + 4) * 2);
        std::vector<uint16_t> accelerated(480 * 480), software(accelerated.size());
        assert(TryAcceleratedDirectFrame(g_frame, reinterpret_cast<uint8_t*>(accelerated.data())));
        ComposeFrame(g_frame, &full, reinterpret_cast<uint8_t*>(software.data()));
        assert(accelerated == software);
        const unsigned calls = g_ppa_calls;
        g_frame.system_alpha_plane.visible = 1;
        assert(!TryAcceleratedDirectFrame(g_frame, reinterpret_cast<uint8_t*>(accelerated.data())));
        assert(g_ppa_calls == calls); // Missing trusted snapshots stay on LVGL.
        g_frame.system_alpha_plane.visible = 0;
        g_ppa_fail = true;
        assert(!TryAcceleratedDirectFrame(g_frame, reinterpret_cast<uint8_t*>(accelerated.data())));
        std::fill(accelerated.begin(), accelerated.end(), 0);
        ComposeFrame(g_frame, &full, reinterpret_cast<uint8_t*>(accelerated.data()));
        assert(accelerated == software); // Error permits complete CPU repaint.
        g_ppa_fail = false;
    }
    std::vector<uint16_t> background(480 * 480, 0x1234);
    const uint16_t top[] = {0, 160, 320}, bottom[] = {160, 320, 480};
    const uint16_t colors[] = {0xf800, 0x07e0, 0x001f};
    assert(FillRasterBands(nullptr, background.data(), 480, 480, 480, top, bottom, colors, 3));
    for (size_t y = 0; y < 480; ++y)
        for (size_t x = 0; x < 480; ++x)
            assert(background[y * 480 + x] == colors[y / 160]);
    const auto previous = background;
    const uint16_t invalid_bottom[] = {160, 320, 481};
    assert(!FillRasterBands(nullptr, background.data(), 480, 480, 480, top, invalid_bottom, colors, 3));
    assert(background == previous);
    g_ppa_fail = true;
    assert(!FillRasterBands(nullptr, background.data(), 480, 480, 480, top, bottom, colors, 3));
    g_ppa_fail = false;
    g_has_frame = false;
}

void TestPpaAlphaComposition() {
    using namespace mosaico_pxa_surface;
    const lv_area_t full = {0, 0, 479, 479};
    // Independent color/alpha padding, overlapping guest/system planes and
    // clipped edges must match the existing software compositor byte for byte.
    std::vector<uint16_t> colors(40 * 70);
    std::vector<uint8_t> alpha(42 * 70);
    for (size_t i = 0; i < colors.size(); ++i) colors[i] = i * 179;
    for (size_t i = 0; i < alpha.size(); ++i) alpha[i] = i % 4 == 0 ? 0 : i % 4 == 1 ? 255 : i;
    for (uint8_t scale : {1, 2, 4}) {
        const uint16_t width = 480 / scale;
        std::vector<uint16_t> source((width + 4) * width);
        for (size_t i = 0; i < source.size(); ++i) source[i] = i * 37;
        PrepareFrame(source.data(), width, width, (width + 4) * 2);
        g_frame.ui_alpha_plane = {colors.data(), alpha.data(), 80, 42,
            4, 203, 36, 68, 170, 1, 1};
        g_frame.system_alpha_plane = {colors.data(), alpha.data(), 80, 42,
            12, 211, 36, 68, 255, 1, 2};
        for (int x : {-12, 12, 470}) {
            g_frame.system_alpha_plane.x = x;
            for (bool suppress_guest : {false, true}) {
                g_frame.suppress_guest_alpha = suppress_guest;
                assert(DirectFrameEligible(g_frame));
                std::vector<uint16_t> actual(480 * 480), expected(actual.size());
                ComposeFrame(g_frame, &full, reinterpret_cast<uint8_t*>(expected.data()));
                assert(TryAcceleratedDirectFrame(g_frame, reinterpret_cast<uint8_t*>(actual.data())));
                assert(actual == expected);
                // A failed accelerator leaves the CPU path responsible for
                // painting both the game and every overlay exactly once.
                g_ppa_fail = true;
                assert(!TryAcceleratedDirectFrame(g_frame, reinterpret_cast<uint8_t*>(actual.data())));
                std::fill(actual.begin(), actual.end(), 0x5aa5);
                ComposeFrame(g_frame, &full, reinterpret_cast<uint8_t*>(actual.data()));
                assert(actual == expected);
                g_ppa_fail = false;
            }
        }
        g_frame.system_alpha_plane.pixel_stride_bytes = 70;
        assert(!DirectFrameEligible(g_frame));
        g_frame.system_alpha_plane.pixel_stride_bytes = 80;
        g_frame.system_alpha_plane.alpha_stride_bytes = 35;
        assert(!DirectFrameEligible(g_frame));
        g_frame.system_alpha_plane.alpha_stride_bytes = 42;
        g_frame.opaque_ui_region_count = 1;
        assert(!DirectFrameEligible(g_frame));
    }
    g_has_frame = false;
}

void TestTransferPipeline() {
    const lv_area_t area = {12, 20, 27, 20 + 2 * mosaico_board::kTransferRows + 14};
    const size_t row_bytes = 16 * 2;
    const size_t stride = row_bytes + 8;
    std::vector<uint8_t> source(stride * (area.y2 - area.y1 + 1));
    for (size_t i = 0; i < source.size(); ++i) source[i] = i * 37;
    for (int failed_wait : {-1, 0, 2, 4}) {
        std::vector<uint8_t> storage[2] = {
            std::vector<uint8_t>(row_bytes * mosaico_board::kTransferRows, 0xaa),
            std::vector<uint8_t>(row_bytes * mosaico_board::kTransferRows, 0xbb)};
        uint8_t* buffers[2] = {storage[0].data(), storage[1].data()};
        const uint8_t* pending = nullptr;
        std::vector<uint8_t> expected;
        int waits = 0, submissions = 0, mirrored = 0;
        const bool completed = mosaico_board::TransferBands(area, source.data(),
            stride, buffers,
            [&]() {
                if (pending != nullptr)
                    assert(std::memcmp(pending, expected.data(), expected.size()) == 0);
                if (waits++ == failed_wait) return false;
                pending = nullptr;
                return true;
            },
            [&](int32_t y, int32_t rows, const uint8_t* band) {
                assert(pending == nullptr);
                assert(band == buffers[submissions % 2]);
                pending = band;
                expected.resize(rows * row_bytes);
                for (int32_t row = 0; row < rows; ++row)
                    std::memcpy(expected.data() + row * row_bytes,
                        source.data() + (y + row - area.y1) * stride, row_bytes);
                assert(std::memcmp(band, expected.data(), expected.size()) == 0);
                ++submissions;
                return true;
            },
            [&](int32_t, int32_t) { assert(pending != nullptr); ++mirrored; });
        assert(completed == (failed_wait == -1));
        assert(submissions == mirrored);
        if (completed) assert(submissions == 3 && pending == nullptr);
        if (pending != nullptr)
            assert(std::memcmp(pending, expected.data(), expected.size()) == 0);
    }
}

void TestRefreshBatch() {
    mosaico_board::RefreshRows rows;
    std::vector<uint8_t> frame(480 * 480 * 2, 0x39);
    const lv_area_t areas[] = {{28, 40, 31, 41}, {400, 200, 407, 203},
                               {30, 41, 33, 42}};
    for (size_t i = 0; i < 3; ++i) {
        const size_t bytes = (areas[i].x2 - areas[i].x1 + 1) * 2;
        const size_t stride = bytes + 8;
        std::vector<uint8_t> source(stride * (areas[i].y2 - areas[i].y1 + 1),
                                    static_cast<uint8_t>(0x70 + i));
        assert(rows.Merge(areas[i], source.data(), stride, frame.data()));
    }
    lv_area_t area;
    assert(rows.Take(&area));
    assert(area.x1 == 0 && area.x2 == 479 && area.y1 == 40 && area.y2 == 203);
    assert(!rows.Take(&area));
    for (int y = 0; y < 480; ++y) {
        for (int x = 0; x < 480; ++x) {
            uint8_t expected = 0x39;
            for (size_t i = 0; i < 3; ++i)
                if (x >= areas[i].x1 && x <= areas[i].x2 &&
                    y >= areas[i].y1 && y <= areas[i].y2) expected = 0x70 + i;
            assert(frame[(y * 480 + x) * 2] == expected);
            assert(frame[(y * 480 + x) * 2 + 1] == expected);
        }
    }
    const lv_area_t bottom = {40, 478, 43, 479};
    uint8_t source[16] = {};
    assert(!rows.Merge(bottom, source, 7, frame.data()));
    assert(!rows.Take(&area));
    assert(rows.Merge(bottom, source, 8, frame.data()));
    assert(rows.Take(&area) && area.y1 == 478 && area.y2 == 479);
    assert(frame[(40 * 480 + 28) * 2] == 0x70);
}

void TestPipelinedSnapshot() {
    constexpr size_t row_bytes = mosaico_board::kWidth * sizeof(uint16_t);
    std::vector<uint8_t> frame(row_bytes * mosaico_board::kHeight, 0x39);
    std::vector<uint8_t> snapshot(frame.size(), 0xaa);
    mosaico_board::RefreshRows dirty;
    const lv_area_t patch = {28, 40, 31, 99};
    std::vector<uint8_t> pixels(8 * 60, 0x70);
    assert(dirty.Merge(patch, pixels.data(), 8, frame.data()));
    lv_area_t rows;
    assert(dirty.Take(&rows));
    assert(mosaico_board::CopyRefreshRows(rows, frame.data(), snapshot.data()));
    const auto submitted = snapshot;
    std::vector<uint8_t> storage[2] = {
        std::vector<uint8_t>(row_bytes * mosaico_board::kTransferRows),
        std::vector<uint8_t>(row_bytes * mosaico_board::kTransferRows)};
    uint8_t* buffers[2] = {storage[0].data(), storage[1].data()};
    int submissions = 0;
    // While a submitted frame is being transferred, emulate drawing the next
    // frame into both previously dirty and previously untouched rows.
    const lv_area_t next_patch = {28, 90, 31, 149};
    const bool completed = mosaico_board::TransferBands(rows,
        snapshot.data() + rows.y1 * row_bytes, row_bytes, buffers,
        [&]() { assert(snapshot == submitted); return true; },
        [&](int32_t y, int32_t count, const uint8_t* band) {
            assert(std::memcmp(band, submitted.data() + y * row_bytes,
                                count * row_bytes) == 0);
            ++submissions;
            return true;
        },
        [&](int32_t, int32_t) {
            std::fill(pixels.begin(), pixels.end(), 0x80);
            assert(dirty.Merge(next_patch, pixels.data(), 8, frame.data()));
        });
    assert(completed && submissions == 4 && snapshot == submitted);
    assert(dirty.Take(&rows));
    assert(rows.y1 == 90 && rows.y2 == 149);
    assert(mosaico_board::CopyRefreshRows(rows, frame.data(), snapshot.data()));
    // Preserve earlier patches outside the new dirty rows; copy the newly
    // touched pixels, including ones beyond the previous transfer's bounds.
    assert(snapshot[40 * row_bytes + 28 * 2] == 0x70);
    assert(snapshot[90 * row_bytes + 28 * 2] == 0x80);
    assert(snapshot[149 * row_bytes + 28 * 2] == 0x80);
    assert(snapshot[149 * row_bytes] == 0x39);
    assert(snapshot[150 * row_bytes] == 0xaa);
    assert(!mosaico_board::CopyRefreshRows(patch, frame.data(), snapshot.data()));
    assert(!mosaico_board::CopyRefreshRows(rows, nullptr, snapshot.data()));
}

void TestAudioWriteRecovery() {
    mosaico_board::AudioDmaProgress progress;
    // Idle, startup latency, short effects and counter wrap all stay healthy.
    for (int i = 0; i < 100; ++i) assert(!progress.Stalled(false, 0));
    for (int i = 0; i < 6; ++i) assert(!progress.Stalled(true, 0));
    assert(!progress.Stalled(true, 1));
    for (int i = 0; i < 20; ++i) assert(!progress.Stalled(true, 1));
    assert(!progress.Stalled(false, 1));
    assert(!progress.Stalled(true, UINT32_MAX));
    assert(!progress.Stalled(true, 0));
    // Continuous accepted PCM without any real DMA audio triggers recovery.
    for (int i = 0; i < 21; ++i) assert(!progress.Stalled(true, 0));
    assert(progress.Stalled(true, 0));
    assert(!progress.Stalled(true, 0));
    const int16_t pcm[] = {0, 12000, -12000, 32767, -32768};
    for (int failure : {0, 1, 2, 3, 4}) {
        int writes = 0, restarts = 0;
        const bool ok = mosaico_board::SubmitAudioFrame(pcm, sizeof(pcm),
            [&](const void* data, size_t bytes, size_t* written) {
                assert(data == pcm && bytes == sizeof(pcm));
                ++writes;
                // 1: partial success; 2: timeout; 3: persistent failure;
                // 4: restart itself fails. Never treat a short write as OK.
                *written = writes == 1 && failure == 1 ? 2 : bytes;
                return failure == 0 || (failure == 1) ||
                    (writes > 1 && failure == 2);
            }, [&]() { ++restarts; return failure != 4; });
        assert(ok == (failure <= 2));
        assert(restarts == (failure != 0));
        assert(writes == (failure == 0 || failure == 4 ? 1 : 2));
    }
}

void TestWakeGestureIsolation() {
    MosaicoWakeTouch touch;
    assert(!touch.Consume(0, false, true));
    assert(touch.Consume(0, true, true));
    // Once the UI turns on, the waking drag still cannot unlock or reach the
    // app. A second contact in the same gesture is consumed through release.
    assert(touch.Consume(0, true, false));
    assert(touch.Consume(1, true, false));
    assert(touch.Consume(0, false, false));
    assert(touch.active());
    assert(touch.Consume(1, false, false));
    assert(!touch.active());
    // A new gesture on the visible lock screen is delivered for unlocking.
    assert(!touch.Consume(0, true, false));
    assert(!touch.Consume(0, false, false));
    // Stale-contact release after an I2C error also clears the wake gate.
    assert(touch.Consume(1, true, true));
    assert(touch.Consume(1, false, false));
    assert(!touch.active());
}

void TestExchangedFramesAndUiTransition() {
    constexpr size_t count = mosaico_board::kWidth * mosaico_board::kHeight;
    std::vector<uint16_t> first(count, 0x3412), second(count, 0xcdab);
    uint8_t* producer = reinterpret_cast<uint8_t*>(first.data());
    uint8_t* snapshot = reinterpret_cast<uint8_t*>(second.data());
    const lv_area_t full = {0, 0, 479, 479};
    const lv_area_t patch = {20, 30, 21, 31};
    assert(!mosaico_board::ExchangeRefreshFrame(patch, producer, snapshot));
    assert(mosaico_board::ExchangeRefreshFrame(full, producer, snapshot));
    // Rendering the next frame must not modify the frame owned by DMA.
    std::fill(second.begin(), second.end(), 0x7856);
    assert(first.front() == 0x3412 && first.back() == 0x3412);
    assert(mosaico_board::ExchangeRefreshFrame(full, producer, snapshot));
    // A partial UI refresh after two game frames preserves the newest image
    // outside the patch. Screenshot conversion happens only on demand.
    assert(mosaico_board::CopyRefreshRows(full, snapshot, producer));
    const uint16_t overlay[] = {0x00f8, 0xe007, 0x1f00, 0xffff};
    mosaico_board::RefreshRows dirty;
    assert(dirty.Merge(patch, reinterpret_cast<const uint8_t*>(overlay), 4, producer));
    lv_area_t rows;
    assert(dirty.Take(&rows));
    assert(mosaico_board::CopyRefreshRows(rows, producer, snapshot));
    std::vector<uint16_t> capture(count);
    mosaico_board::CaptureScanoutRgb565(snapshot, capture.data());
    assert(capture.front() == 0x5678 && capture.back() == 0x5678);
    assert(capture[30 * 480 + 20] == 0xf800);
    assert(capture[30 * 480 + 21] == 0x07e0);
    assert(capture[31 * 480 + 20] == 0x001f);
    assert(capture[31 * 480 + 21] == 0xffff);
    assert(capture[30 * 480 + 19] == 0x5678);
}

void TestScanFollowTiming() {
    // Model panel reads and row-write completions. Starting a slow full-screen
    // writer at TE rising gives new pixels at the top and old at the bottom.
    // Following the scan gives all old pixels now and all new pixels next time.
    const double period = 16931, blank = 800;
    // Include ideal wire speed at the configured clock as well as observed
    // slower copies/transfers. Faster writes must not overtake the panel scan.
    const double wire_row_us = mosaico_board::kWidth * 16.0 * 1000000 /
                               (4.0 * mosaico_board::kPixelClockHz);
    for (double row_write : {wire_row_us, 48.0, 56.0}) {
        assert(row_write < blank);
        assert(480 * row_write > period);
        for (uint32_t panel_period : {16667u, 16931u}) {
            for (int first : {0, 50, 200, 478}) {
                for (int last : {first, std::min(479, first + 79), 479}) {
                    for (uint32_t scheduling_delay : {0u, 1000u, 2000u}) {
                        const double start = blank + scheduling_delay +
                            mosaico_board::ScanFollowDelayUs(first,
                                panel_period - static_cast<uint32_t>(blank));
                        for (int y = first; y <= last; ++y) {
                            const double write = start + (y - first + 1) * row_write;
                            const double read = blank +
                                y * (panel_period - blank) / 480;
                            assert(write > read);
                            assert(write < read + panel_period);
                        }
                    }
                }
            }
        }
    }
}

void TestTouchClockAndIdle() {
    for (uint32_t boot_offset_ms : {100u, 2000u, 10000u}) {
        const uint32_t lv_now = 25000;
        const int64_t read_us = static_cast<int64_t>(lv_now + boot_offset_ms) * 1000;
        const int64_t sample_us = read_us - 12000;
        // Model LVGL's last_activity_time = data.timestamp and its unsigned
        // elapsed calculation. The previous absolute timestamp locks at once.
        const uint32_t previous_inactive = lv_now - static_cast<uint32_t>(sample_us / 1000);
        assert(previous_inactive > 60000);
        const uint32_t timestamp = MosaicoTouchLvglTimestamp(sample_us, read_us, lv_now, true);
        assert(lv_now - timestamp == 12);
    }
    EspMosaicoTouchQueue queue;
    queue.Update({{240, 8, 3}, true}, 10000000);
    bool queued = queue.pending();
    auto held = queue.Read();
    assert(MosaicoTouchLvglTimestamp(held.timestamp_us, 10000000, 1000, queued) == 1000);
    // A motionless held finger produces no new queue entries. Reading it for
    // longer than the lock interval must still keep LVGL's idle age at zero.
    uint32_t last_activity = 1000;
    for (uint32_t elapsed = 500; elapsed <= 120000; elapsed += 500) {
        queued = queue.pending();
        held = queue.Read();
        assert(!queued && held.event.pressed);
        const uint32_t lv_now = 1000 + elapsed;
        last_activity = MosaicoTouchLvglTimestamp(held.timestamp_us,
            10000000 + static_cast<int64_t>(elapsed) * 1000, lv_now, queued);
        assert(lv_now - last_activity == 0);
    }
    queue.Update({{240, 8, 3}, false}, 130001000);
    assert(!queue.Read().event.pressed);
    // Released samples don't update LVGL's last activity; real idle expiry
    // therefore remains enabled after the user lets go.
    assert((last_activity + 60000) - last_activity == 60000);
    const uint32_t wrapped = MosaicoTouchLvglTimestamp(990000, 1000000, 3, true);
    assert(static_cast<uint32_t>(3 - wrapped) == 10);
    assert(MosaicoTouchLvglTimestamp(1000001, 1000000, 40, true) == 40);
    assert(MosaicoTouchLvglTimestamp(0, 1000000, 40, true) == 40);
}

void TestDelayedTouchReads() {
    EspMosaicoTouchQueue queue;
    queue.Update({{240, 8, 3}, true}, 1000);
    for (int y = 10; y <= 200; y += 10)
        queue.Update({{240, static_cast<uint16_t>(y), 3}, true}, y * 1000);
    queue.Update({{240, 200, 3}, false}, 201000);
    // A busy renderer must still hit-test the original top-edge press, then
    // see the latest movement and the release, even after the whole gesture.
    auto sample = queue.Read();
    assert(sample.event.pressed && sample.event.point.y == 8 && sample.timestamp_us == 1000);
    assert(queue.pending());
    sample = queue.Read();
    assert(sample.event.pressed && sample.event.point.y == 200 && sample.motion);
    sample = queue.Read();
    assert(!sample.event.pressed && !queue.pending());
    queue.Update({{20, 40, 5}, true}, 202000);
    queue.Update({{80, 140, 7}, true}, 203000);
    assert(queue.Read().event.point.id == 5);
    assert(!queue.Read().event.pressed);
    sample = queue.Read();
    assert(sample.event.pressed && sample.event.point.id == 7);
    // Overflow must cancel the held consumer gesture before starting a new
    // one. A later release cannot leave the UI permanently pressed.
    for (int i = 0; i < 40; ++i)
        queue.Update({{100, 100, 7}, i % 2 != 0}, 204000 + i * 1000);
    assert(!queue.Read().event.pressed);
    while (queue.pending()) (void)queue.Read();
    queue.Update({{100, 100, 7}, false}, 250000);
    assert(!queue.Read().event.pressed);
}

void TestTouchRecovery() {
    EspMosaicoTouchState state;
    EspMosaicoTouchPoint captured[2] = {};
    const EspMosaicoTouchPoint pressed[2] = {{0, 479, 3}, {479, 0, 7}};
    assert(state.Snapshot(0, captured, 2) == 0);
    state.Update(true, pressed, 2, 1000);
    assert(state.Snapshot(1000, captured, 2) == 2);
    assert(captured[0].x == 0 && captured[0].y == 479 && captured[0].id == 3);
    assert(captured[1].x == 479 && captured[1].y == 0 && captured[1].id == 7);
    assert(state.Snapshot(1000, captured, 1) == 1);
    state.Update(false, nullptr, 0, 10000);
    assert(state.Snapshot(150999, captured, 2) == 2);
    assert(state.Snapshot(151000, captured, 2) == 0);
    state.Update(true, pressed, 1, 200000);
    assert(state.Snapshot(200000, captured, 2) == 1);
    state.Update(true, nullptr, 0, 200001);
    assert(state.Snapshot(200001, captured, 2) == 0);
    state.Update(true, pressed, 1, 300000);
    const EspMosaicoTouchPoint malformed[1] = {{480, 480, 1}};
    state.Update(true, malformed, 1, 400000);
    assert(state.Snapshot(400000, captured, 2) == 1);
    assert(captured[0].x == 0 && captured[0].y == 479);
    assert(state.Snapshot(450000, captured, 2) == 0);
    assert(state.Snapshot(299999, captured, 2) == 0);
    assert(state.Snapshot(300000, nullptr, 2) == 0);
    state.Update(true, nullptr, 1, 460000);
    assert(state.Snapshot(460000, captured, 2) == 0);
}

void TestTouchSlots() {
    EspMosaicoTouchSlots slots;
    EspMosaicoTouchPoint points[2] = {{200, 300, 3}, {100, 400, 7}};
    slots.Update(points, 2);
    assert(slots.Get(0).pressed && slots.Get(0).point.id == 3);
    assert(slots.Get(1).pressed && slots.Get(1).point.id == 7);
    std::swap(points[0], points[1]);
    slots.Update(points, 2);
    assert(slots.Get(0).point.id == 3 && slots.Get(1).point.id == 7);
    slots.Update(points, 1);
    assert(!slots.Get(0).pressed && slots.Get(0).point.x == 200);
    assert(slots.Get(1).pressed && slots.Get(1).point.id == 7);
    const EspMosaicoTouchPoint replacement[2] = {{210, 310, 4}, {110, 410, 9}};
    slots.Update(replacement, 2);
    assert(slots.Get(0).pressed && slots.Get(0).point.id == 4);
    assert(!slots.Get(1).pressed && slots.Get(1).point.id == 7);
    slots.Update(replacement, 2);
    assert(slots.Get(0).point.id == 4 && slots.Get(1).point.id == 9);
    slots.Update(nullptr, 0);
    assert(!slots.Get(0).pressed && !slots.Get(1).pressed);
    // Immediate replacement must expose releases to both LVGL pointers.
    slots.Update(points, 2);
    slots.Update(replacement, 2);
    assert(!slots.Get(0).pressed && !slots.Get(1).pressed);
    slots.Update(replacement, 2);
    assert(slots.Get(0).pressed && slots.Get(1).pressed);
}

void TestTouchEvents() {
    EspMosaicoTouchEvents tracker;
    EspMosaicoTouchEvent events[EspMosaicoTouchEvents::kMaxEvents] = {};
    const EspMosaicoTouchPoint pressed[2] = {{200, 300, 3}, {100, 400, 7}};
    assert(tracker.Update(nullptr, 0, events) == 0);
    assert(tracker.Update(pressed, 1, events) == 1);
    assert(events[0].pressed && events[0].point.id == 3);
    assert(tracker.Update(nullptr, 0, events) == 1);
    assert(!events[0].pressed && events[0].point.id == 3);
    assert(events[0].point.x == 200 && events[0].point.y == 300);
    assert(tracker.pointer_point().x == 200 && tracker.pointer_point().y == 300);
    assert(tracker.Update(nullptr, 0, events) == 0);

    assert(tracker.Update(pressed, 2, events) == 2);
    const EspMosaicoTouchPoint reordered[2] = {pressed[1], pressed[0]};
    assert(tracker.Update(reordered, 2, events) == 2);
    assert(events[0].pressed && events[1].pressed);
    assert(events[0].point.id == 7 && events[1].point.id == 3);
    assert(tracker.Update(&pressed[1], 1, events) == 2);
    assert(!events[0].pressed && events[0].point.id == 3);
    assert(events[1].pressed && events[1].point.id == 7);
    assert(tracker.Update(nullptr, 0, events) == 1);
    assert(!events[0].pressed && events[0].point.id == 7);
    assert(events[0].point.x == 100 && events[0].point.y == 400);

    assert(tracker.Update(pressed, 2, events) == 2);
    const EspMosaicoTouchPoint replaced[2] = {{250, 310, 4}, {150, 410, 9}};
    assert(tracker.Update(replaced, 2, events) == 4);
    assert(!events[0].pressed && events[0].point.id == 3);
    assert(!events[1].pressed && events[1].point.id == 7);
    assert(events[2].pressed && events[2].point.id == 4);
    assert(events[3].pressed && events[3].point.id == 9);
    assert(tracker.Update(nullptr, 2, events) == 0);
    assert(tracker.Update(pressed, 2, nullptr) == 0);
    assert(tracker.Update(nullptr, 0, events) == 2);
    assert(!events[0].pressed && events[0].point.id == 4);
    assert(!events[1].pressed && events[1].point.id == 9);

    for (uint8_t identifier = 0; identifier < 16; ++identifier) {
        const EspMosaicoTouchPoint point = {120, 240, identifier};
        assert(tracker.Update(&point, 1, events) == 1);
        assert(events[0].pressed && events[0].point.id == identifier);
        assert(tracker.Update(nullptr, 0, events) == 1);
        assert(!events[0].pressed && events[0].point.id == identifier);
    }

    EspMosaicoTouchState state;
    EspMosaicoTouchPoint captured[2] = {};
    state.Update(true, pressed, 2, 1000);
    assert(tracker.Update(captured, state.Snapshot(1000, captured, 2), events) == 2);
    state.Update(false, nullptr, 0, 100000);
    assert(tracker.Update(captured, state.Snapshot(151000, captured, 2), events) == 2);
    assert(!events[0].pressed && events[0].point.id == 3);
    assert(!events[1].pressed && events[1].point.id == 7);
    assert(tracker.Update(captured, state.Snapshot(200000, captured, 2), events) == 0);
}

void TestPlacement() {
    std::vector<uint16_t> source(300 * 100, 0xf800);
    PrepareFrame(source.data(), 300, 100, 600);
    const lv_area_t area = {88, 188, 92, 192};
    uint16_t output[25];
    for (uint16_t& pixel : output) pixel = __builtin_bswap16(0x001f);
    (void)mosaico_pxa_surface::ComposeFlushArea(
        &area, reinterpret_cast<uint8_t*>(output));
    for (size_t offset_y = 0; offset_y < 5; ++offset_y) {
        for (size_t offset_x = 0; offset_x < 5; ++offset_x) {
            const uint16_t expected = offset_y >= 2 && offset_x >= 2 ? 0xf800 : 0x001f;
            assert(output[offset_y * 5 + offset_x] == __builtin_bswap16(expected));
        }
    }
    g_frame.x = -2;
    g_frame.y = -3;
    const lv_area_t clipped = {0, 0, 0, 0};
    uint16_t pixel = 0;
    (void)mosaico_pxa_surface::ComposeFlushArea(
        &clipped, reinterpret_cast<uint8_t*>(&pixel));
    assert(pixel == __builtin_bswap16(0xf800));
}

void TestTrustedOverlays() {
    std::vector<uint16_t> source(480 * 480, 0xf800);
    PrepareFrame(source.data(), 480, 480, 960);
    const lv_area_t area = {10, 20, 10, 20};
    uint16_t guest_color = 0x07e0;
    uint16_t system_color = 0xffff;
    uint8_t guest_alpha = 128;
    uint8_t system_alpha = 64;
    g_frame.ui_alpha_plane = {
        .pixels = &guest_color,
        .alpha = &guest_alpha,
        .pixel_stride_bytes = 2,
        .alpha_stride_bytes = 1,
        .x = 10, .y = 20, .width = 1, .height = 1,
        .opacity = 255, .visible = 1, .revision = 0,
    };
    g_frame.system_alpha_plane = g_frame.ui_alpha_plane;
    g_frame.system_alpha_plane.pixels = &system_color;
    g_frame.system_alpha_plane.alpha = &system_alpha;
    g_frame.system_alpha_plane.opacity = 128;
    for (bool opaque : {false, true}) {
        for (bool suppress : {false, true}) {
            g_frame.opaque_ui_region_count = opaque ? 1 : 0;
            g_frame.opaque_ui_regions[0] = {10, 20, 1, 1};
            g_frame.suppress_guest_alpha = suppress;
            uint16_t pixel = __builtin_bswap16(0x001f);
            uint16_t expected = opaque ? 0x001f : 0xf800;
            if (!suppress)
                expected = pxa_esp_surface_blend_alpha_pixel(
                    expected, guest_color, guest_alpha, 255);
            expected = pxa_esp_surface_blend_alpha_pixel(
                expected, system_color, system_alpha, 128);
            (void)mosaico_pxa_surface::ComposeFlushArea(
                &area, reinterpret_cast<uint8_t*>(&pixel));
            assert(pixel == __builtin_bswap16(expected));
        }
    }
}

void TestClippedOverlayFastPath() {
    const lv_area_t area = {0, 0, 63, 47};
    constexpr size_t stride = 68;
    constexpr size_t plane_stride = 24;
    std::vector<uint16_t> colors(plane_stride * 23);
    std::vector<uint8_t> alpha(plane_stride * 23);
    for (size_t i = 0; i < colors.size(); ++i) {
        colors[i] = i * 173;
        alpha[i] = i % 3 == 0 ? 0 : (i % 3 == 1 ? 128 : 255);
    }
    for (uint8_t scale : {1, 2, 4}) {
        const uint16_t side = 480 / scale;
        std::vector<uint16_t> source(side * side);
        for (size_t i = 0; i < source.size(); ++i) source[i] = i * 57;
        for (bool placed : {false, true}) {
            PrepareFrame(source.data(), side, side, side * 2);
            if (placed) { g_frame.x = 31; g_frame.y = 29; }
            g_frame.ui_alpha_plane = {
                .pixels = colors.data(), .alpha = alpha.data(),
                .pixel_stride_bytes = plane_stride * 2, .alpha_stride_bytes = plane_stride,
                .x = -3, .y = 7, .width = 19, .height = 23,
                .opacity = 255, .visible = 1, .revision = 1,
            };
            g_frame.system_alpha_plane = g_frame.ui_alpha_plane;
            g_frame.system_alpha_plane.x = 12;
            g_frame.system_alpha_plane.y = -5;
            for (unsigned variant = 0; variant < 5; ++variant) {
                g_frame.suppress_guest_alpha = variant == 1;
                g_frame.system_alpha_plane.opacity = variant == 2 ? 0 : 137;
                g_frame.system_alpha_plane.visible = variant != 3;
                g_frame.system_alpha_plane.pixels = variant == 4 ? nullptr : colors.data();
                std::vector<uint16_t> fast(stride * 48, 0x1957), reference = fast;
                g_frame.opaque_ui_region_count = 0;
                mosaico_pxa_surface::ComposeFrame(g_frame, &area,
                    reinterpret_cast<uint8_t*>(fast.data()), stride * 2);
                // Off-screen opaque rectangle selects the generic compositor
                // without affecting any tested pixels; compare exact output.
                g_frame.opaque_ui_region_count = 1;
                g_frame.opaque_ui_regions[0] = {479, 479, 1, 1};
                mosaico_pxa_surface::ComposeFrame(g_frame, &area,
                    reinterpret_cast<uint8_t*>(reference.data()), stride * 2);
                assert(fast == reference);
                for (unsigned y = 0; y < 48; ++y)
                    for (unsigned x = 64; x < stride; ++x)
                        assert(fast[y * stride + x] == 0x1957);
            }
        }
    }
}

void TestPremultipliedArgb() {
    const lv_area_t area = {0, 0, 0, 0};
    for (const auto& colors : {std::pair<uint32_t, uint16_t>{0x00000000, 0x001f},
                              {0xffff0000, 0xf800}, {0x80800000, 0x800f}}) {
        std::vector<uint32_t> source(480 * 480, colors.first);
        PrepareFrame(source.data(), 480, 480, 1920,
                      PXA_SURFACE_FORMAT_ARGB8888_PREMULTIPLIED);
        uint16_t pixel = __builtin_bswap16(0x001f);
        (void)mosaico_pxa_surface::ComposeFlushArea(
            &area, reinterpret_cast<uint8_t*>(&pixel));
        assert(pixel == __builtin_bswap16(colors.second));
    }
}

}

extern "C" bool pxa_esp_surface_acquire_latest(pxa_esp_surface_frame_t* frame) {
    if (!g_has_frame) return false;
    *frame = g_frame;
    return true;
}
extern "C" bool pxa_esp_surface_acquire_prepared(pxa_esp_surface_frame_t* frame, bool) {
    return pxa_esp_surface_acquire_latest(frame);
}
extern "C" bool pxa_esp_surface_prepare_latest_raster() { return false; }
extern "C" bool pxa_esp_surface_has_pending_frame() { return g_has_frame; }
extern "C" bool pxa_esp_surface_has_pending_raster_draw() { return false; }
extern "C" bool pxa_esp_surface_composition_required() { return false; }
extern "C" bool pxa_esp_surface_get_present_info(pxa_esp_surface_present_info_t* info) {
    if (!g_has_frame) return false;
    *info = {};
    info->visible = g_frame.visible;
    info->width = g_frame.width;
    info->height = g_frame.height;
    info->x = g_frame.x;
    info->y = g_frame.y;
    return true;
}
extern "C" void pxa_esp_surface_release_frame(uint64_t lease) {
    assert(lease == 42);
    g_released = true;
}
extern "C" bool pxa_esp_game_render_set_scale_profile(uint8_t, uint8_t) { return true; }
extern "C" void pxa_esp_surface_set_frame_ready_callback(
    pxa_esp_surface_frame_ready_fn, void*) {}
extern "C" void pxa_esp_surface_set_fill_bands_callback(
    pxa_esp_surface_fill_bands_fn, void*) {}

int ppa_register_client(const ppa_client_config_t*, ppa_client_handle_t* handle) {
    *handle = reinterpret_cast<void*>(1);
    return 0;
}
int ppa_do_scale_rotate_mirror(ppa_client_handle_t, const ppa_srm_oper_config_t* c) {
    ++g_ppa_calls;
    if (g_ppa_fail) return -1;
    assert(c->mode == PPA_TRANS_MODE_BLOCKING && c->rotation_angle == PPA_SRM_ROTATION_ANGLE_0);
    assert(c->out.buffer_size >= c->out.pic_w * c->out.pic_h * 2);
    const auto* source = static_cast<const uint16_t*>(c->in.buffer);
    auto* target = static_cast<uint16_t*>(c->out.buffer);
    for (uint32_t y = 0; y < c->out.pic_h; ++y) {
        for (uint32_t x = 0; x < c->out.pic_w; ++x) {
            const uint32_t sx = x / static_cast<unsigned>(c->scale_x);
            const uint32_t sy = y / static_cast<unsigned>(c->scale_y);
            assert(sx < c->in.block_w && sy < c->in.block_h);
            const uint16_t color = source[sy * c->in.pic_w + sx];
            target[y * c->out.pic_w + x] = c->byte_swap ? __builtin_bswap16(color) : color;
        }
    }
    return 0;
}
int ppa_do_fill(ppa_client_handle_t, const ppa_fill_oper_config_t* c) {
    if (g_ppa_fail) return -1;
    const uint16_t color = ((c->fill_argb_color.r >> 3) << 11) |
        ((c->fill_argb_color.g >> 2) << 5) | (c->fill_argb_color.b >> 3);
    auto* target = static_cast<uint16_t*>(c->out.buffer);
    for (uint32_t y = 0; y < c->fill_block_h; ++y)
        for (uint32_t x = 0; x < c->fill_block_w; ++x)
            target[(y + c->out.block_offset_y) * c->out.pic_w + x + c->out.block_offset_x] = color;
    return 0;
}

int main() {
    uint16_t unchanged = 0x1234;
    const lv_area_t area = {0, 0, 0, 0};
    assert(mosaico_pxa_surface::ComposeFlushArea(
        &area, reinterpret_cast<uint8_t*>(&unchanged)) == 0);
    assert(unchanged == 0x1234);
    assert(mosaico_pxa_surface::ComposeFlushArea(nullptr, nullptr) == 0);
    assert(mosaico_pxa_surface::ComposeFlushArea(
        &area, reinterpret_cast<uint8_t*>(&unchanged), 1) == 0);
    std::vector<uint16_t> placed(300 * 100);
    PrepareFrame(placed.data(), 300, 100, 600);
    lv_area_t bounds;
    assert(mosaico_pxa_surface::ResolvePresentArea(&bounds));
    assert(bounds.x1 == 90 && bounds.y1 == 190 && bounds.x2 == 389 && bounds.y2 == 289);
    g_frame.x = 470;
    g_frame.y = 470;
    assert(mosaico_pxa_surface::ResolvePresentArea(&bounds));
    assert(bounds.x1 == 470 && bounds.y1 == 470 && bounds.x2 == 479 && bounds.y2 == 479);
    g_frame.x = 480;
    assert(!mosaico_pxa_surface::ResolvePresentArea(&bounds));
    g_has_frame = false;
    TestAreaAlignment();
    TestDirectEligibility();
    TestOptimizedComposition();
    TestPpaDirectAndFallback();
    TestPpaAlphaComposition();
    TestTransferPipeline();
    TestRefreshBatch();
    TestPipelinedSnapshot();
    TestExchangedFramesAndUiTransition();
    TestAudioWriteRecovery();
    TestWakeGestureIsolation();
    TestScanFollowTiming();
    TestTouchRecovery();
    TestDelayedTouchReads();
    TestTouchClockAndIdle();
    TestTouchEvents();
    TestTouchSlots();
    for (uint8_t scale : {1, 2, 4}) TestRgb565(scale);
    TestPlacement();
    TestTrustedOverlays();
    TestClippedOverlayFastPath();
    TestPremultipliedArgb();
    std::puts("ESP-Mosaico DMA pipeline, immutable snapshots, refresh batching, scan-follow timing, touch and compositor tests passed");
}
