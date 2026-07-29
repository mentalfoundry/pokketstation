@ Timing-loop primitives and the real-BIOS-call measurement functions used
@ by experiments 1-4 (experiments.s). See ../README.md for the "Timer0
@ wraps at 0x10000" finding behind the 16-bit masking in every function
@ here that computes a delta.
    .syntax unified
    .arm

    .include "constants.inc"

    .section .text, "ax"
    .global memcpy_words
    .global measure_loop_ptr
    .global run_diagnostic_single_call
    .global measure_bios_call_loop_real
    .global measure_bios_call_loop_wram
    .global measure_bios_thumb_call_loop_real
    .global measure_bios_thumb_call_loop_wram

memcpy_words:            @ r0=dst, r1=src, r2=count(words)
    push {r3, lr}
mcw_loop:
    cmp r2, #0
    beq mcw_done
    ldr r3, [r1], #4
    str r3, [r0], #4
    sub r2, r2, #1
    b mcw_loop
mcw_done:
    pop {r3, lr}
    bx lr
    .ltorg

measure_loop_ptr:         @ r0=ptr, r1=count -> r0=delta (before-after)
    push {r4, r5, lr}
    mov r5, r0
    ldr r4, =TIMER0_COUNT
    ldr r2, [r4]
ml_loop:
    ldr r3, [r5]
    subs r1, r1, #1
    bne ml_loop
    ldr r3, [r4]
    sub r0, r2, r3
    mov r0, r0, lsl #16
    mov r0, r0, lsr #16
    pop {r4, r5, lr}
    bx lr
    .ltorg

@ Diagnostic: raw Timer0 before/after ONE isolated real BIOS ARM-helper
@ call (not a 30000-iteration loop) - stashes raw snapshots, not a delta,
@ so the actual register values can be inspected directly on screen 5.
run_diagnostic_single_call:
    push {r4, r5, lr}
    ldr r4, =TIMER0_COUNT
    ldr r2, [r4]
    ldr r5, =WRAM_DIAG_SINGLE_BEFORE
    str r2, [r5]

    adr lr, diag_single_ret
    ldr r5, =BIOS_ARM_HELPER
    bx r5
diag_single_ret:
    ldr r4, =TIMER0_COUNT
    ldr r3, [r4]
    ldr r5, =WRAM_DIAG_SINGLE_AFTER
    str r3, [r5]
    pop {r4, r5, lr}
    bx lr
    .ltorg

measure_bios_call_loop_real:   @ r1=count -> r0=delta
    @ NOTE: a plain "BL 0x04001BC8" is out of ARM's +-32MB branch range from
    @ this code's position in FLASH1 (BIOS is ~32MB+ away). Use a
    @ register-indirect call (LDR address + ADR return + BX) instead, which
    @ works regardless of distance/placement.
    push {r4, r5, lr}
    ldr r4, =TIMER0_COUNT
    ldr r2, [r4]
    ldr r5, =WRAM_DIAG_BEFORE
    str r2, [r5]
mbclr_loop:
    adr lr, mbclr_ret
    ldr r5, =BIOS_ARM_HELPER
    bx r5
mbclr_ret:
    subs r1, r1, #1
    bne mbclr_loop
    ldr r3, [r4]
    ldr r5, =WRAM_DIAG_AFTER
    str r3, [r5]
    sub r0, r2, r3
    mov r0, r0, lsl #16
    mov r0, r0, lsr #16
    pop {r4, r5, lr}
    bx lr
    .ltorg

measure_bios_call_loop_wram:   @ r1=count -> r0=delta
    @ Same register-indirect call technique as measure_bios_call_loop_real,
    @ for consistency (a plain BL to 0x224 happens to be in-range from most
    @ FLASH1 offsets, but keeping both call sites structurally identical
    @ matters more than exploiting that here).
    push {r4, r5, lr}
    ldr r4, =TIMER0_COUNT
    ldr r2, [r4]
mbclw_loop:
    adr lr, mbclw_ret
    @ Deliberately NOT "ldr r5, =WRAM_ARM_MIRROR": GNU as's "=expr"
    @ pseudo-op silently optimizes small constants (0x224 fits an ARM
    @ rotated immediate) into a plain MOV, skipping the memory load
    @ entirely - unlike measure_bios_call_loop_real's BIOS_ARM_HELPER
    @ (0x04001BC8, too large to optimize), which always stays a genuine
    @ literal-pool LDR. That asymmetry would make this loop structurally
    @ CHEAPER than the real-BIOS loop for reasons having nothing to do
    @ with the actual memory-region timing being measured - caught by
    @ comparing this build's emulator results against the known-good
    @ reference build before ever touching real hardware. Loading from an
    @ explicit named literal (not the auto-optimizing pseudo-op) forces a
    @ real LDR every time, keeping both call sites instruction-for-
    @ instruction identical, differing only in the target value.
    ldr r5, mbclw_wram_addr
    bx r5
mbclw_ret:
    subs r1, r1, #1
    bne mbclw_loop
    ldr r3, [r4]
    sub r0, r2, r3
    mov r0, r0, lsl #16
    mov r0, r0, lsr #16
    pop {r4, r5, lr}
    bx lr
    .ltorg
mbclw_wram_addr:
    .word WRAM_ARM_MIRROR

measure_bios_thumb_call_loop_real:   @ r1=count -> r0=delta
    push {r4, r5, lr}
    ldr r4, =TIMER0_COUNT
    ldr r2, [r4]
mbtclr_loop:
    mov r0, #0
    adr lr, mbtclr_ret
    ldr r5, =BIOS_THUMB_HELPER_BIT1
    bx r5
mbtclr_ret:
    subs r1, r1, #1
    bne mbtclr_loop
    ldr r3, [r4]
    sub r0, r2, r3
    mov r0, r0, lsl #16
    mov r0, r0, lsr #16
    pop {r4, r5, lr}
    bx lr
    .ltorg

measure_bios_thumb_call_loop_wram:   @ r1=count -> r0=delta
    push {r4, r5, lr}
    ldr r4, =TIMER0_COUNT
    ldr r2, [r4]
mbtclw_loop:
    mov r0, #0
    adr lr, mbtclw_ret
    ldr r5, =(WRAM_THUMB_MIRROR | 1)
    bx r5
mbtclw_ret:
    subs r1, r1, #1
    bne mbtclw_loop
    ldr r3, [r4]
    sub r0, r2, r3
    mov r0, r0, lsl #16
    mov r0, r0, lsr #16
    pop {r4, r5, lr}
    bx lr
    .ltorg
