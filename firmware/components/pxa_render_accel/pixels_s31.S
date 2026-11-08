/* SPDX-License-Identifier: MIT
 * S31 PIE kernels. Call only on core 1 with 16-byte-aligned vector buffers.
 * Counts are nonzero groups of eight pixels. Mix saves/restores SAR; all
 * other kernels leave it untouched. No kernel changes rounding state.
 */
    .text
    .align 4
    .global pxa_s31_fill16
    .type pxa_s31_fill16, @function
pxa_s31_fill16:
    esp.vldbc.16.ip q0, a2, 0
1:  esp.vst.128.ip q0, a0, 16
    addi a1, a1, -1
    bnez a1, 1b
    ret
    .size pxa_s31_fill16, .-pxa_s31_fill16

    /* Each channel uses eight unsigned 16-bit lanes. Red and green already
     * contain five zero low bits. Shift blue up by five as well, so every
     * VMUL >> SAR is exact regardless of the caller's rounding mode.
     * Add the preweighted foreground before truncating to RGB565. The blue
     * logical word shift can cross lanes, but its mask removes those bits.
     * a3/a4: foreground red/green * weight / 32; a5: blue * weight. */
    .align 4
    .global pxa_s31_mix16
    .type pxa_s31_mix16, @function
pxa_s31_mix16:
    esp.vldbc.16.ip q4, a2, 0
    addi a2, a2, 2
    esp.vldbc.16.ip q5, a2, 0
    addi a2, a2, 2
    esp.vldbc.16.ip q6, a2, 0
    addi a2, a2, 2
    esp.vldbc.16.ip q7, a2, 0
    /* MOVX accepts only x8..x15. The constants pointer is now dead. */
    esp.movx.r.sar a2
    mv a6, a1
    li a1, 5
    esp.movx.w.sar a1
    mv a1, a6
1:  esp.vld.128.ip q0, a0, 0
    esp.andq q1, q0, q4
    esp.andq q2, q0, q5
    esp.andq q3, q0, q6
    esp.vsl.32 q3, q3
    esp.vmul.u16 q1, q1, q7
    esp.vmul.u16 q2, q2, q7
    esp.vmul.u16 q3, q3, q7
    esp.vsadds.u16 q1, q1, a3
    esp.vsadds.u16 q2, q2, a4
    esp.vsadds.u16 q3, q3, a5
    esp.vsr.u32 q3, q3
    esp.andq q1, q1, q4
    esp.andq q2, q2, q5
    esp.andq q3, q3, q6
    esp.orq q0, q1, q2
    esp.orq q0, q0, q3
    esp.vst.128.ip q0, a0, 16
    addi a1, a1, -1
    bnez a1, 1b
    esp.movx.w.sar a2
    ret
    .size pxa_s31_mix16, .-pxa_s31_mix16

    .align 4
    .global pxa_s31_swap16
    .type pxa_s31_swap16, @function
pxa_s31_swap16:
1:  esp.vld.128.ip q0, a1, 16
    esp.orq q1, q0, q0
    esp.vunzip.8 q0, q1
    esp.vzip.8 q1, q0
    esp.vst.128.ip q1, a0, 16
    addi a2, a2, -1
    bnez a2, 1b
    ret
    .size pxa_s31_swap16, .-pxa_s31_swap16

    .align 4
    .global pxa_s31_expand16
    .type pxa_s31_expand16, @function
pxa_s31_expand16:
1:  esp.vldhbc.16.incp q0, q1, a1
    beqz a4, 2f
    /* Byte permutation preserves all RGB565 bits. Arithmetic lane shifts
     * saturate on this PIE implementation and are unsuitable for byte swap. */
    esp.vunzip.8 q0, q1
    esp.vzip.8 q1, q0
    esp.orq q2, q0, q0
    esp.orq q0, q1, q1
    esp.orq q1, q2, q2
2:  li t0, 4
    bne a3, t0, 3f
    esp.orq q2, q0, q0
    esp.orq q3, q1, q1
    esp.vzip.16 q0, q2
    esp.vzip.16 q1, q3
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q2, a0, 16
    esp.vst.128.ip q1, a0, 16
    esp.vst.128.ip q3, a0, 16
    j 4f
3:  esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q1, a0, 16
4:  addi a2, a2, -1
    bnez a2, 1b
    ret
    .size pxa_s31_expand16, .-pxa_s31_expand16

    .align 4
    .global pxa_s31_equal_blocks
    .type pxa_s31_equal_blocks, @function
pxa_s31_equal_blocks:
    li a3, 0
1:  esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a1, 16
    esp.xorq q2, q0, q1
    esp.max.u32.a q2, a4
    bnez a4, 2f
    addi a3, a3, 1
    bltu a3, a2, 1b
2:  mv a0, a3
    ret
    .size pxa_s31_equal_blocks, .-pxa_s31_equal_blocks

