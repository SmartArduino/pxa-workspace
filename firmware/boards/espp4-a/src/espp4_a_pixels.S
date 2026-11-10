/* SPDX-License-Identifier: MIT
 * Based on firmware/components/pxa_render_accel/pixels_s31.S (MIT).
 * ESP32-P4 v3 PIE kernels. Whole aligned blocks only, task context only.
 * No SAR, rounding mode, scalar callee-saved register or HW loop state changes.
 */
#include "sdkconfig.h"
#if !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    .text
    .align 4
    .global espp4_a_fill_blocks
    .type espp4_a_fill_blocks, @function
espp4_a_fill_blocks:
    esp.vldbc.16.ip q0, a2, 0
1:  esp.vst.128.ip q0, a0, 16
    addi a1, a1, -1
    bnez a1, 1b
    ret
    .size espp4_a_fill_blocks, .-espp4_a_fill_blocks

    .align 4
    .global espp4_a_expand_blocks
    .type espp4_a_expand_blocks, @function
espp4_a_expand_blocks:
1:  esp.vldhbc.16.incp q0, q1, a1
    li t0, 4
    bne a3, t0, 2f
    esp.orq q2, q0, q0
    esp.orq q3, q1, q1
    esp.vzip.16 q0, q2
    esp.vzip.16 q1, q3
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q2, a0, 16
    esp.vst.128.ip q1, a0, 16
    esp.vst.128.ip q3, a0, 16
    j 3f
2:  esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q1, a0, 16
3:  addi a2, a2, -1
    bnez a2, 1b
    ret
    .size espp4_a_expand_blocks, .-espp4_a_expand_blocks
#endif
