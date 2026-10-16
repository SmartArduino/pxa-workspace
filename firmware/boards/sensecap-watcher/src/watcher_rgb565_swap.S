// ESP32-S3 PIE: unzip the low/high bytes, then zip in reversed order.
// Thread context only. Pointers must be 16-byte aligned, length a positive
// multiple of 32 bytes. The C++ caller handles other alignments and tails.
    .text
    .align 4
    .global watcher_rgb565_swap_aligned
    .type watcher_rgb565_swap_aligned,@function
watcher_rgb565_swap_aligned:
    entry a1, 32
    srli a4, a4, 5
    loopnez a4, .Lswap_done
        ee.vld.128.ip q0, a3, 16
        ee.vld.128.ip q1, a3, 16
        ee.vunzip.8 q0, q1
        ee.vzip.8 q1, q0
        ee.vst.128.ip q1, a2, 16
        ee.vst.128.ip q0, a2, 16
.Lswap_done:
    retw.n
    .size watcher_rgb565_swap_aligned, .-watcher_rgb565_swap_aligned
