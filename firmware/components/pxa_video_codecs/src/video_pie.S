/* SPDX-License-Identifier: Apache-2.0
 * ESP32-P4 rev3 PIE leaf kernels. No hardware-loop or QACC changes.
 * Vector registers are caller-saved. CFG and SAR changes are restored.
 */
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S31 || (CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
    .text
    .align 4
    .global pxa_video_pie_pack_row
    .type pxa_video_pie_pack_row, @function
/* a0=luma, a1=chroma, a2=output, a3=32-pixel blocks. All aligned to 16. */
pxa_video_pie_pack_row:
1:  esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a0, 16
    esp.vld.128.ip q2, a1, 16
    esp.vunzip.8 q0, q1
    esp.vzipt.8 q2, q0, q1
    esp.vst.128.ip q2, a2, 16
    esp.vst.128.ip q0, a2, 16
    esp.vst.128.ip q1, a2, 16
    addi a3, a3, -1
    bnez a3, 1b
    ret
    .size pxa_video_pie_pack_row, .-pxa_video_pie_pack_row

    .align 4
    .global pxa_video_pie_half444_row
    .type pxa_video_pie_half444_row, @function
/* Y(2x), U(x), V(x) -> RGB-converter YUV444 triplets, 16 pixels per block. */
pxa_video_pie_half444_row:
1:  esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a0, 16
    esp.vunzip.8 q0, q1
    esp.vld.128.ip q1, a1, 16
    esp.vld.128.ip q2, a2, 16
    esp.vzipt.8 q0, q1, q2
    esp.vst.128.ip q0, a3, 16
    esp.vst.128.ip q1, a3, 16
    esp.vst.128.ip q2, a3, 16
    addi a4, a4, -1
    bnez a4, 1b
    ret
    .size pxa_video_pie_half444_row, .-pxa_video_pie_half444_row
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    .text

    .align 4
    .global pxa_video_pie_average16
    .type pxa_video_pie_average16, @function
/* Same eight arguments as OpenH264 PixelAvg; width is exactly 16.
 * ceil((a+b)/2) = (a|b) - ((a^b)>>1), avoiding saturated byte addition.
 * Hardware misalignment handles every source/destination offset exactly.
 */
pxa_video_pie_average16:
    esp.movx.r.cfg t3
    andi t4, t3, -241
    ori t4, t4, 3
    esp.movx.w.cfg t4
    esp.zero.q q3
    esp.notq q3, q3
    li t5, 0x7f7f7f7f
    esp.movi.32.q q4, t5, 0
    esp.movi.32.q q4, t5, 1
    esp.movi.32.q q4, t5, 2
    esp.movi.32.q q4, t5, 3
1:  esp.vld.128.ip q0, a2, 0
    esp.vld.128.ip q1, a4, 0
    esp.xorq q2, q0, q1
    esp.orq q0, q0, q1
    esp.vsld.8 q2, q2, q3
    esp.andq q2, q2, q4
    esp.vsub.u8 q0, q0, q2
    esp.vst.128.ip q0, a0, 0
    add a0, a0, a1
    add a2, a2, a3
    add a4, a4, a5
    addi a7, a7, -1
    bnez a7, 1b
    esp.movx.w.cfg t3
    ret
    .size pxa_video_pie_average16, .-pxa_video_pie_average16

    .align 4
    .global pxa_video_pie_halfpel
    .type pxa_video_pie_halfpel, @function
/* src, src_stride, dst, dst_stride, width (8 or 16), height, tap_step.
 * AVC's exact six-tap filter, eight signed 16-bit lanes at a time.
 * Reference pictures have OpenH264's border padding, including vector loads.
 */
pxa_video_pie_halfpel:
    addi sp, sp, -16
    sw s0, 0(sp)
    sw s1, 4(sp)
    sw s2, 8(sp)
    esp.movx.r.cfg t3
    sw t3, 12(sp)
    andi t4, t3, -241
    ori t4, t4, 3
    esp.movx.w.cfg t4
    la t3, .Lpxa_video_pie_constants
    esp.vld.128.ip q4, t3, 16
    esp.vld.128.ip q5, t3, 16
    esp.vld.128.ip q6, t3, 16
    esp.vld.128.ip q7, t3, 16
    li t4, 16
    mv s0, a0
    mv s1, a2
    sub a1, a1, a4
    sub a3, a3, a4
1:  mv s2, a4
2:  slli t0, a6, 1
    sub t6, s0, t0
    esp.vld.l.64.ip q1, t6, 0
    esp.vext.u8 q1, q3, q1
    add t0, t0, a6
    add t6, s0, t0
    esp.vld.l.64.ip q2, t6, 0
    esp.vext.u8 q2, q3, q2
    esp.vadd.s16 q0, q1, q2
    sub t6, s0, a6
    esp.vld.l.64.ip q1, t6, 0
    esp.vext.u8 q1, q3, q1
    slli t0, a6, 1
    add t6, s0, t0
    esp.vld.l.64.ip q2, t6, 0
    esp.vext.u8 q2, q3, q2
    esp.vadd.s16 q1, q1, q2
    esp.vsld.16 q2, q1, q4
    esp.vadd.s16 q1, q1, q2
    esp.vsub.s16 q0, q0, q1
    mv t6, s0
    esp.vld.l.64.ip q1, t6, 0
    esp.vext.u8 q1, q3, q1
    add t6, s0, a6
    esp.vld.l.64.ip q2, t6, 0
    esp.vext.u8 q2, q3, q2
    esp.vadd.s16 q1, q1, q2
    esp.vsld.16 q2, q1, q4
    esp.vsld.16 q1, q1, q5
    esp.vadd.s16 q1, q1, q2
    esp.vadd.s16 q0, q0, q1
    esp.vsadds.s16 q0, q0, t4
    esp.vsld.16 q0, q0, q6
    esp.zero.q q1
    esp.vmax.s16 q0, q0, q1
    esp.vmin.s16 q0, q0, q7
    esp.vunzip.8 q0, q1
    mv t5, s1
    esp.vst.l.64.ip q0, t5, 0
    addi s0, s0, 8
    addi s1, s1, 8
    addi s2, s2, -8
    bnez s2, 2b
    add s0, s0, a1
    add s1, s1, a3
    addi a5, a5, -1
    bnez a5, 1b
    lw t3, 12(sp)
    esp.movx.w.cfg t3
    lw s0, 0(sp)
    lw s1, 4(sp)
    lw s2, 8(sp)
    addi sp, sp, 16
    ret
    .size pxa_video_pie_halfpel, .-pxa_video_pie_halfpel

    .align 4
    .global pxa_video_pie_chroma8
    .type pxa_video_pie_chroma8, @function
/* src, src_stride, dst, dst_stride, four u8 weights, height. Width = 8. */
pxa_video_pie_chroma8:
    addi sp, sp, -16
    esp.movx.r.cfg t3
    sw t3, 0(sp)
    andi t4, t3, -241
    ori t4, t4, 3
    esp.movx.w.cfg t4
    esp.movx.r.sar t3
    sw t3, 4(sp)
    li t3, 0
    esp.movx.w.sar t3
    esp.zero.q q1
    lbu t3, 0(a4)
    esp.vsadds.s16 q4, q1, t3
    lbu t3, 1(a4)
    esp.vsadds.s16 q5, q1, t3
    lbu t3, 2(a4)
    esp.vsadds.s16 q6, q1, t3
    lbu t3, 3(a4)
    esp.vsadds.s16 q7, q1, t3
    li t3, -6
    esp.vsadds.s16 q3, q1, t3
    li t4, 32
1:  mv t6, a0
    esp.vld.l.64.ip q1, t6, 0
    esp.vext.u8 q1, q2, q1
    esp.vmul.u16 q0, q1, q4
    addi t6, a0, 1
    esp.vld.l.64.ip q1, t6, 0
    esp.vext.u8 q1, q2, q1
    esp.vmul.u16 q1, q1, q5
    esp.vadd.s16 q0, q0, q1
    add t6, a0, a1
    esp.vld.l.64.ip q1, t6, 0
    esp.vext.u8 q1, q2, q1
    esp.vmul.u16 q1, q1, q6
    esp.vadd.s16 q0, q0, q1
    add t6, a0, a1
    addi t6, t6, 1
    esp.vld.l.64.ip q1, t6, 0
    esp.vext.u8 q1, q2, q1
    esp.vmul.u16 q1, q1, q7
    esp.vadd.s16 q0, q0, q1
    esp.vsadds.s16 q0, q0, t4
    esp.vsld.16 q0, q0, q3
    esp.zero.q q1
    esp.vunzip.8 q0, q1
    esp.vst.l.64.ip q0, a2, 0
    add a0, a0, a1
    add a2, a2, a3
    addi a5, a5, -1
    bnez a5, 1b
    lw t3, 4(sp)
    esp.movx.w.sar t3
    lw t3, 0(sp)
    esp.movx.w.cfg t3
    addi sp, sp, 16
    ret
    .size pxa_video_pie_chroma8, .-pxa_video_pie_chroma8

    .section .rodata.pxa_video_pie_constants,"a",@progbits
    .align 4
.Lpxa_video_pie_constants:
    .rept 8
    .short 2
    .endr
    .rept 8
    .short 4
    .endr
    .rept 8
    .short -5
    .endr
    .rept 8
    .short 255
    .endr
#endif

#if CONFIG_IDF_TARGET_ESP32S31 && PXA_GAMEKIT_BOX420_360
    .text
/* Rounded unsigned byte average (a|b) - (((a^b)&0xfe)>>1). The mask
 * prevents adjacent byte lanes from spilling across a 32-bit word shift. */
    .macro video_byte_avg dest,left,right
    esp.xorq q4,\left,\right
    esp.andq q4,q4,q7
    esp.vsr.u32 q4,q4
    esp.orq \dest,\left,\right
    esp.vsub.u8 \dest,\dest,q4
    .endm
    .macro video_box16 dest,row0,row1
    esp.vld.128.ip q0,\row0,16
    esp.vld.128.ip q1,\row0,16
    esp.vld.128.ip q2,\row1,16
    esp.vld.128.ip q3,\row1,16
    esp.vunzip.8 q0,q1
    esp.vunzip.8 q2,q3
    video_byte_avg q0,q0,q1
    video_byte_avg q2,q2,q3
    video_byte_avg \dest,q0,q2
    .endm
    .align 4
/* a0/a1: two luma rows; a2/a3: two chroma rows; a4: packed destination;
 * a5: positive count of 64-column blocks, each producing 48 packed bytes. */
    .global pxa_video_pie_box420_row
    .type pxa_video_pie_box420_row,@function
pxa_video_pie_box420_row:
    esp.movx.r.cfg t4
    mv t3,t4
    andi t3,t3,-241
    ori t3,t3,3
    esp.movx.w.cfg t3
    esp.movx.r.sar t5
    li t3,1
    esp.movx.w.sar t3
    la t3,.Lbox420_mask
    esp.vld.128.ip q7,t3,16
1:  video_box16 q5,a0,a1
    video_box16 q6,a0,a1
    esp.vunzip.8 q5,q6
    video_box16 q0,a2,a3
    esp.vzipt.8 q0,q5,q6
    esp.vst.128.ip q0,a4,16
    esp.vst.128.ip q5,a4,16
    esp.vst.128.ip q6,a4,16
    addi a5,a5,-1
    bnez a5,1b
    esp.movx.w.sar t5
    esp.movx.w.cfg t4
    ret
    .size pxa_video_pie_box420_row,.-pxa_video_pie_box420_row
    .section .rodata,"a"
    .align 4
.Lbox420_mask:
    .word 0xfefefefe,0xfefefefe,0xfefefefe,0xfefefefe
#endif
