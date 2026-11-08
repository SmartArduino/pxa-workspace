// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_images.h"
#include <lvgl.h>
#include <lvgl_private.h>
#include <src/image/lv_image_decoder_private.h>
#include <esp_jpeg_dec.h>
#include <algorithm>

namespace {
lv_result_t JpegInfo(lv_image_decoder_t* decoder, lv_image_decoder_dsc_t* session,
                    lv_image_header_t* header) {
    // Core supplies an in-memory, bounded JPEG. Split images and other formats
    // continue through the native decoder with their area callback unchanged.
    if (session->src_type != LV_IMAGE_SRC_VARIABLE || !session->src)
        return LV_RESULT_INVALID;
    const auto* image = static_cast<const lv_image_dsc_t*>(session->src);
    if (!image->data || image->data_size < 3 || image->data[0] != 0xff ||
        image->data[1] != 0xd8 || image->data[2] != 0xff) return LV_RESULT_INVALID;
    auto* native = static_cast<lv_image_decoder_t*>(decoder->user_data);
    const auto result = native->info_cb(native, session, header);
    if (result == LV_RESULT_OK && (header->w > 640 || header->h > 640)) {
        // Decode large photos directly into a bounded display-sized buffer.
        // The source JPEG retains its full resolution on disk/transfer.
        const unsigned longest = std::max(header->w, header->h);
        header->w = std::max(8u, (unsigned(header->w) * 512 / longest) & ~7u);
        header->h = std::max(8u, (unsigned(header->h) * 512 / longest) & ~7u);
        header->cf = LV_COLOR_FORMAT_RGB565;
        header->stride = header->w * 2;
    }
    return result;
}
lv_result_t JpegOpen(lv_image_decoder_t* decoder, lv_image_decoder_dsc_t* session) {
    auto* native = static_cast<lv_image_decoder_t*>(decoder->user_data);
    lv_image_header_t original{};
    if (native->info_cb(native, session, &original) != LV_RESULT_OK) return LV_RESULT_INVALID;
    if (session->header.w == original.w && session->header.h == original.h)
        return native->open_cb(native, session);
    const auto* image = static_cast<const lv_image_dsc_t*>(session->src);
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.scale.width = session->header.w; config.scale.height = session->header.h;
    jpeg_dec_handle_t jpeg = nullptr;
    if (jpeg_dec_open(&config, &jpeg) != JPEG_ERR_OK || !jpeg) return LV_RESULT_INVALID;
    jpeg_dec_io_t io{};
    io.inbuf = const_cast<uint8_t*>(image->data); io.inbuf_len = image->data_size;
    jpeg_dec_header_info_t info{};
    int output_bytes = 0;
    auto result = LV_RESULT_INVALID;
    lv_draw_buf_t* buffer = nullptr;
    if (jpeg_dec_parse_header(jpeg, &io, &info) == JPEG_ERR_OK &&
        jpeg_dec_get_outbuf_len(jpeg, &output_bytes) == JPEG_ERR_OK && output_bytes > 0 &&
        unsigned(output_bytes) <= session->header.w * session->header.h * 2) {
        buffer = lv_draw_buf_create_ex(&LV_GLOBAL_DEFAULT()->image_cache_draw_buf_handlers, session->header.w,
            session->header.h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        if (buffer && (reinterpret_cast<uintptr_t>(buffer->data) & 15) == 0 &&
            unsigned(output_bytes) <= buffer->data_size) {
            io.outbuf = buffer->data;
            if (jpeg_dec_process(jpeg, &io) == JPEG_ERR_OK) {
                session->decoded = buffer; buffer = nullptr; result = LV_RESULT_OK;
            }
        }
    }
    if (buffer) lv_draw_buf_destroy_user(&LV_GLOBAL_DEFAULT()->image_cache_draw_buf_handlers, buffer);
    jpeg_dec_close(jpeg);
    return result;
}
}

bool Espp4AInitializeJpegImages(esp_lv_decoder_handle_t native_decoder) {
    auto* native = static_cast<lv_image_decoder_t*>(native_decoder);
    if (!native || !native->info_cb || !native->open_cb || !native->close_cb) return false;
    auto* jpeg = lv_image_decoder_create();
    if (!jpeg) return false;
    jpeg->name = "espp4_jpeg";
    jpeg->user_data = native;
    lv_image_decoder_set_info_cb(jpeg, JpegInfo);
    lv_image_decoder_set_open_cb(jpeg, JpegOpen);
    lv_image_decoder_set_close_cb(jpeg, native->close_cb);
    // esp_lv_decoder decodes ordinary JPEGs into one complete draw buffer.
    // Its area callback only handles split formats. Expose the full-buffer
    // contract to Core while retaining the supplier's decoder and allocation
    // handlers, including the existing memory-budget hooks.
    return true;
}
