/* Exact eight-pixel S31 PIE ARGB8888 -> RGB565 kernel.
 * Ported from esp32s31-lvgltest round 5; see docs/performance for provenance.
 * Call only on a task pinned to Core1. CFG/SAR are preserved. */
.option arch, +xespv
.text
.macro korvo_pie_blend8
    /* Blue channel, 0..31. */
    la a4, .Lpie_31
    esp.vldbc.16.ip q6, a4, 0
    esp.andq q3, q0, q6
    esp.andq q5, q2, q6
    esp.vsub.s16 q5, q5, q3
    esp.vmul.s16 q5, q5, q1
    esp.vadd.s16 q4, q3, q5

    /* Green channel, 0..63. */
    la a4, .Lpie_minus_five
    esp.vldbc.16.ip q7, a4, 0
    esp.vsld.16 q3, q0, q7
    esp.vsld.16 q5, q2, q7
    la a4, .Lpie_63
    esp.vldbc.16.ip q6, a4, 0
    esp.andq q3, q3, q6
    esp.andq q5, q5, q6
    esp.vsub.s16 q5, q5, q3
    esp.vmul.s16 q5, q5, q1
    esp.vadd.s16 q3, q3, q5
    la a4, .Lpie_five
    esp.vldbc.16.ip q7, a4, 0
    esp.vsld.16 q3, q3, q7
    esp.orq q4, q4, q3

    /* Red channel, 0..31. */
    la a4, .Lpie_minus_eleven
    esp.vldbc.16.ip q7, a4, 0
    esp.vsld.16 q3, q0, q7
    esp.vsld.16 q5, q2, q7
    la a4, .Lpie_31
    esp.vldbc.16.ip q6, a4, 0
    esp.andq q3, q3, q6
    esp.andq q5, q5, q6
    esp.vsub.s16 q5, q5, q3
    esp.vmul.s16 q5, q5, q1
    esp.vadd.s16 q3, q3, q5
    /* VSLD.16 saturates signed left shifts. Red can set bit 15, so use an
     * unsigned multiply with SAR=0 to pack it without signed saturation. */
    li a4, 0
    esp.movx.w.sar a4
    la a4, .Lpie_2048
    esp.vldbc.16.ip q7, a4, 0
    esp.vmul.u16 q3, q3, q7
    li a4, 5
    esp.movx.w.sar a4
    esp.orq q4, q4, q3
.endm
.balign 4
.global korvo_pie_argb_rows
.type korvo_pie_argb_rows, @function
/* dest, straight ARGB8888 source, width8, height, dest_stride, src_stride, opa */
korvo_pie_argb_rows:
    addi sp, sp, -16
    sw a6, 0(sp)
    slli t2, a2, 1
    sub t2, a4, t2
    slli t3, a2, 2
    sub t3, a5, t3
    esp.movx.r.cfg a4
    mv t0, a4
    esp.movx.r.sar a4
    mv t1, a4
    li a4, 3
    esp.movx.w.cfg a4
    mv t6, a7
    li t5, 253
.Largb_row:
    mv t4, a2
.Largb_block:
    esp.vld.128.ip q5, a1, 16
    esp.vld.128.ip q6, a1, 16
    esp.vunzip.16 q5, q6
    la a4, .Lpie_minus_eight
    esp.vldbc.16.ip q7, a4, 0
    esp.vsld.16 q1, q6, q7
    la a4, .Lpie_255
    esp.vldbc.16.ip q4, a4, 0
    esp.andq q1, q1, q4
    bgeu a6, t5, .Largb_alpha_ready
    li a4, 8
    esp.movx.w.sar a4
    mv a4, sp
    esp.vldbc.16.ip q7, a4, 0
    esp.vmul.u16 q1, q1, q7    /* LV_OPA_MIX2(alpha, opa), floor / 256 */
.Largb_alpha_ready:
    la a4, .Lpie_four
    esp.vldbc.16.ip q7, a4, 0
    esp.vadd.u16 q1, q1, q7
    la a4, .Lpie_minus_three
    esp.vldbc.16.ip q7, a4, 0
    esp.vsld.16 q1, q1, q7
    /* Hardware reductions classify all eight quantized alpha lanes. */
    esp.max.u16.a q1, a4
    beqz a4, .Largb_transparent
    mv a5, a4
    esp.min.u16.a q1, a4
    li a7, 32
    beq a4, a7, .Largb_class_opaque
    xori a7, a4, 16
    xori a5, a5, 16
    or a7, a7, a5
    seqz a7, a7
    slli a7, a7, 1           /* 2: every lane is exactly half alpha */
    j .Largb_class_ready
.Largb_class_opaque:
    li a7, 1
.Largb_class_ready:
    la a4, .Lpie_minus_three
    esp.vldbc.16.ip q7, a4, 0
    esp.vsld.16 q2, q5, q7
    esp.vsld.16 q3, q6, q7
    la a4, .Lpie_31
    esp.vldbc.16.ip q4, a4, 0
    esp.andq q2, q2, q4        /* blue */
    esp.andq q3, q3, q4        /* red, unpacked */
    li a4, 0
    esp.movx.w.sar a4
    la a4, .Lpie_2048
    esp.vldbc.16.ip q7, a4, 0
    esp.vmul.u16 q3, q3, q7
    esp.orq q2, q2, q3
    la a4, .Lpie_minus_five
    esp.vldbc.16.ip q7, a4, 0
    esp.vsld.16 q3, q5, q7
    la a4, .Lpie_green_mask
    esp.vldbc.16.ip q4, a4, 0
    esp.andq q3, q3, q4
    esp.orq q2, q2, q3
    li a4, 1
    beq a7, a4, .Largb_opaque
    bnez a7, .Largb_half
    li a4, 5
    esp.movx.w.sar a4
    esp.vld.128.ip q0, a0, 0
    korvo_pie_blend8
    esp.vst.128.ip q4, a0, 16
    j .Largb_next
.Largb_half:
    /* Packed floor((fg_channel + bg_channel) / 2), exactly matching a5=16:
     * ((fg ^ bg) & 0xf7de) / 2 + (fg & bg). Clear each channel's LSB
     * before the unsigned divide, so bits never cross RGB565 channels. */
    esp.vld.128.ip q0, a0, 0
    esp.xorq q4, q0, q2
    la a4, .Lpie_half_mask
    esp.vldbc.16.ip q7, a4, 0
    esp.andq q4, q4, q7
    esp.andq q3, q0, q2
    li a4, 1
    esp.movx.w.sar a4
    la a4, .Lpie_one
    esp.vldbc.16.ip q7, a4, 0
    esp.vmul.u16 q4, q4, q7
    esp.vadd.u16 q4, q4, q3
    esp.vst.128.ip q4, a0, 16
    j .Largb_next
.Largb_opaque:
    esp.vst.128.ip q2, a0, 16
    j .Largb_next
.Largb_transparent:
    addi a0, a0, 16
.Largb_next:
    addi t4, t4, -8
    bnez t4, .Largb_block
    add a0, a0, t2
    add a1, a1, t3
    addi a3, a3, -1
    bnez a3, .Largb_row
    mv a4, t1
    esp.movx.w.sar a4
    mv a4, t0
    esp.movx.w.cfg a4
    addi sp, sp, 16
    ret
.size korvo_pie_argb_rows, .-korvo_pie_argb_rows
.section .rodata
.balign 16
.Lpie_31:          .short 31
.Lpie_63:          .short 63
.Lpie_four:        .short 4
.Lpie_minus_three: .short -3
.Lpie_minus_five:  .short -5
.Lpie_five:        .short 5
.Lpie_minus_eleven:.short -11
.Lpie_2048:        .short 2048

.Lpie_minus_eight: .short -8
.Lpie_255: .short 255
.Lpie_green_mask: .short 0x07e0

.Lpie_half_mask: .short 0xf7de
.Lpie_one: .short 1
