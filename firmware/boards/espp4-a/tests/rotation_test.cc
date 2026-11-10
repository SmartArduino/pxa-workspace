// SPDX-License-Identifier: Apache-2.0
#include "espp4_a_rotation.h"
#include <pxa/display_rotation.h>
#include <array>
#include <cassert>
#include <cstring>
#include <cstdio>
struct ppa_client_t {};
static ppa_client_t client;
static unsigned registrations, releases, transactions;
static bool reject_registration, reject_transaction;
extern "C" esp_err_t ppa_register_client(const ppa_client_config_t* config, ppa_client_handle_t* out) {
    ++registrations;
    assert(config->oper_type == PPA_OPERATION_SRM && config->max_pending_trans_num == 1);
    if (reject_registration) return ESP_ERR_NO_MEM;
    *out = &client; return ESP_OK;
}
extern "C" esp_err_t ppa_unregister_client(ppa_client_handle_t value) {
    assert(value == &client); ++releases; return ESP_OK;
}
extern "C" esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t value, const ppa_srm_oper_config_t* op) {
    assert(value == &client); ++transactions;
    assert(op->mode == PPA_TRANS_MODE_BLOCKING && op->scale_x == 1 && op->scale_y == 1);
    assert(op->in.srm_cm == PPA_SRM_COLOR_MODE_RGB565 && op->out.srm_cm == PPA_SRM_COLOR_MODE_RGB565);
    assert(op->out.buffer_size == op->out.pic_w * op->out.pic_h * 2);
    if (reject_transaction) return ESP_FAIL;
    const unsigned clockwise = (4 - unsigned(op->rotation_angle)) & 3;
    assert(op->in.pic_w == pxa_display_logical_width(op->out.pic_w, op->out.pic_h, clockwise));
    assert(op->in.pic_h == pxa_display_logical_height(op->out.pic_w, op->out.pic_h, clockwise));
    assert(op->in.block_w == op->in.pic_w && op->in.block_h == op->in.pic_h);
    pxa_display_rgb565_to_native(static_cast<const uint16_t*>(op->in.buffer),
        static_cast<uint16_t*>(op->out.buffer), op->out.pic_w, op->out.pic_h, clockwise);
    return ESP_OK;
}
int main() {
    std::array<uint16_t, 48 * 32 + 2> source{}, output{}, expected{};
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint16_t(i * 1009);
    for (unsigned failure = 0; failure < 3; ++failure) {
        registrations = releases = transactions = 0;
        reject_registration = failure == 1; reject_transaction = failure == 2;
        {
            espp4_a::RotationEngine engine;
            for (uint8_t rotation = 0; rotation < 4; ++rotation) {
                output.fill(0xabcd); expected.fill(0xabcd);
                const bool hardware = engine.Rotate(source.data()+1,output.data()+1,48,32,rotation);
                pxa_display_rgb565_to_native(source.data()+1,expected.data()+1,48,32,rotation);
                assert(output == expected);
                assert(hardware == (rotation != 0 && failure == 0));
            }
            assert(registrations == 1 && transactions == (failure == 0 ? 3u : failure == 2 ? 1u : 0u));
            auto stats = engine.TakeStats();
            assert(stats.hardware == (failure ? 0u : 3u) && stats.software == (failure ? 3u : 0u));
            stats = engine.TakeStats(); assert(!stats.hardware && !stats.software);
            assert(!engine.Rotate(nullptr,output.data(),48,32,1));
            assert(!engine.Rotate(source.data(),source.data(),48,32,1));
            assert(!engine.Rotate(source.data(),output.data(),48,32,4));
            assert(!engine.Rotate(source.data(),output.data(),UINT32_MAX,UINT32_MAX,1));
        }
        assert(releases == (failure == 1 ? 0u : 1u));
    }
    std::puts("PASS: clockwise mapping, blocking RGB565 DMA contract, guards, reuse and software fallback");
}
