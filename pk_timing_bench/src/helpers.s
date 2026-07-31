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
    .global measure_loop_ptr_store
    .global run_diagnostic_single_call
    .global measure_bios_call_loop_real
    .global measure_bios_call_loop_wram
    .global measure_bios_thumb_call_loop_real
    .global measure_bios_thumb_call_loop_wram
    .global measure_timer_periods
    .global irq_ack_handler
    .global irq_rearm_handler
    .global irq_rearm_handler_t2

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

@ Same loop as measure_loop_ptr, but stores instead of loading. Used by
@ experiment 9 to measure a real MMIO write's cost, since a real transmit
@ handler's hot-path IRDA_DATA access is a store (the LED bit), not a load.
@ Stores a constant 0 every iteration: this measures the bus/register access
@ cost, not any behavior the stored value itself might trigger.
measure_loop_ptr_store:   @ r0=ptr, r1=count -> r0=delta (before-after)
    push {r4, r5, lr}
    mov r5, r0
    mov r3, #0
    ldr r4, =TIMER0_COUNT
    ldr r2, [r4]
mls_loop:
    str r3, [r5]
    subs r1, r1, #1
    bne mls_loop
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

@ Measures how long Timer2 REALLY takes to complete r1 whole periods when
@ armed with period r0, using Timer0 as the stopwatch. Returns the elapsed
@ Timer0 tick count in r0, 16-bit masked. Timer0 wraps at 0x10000 on real
@ hardware. See ../README.md.
@
@ No interrupts are involved anywhere here: Timer2's reloads are detected by
@ polling its count register, so this measures the timer block's own period
@ behavior in isolation. That is the whole point.
@ It separates "a timer armed with period P really does take P ticks" from any
@ cost that appears only once an interrupt is actually taken.
@
@ Timer2 counts DOWN and reloads to `period`, so a reload is detected as the
@ count reading HIGHER than the previous sample. The loop samples far faster
@ than the shortest period tested (period 1016 at /2 is ~2032 raw cycles,
@ versus a handful of cycles per poll iteration), so no reload can be missed.
@
@ This saves and restores Timer2's original period, count, and control, so it
@ leaves the timer exactly as it found it.
@ The real BIOS may still rely on that timer after this app exits to the system.
measure_timer_periods:     @ r0=period, r1=reload count -> r0=Timer0 delta
    push {r4, r5, r6, r7, r8, r9, r10, lr}
    ldr r4, =TIMER2_BASE

    @ save original Timer2 state
    ldr r8, [r4]
    ldr r9, [r4, #4]
    ldr r10, [r4, #8]

    mov r6, #0
    str r6, [r4, #8]                  @ stop before reprogramming
    str r0, [r4]                      @ period
    str r0, [r4, #4]                  @ prime count to a full period
    mov r6, #TIMER2_CTRL_DIV2_ENABLE
    str r6, [r4, #8]

    ldr r7, =TIMER0_COUNT

    @ Sync to a reload edge first, so the first period counted is a whole one
    @ rather than however much of a period happened to be left when we armed it.
    ldr r5, [r4, #4]
mtp_sync:
    ldr r6, [r4, #4]
    cmp r6, r5
    mov r5, r6                        @ MOV without S leaves the CMP flags intact
    bls mtp_sync

    ldr r2, [r7]                      @ stopwatch: Timer0 before

mtp_count:
    ldr r6, [r4, #4]
    cmp r6, r5
    mov r5, r6
    bls mtp_count
    subs r1, r1, #1
    bne mtp_count

    ldr r3, [r7]                      @ stopwatch: Timer0 after
    sub r0, r2, r3                    @ Timer0 counts down, so subtract after from before
    mov r0, r0, lsl #16
    mov r0, r0, lsr #16

    @ restore original Timer2 state
    mov r6, #0
    str r6, [r4, #8]
    str r8, [r4]
    str r9, [r4, #4]
    str r10, [r4, #8]

    pop {r4, r5, r6, r7, r8, r9, r10, lr}
    bx lr
    .ltorg

@ Minimal IRQ handler for experiment 7. Registered via SWI 1 (see
@ register_irq_handler below) and called by the kernel as an ordinary ARM
@ function, matching the calling convention a real app's own dispatcher uses
@ (push/.../pop {..., pc}).
@
@ It does the least possible work: acknowledge every currently-pending source
@ and return. Experiment 7 does not care what the handler DOES - it measures
@ how long the round trip into and out of it costs.
@ A trivial body therefore keeps the entry and dispatch path dominant in the
@ measurement.
@
@ Acknowledging matters: INTC's IRQ line is level-driven off HOLD, so leaving a
@ source pending would re-enter this handler forever and wedge the app.
irq_ack_handler:
    push {r0, r1, lr}
    ldr r0, =INTC_BASE
    ldr r1, [r0]                      @ HOLD: everything currently asserted
    str r1, [r0, #0x10]               @ acknowledge (+0x10) clears hold+status
    pop {r0, r1, pc}
    .ltorg

@ IRQ handler for experiment 8. It re-arms Timer1 with the same period it was
@ given, then counts the interrupt.
@
@ Re-arming inside the handler is the whole point. It copies what a real
@ IR-using app does on every one of its transmit interrupts, and it is what
@ makes the expiry-to-re-arm latency observable: the next period does not start
@ until this handler reaches the re-arm, so any latency before that point adds
@ to the measured period instead of cancelling out.
irq_rearm_handler:
    push {r0, r1, lr}
    ldr r0, =INTC_BASE
    ldr r1, [r0]
    str r1, [r0, #0x10]               @ acknowledge everything pending

    ldr r0, =TIMER1_BASE              @ re-arm with the same period
    ldr r1, =EXP8_TIMER_PERIOD
    str r1, [r0]
    str r1, [r0, #4]

    ldr r0, =WRAM_REARM_COUNTER
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    pop {r0, r1, pc}
    .ltorg

@ IRQ handler for experiment 10. Identical to irq_rearm_handler above, except
@ it re-arms TIMER2_BASE and counts into a separate WRAM slot. Timer2 is the
@ FIQ-routed timer (INT_FIQ_MASK), so the same "register once, un-mask, take
@ N interrupts, measure" shape as experiment 8 exercises the FIQ path instead
@ of IRQ - the real IR transmit handler's actual exception type. See
@ experiments.s's run_experiment_10_fiq_rearm_latency.
irq_rearm_handler_t2:
    push {r0, r1, lr}
    ldr r0, =INTC_BASE
    ldr r1, [r0]
    str r1, [r0, #0x10]               @ acknowledge everything pending

    ldr r0, =TIMER2_BASE               @ re-arm with the same period
    ldr r1, =EXP8_TIMER_PERIOD
    str r1, [r0]
    str r1, [r0, #4]

    ldr r0, =WRAM_REARM2_COUNTER
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    pop {r0, r1, pc}
    .ltorg
