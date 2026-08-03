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
    .global irq_rearm_handler_t2_full
    .global irq_count_handler
    .global bcd8_to_bin
    .global measure_rtc_toggles

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

@ Times a fixed number of RTC interrupt-line transitions against Timer0, for
@ experiment 12. r0 = transitions to count, returns Timer0 ticks elapsed.
@
@ Polls INT_STATUS's RTC bit rather than taking interrupts: this app runs with
@ every source masked, and the status register reports the raw signal LEVEL
@ (see docs/hardware-notes.md's "Interrupt controller"), so the level can be
@ watched directly without un-masking anything or acknowledging anything.
@
@ It waits for one transition before starting the stopwatch, so the measurement
@ always spans whole intervals rather than starting part-way through one.
@
@ The 16-bit mask on the result is the usual Timer0 wrap (see ../README.md);
@ both callers size their transition counts to stay well inside one wrap.
measure_rtc_toggles:              @ r0 = transitions -> r0 = Timer0 ticks
    push {r4, r5, r6, r7, lr}
    mov r4, r0
    ldr r5, =INTC_STATUS
    ldr r6, =TIMER0_COUNT
    ldr r0, [r5]
    and r7, r0, #INT_RTC_BIT      @ r7 = level as last seen
mrt_align:
    ldr r0, [r5]
    and r0, r0, #INT_RTC_BIT
    cmp r0, r7
    beq mrt_align
    mov r7, r0
    ldr r3, [r6]                  @ start the stopwatch on that transition
mrt_loop:
    ldr r0, [r5]
    and r0, r0, #INT_RTC_BIT
    cmp r0, r7
    beq mrt_loop
    mov r7, r0
    subs r4, r4, #1
    bne mrt_loop
    ldr r0, [r6]
    sub r0, r3, r0                @ Timer0 counts down, so before minus after
    mov r0, r0, lsl #16
    mov r0, r0, lsr #16
    pop {r4, r5, r6, r7, lr}
    bx lr
    .ltorg

@ IRQ handler for screen 13's CLK stop test. Same shape as irq_rearm_handler
@ above - acknowledge, re-arm, count - but it counts into its own WRAM slot so
@ running the stop test cannot disturb screen 8's result.
@
@ The count is the whole point of the test. Timer1 is clocked by the System
@ Clock, the same oscillator the stop bit is believed to halt, so this counter
@ is what says whether the timers kept running while the CPU was stopped. A
@ count of zero across a multi-second stop means they froze with it.
irq_count_handler:
    push {r0, r1, r2, lr}
    ldr r0, =INTC_BASE
    ldr r1, [r0]                      @ r1 = what was pending, kept for the test below
    str r1, [r0, #0x10]               @ acknowledge everything pending

    @ Count ONLY Timer1. The buttons are un-masked too, because a press is what
    @ ends the stop, so this handler also runs for the waking press. Counting
    @ that would put a 1 or 2 in the result of a run where the timers never
    @ ticked at all, and "did the timers run" is the entire question.
    tst r1, #INT_TIMER1_BIT
    beq ich_done
    ldr r0, =WRAM_STOP_IRQCOUNT
    ldr r2, [r0]
    add r2, r2, #1
    str r2, [r0]
ich_done:
    @ No re-arm: Timer1 reloads from its period register by itself. Screen 8
    @ re-arms because re-arm latency is what it measures; here a free-running
    @ timer is both simpler and closer to what is being asked.
    pop {r0, r1, r2, pc}
    .ltorg

@ Packed-BCD byte to binary. The RTC reports seconds as BCD (see rtc.h), and
@ screen 13 needs to subtract two of them.
bcd8_to_bin:                          @ r0 = BCD byte -> r0 = 0-99
    push {r1, lr}
    mov r1, r0, lsr #4
    add r1, r1, r1, lsl #2            @ r1 = tens * 5
    mov r1, r1, lsl #1                @ r1 = tens * 10
    and r0, r0, #0xF
    add r0, r0, r1
    pop {r1, lr}
    bx lr
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

@ Experiment 11's flag-check subroutine. A disassembled trace of the real
@ transmit handler shows it calls a small subroutine that reads a byte out
@ of its own state struct, then branches on it, before doing its main work.
@ This stands in for that read. It always returns nonzero: this experiment
@ always takes the "do the work" path, since the real handler's own
@ state-machine branching is not what is being measured here, only the
@ dispatch cost around it.
exp11_flag_check:
    push {r7, lr}
    ldr r7, =WRAM_EXP11_FLAG
    ldrb r0, [r7]
    pop {r7, pc}
    .ltorg

@ Experiment 11's re-arm subroutine. Sets up Timer2's base and period, then
@ crosses into Thumb through an interworking BX to actually perform the
@ writes (exp11_thumb_rearm, thumb_loop.s) - mirroring the real handler's
@ own ARM-to-Thumb trampoline call before its own re-arm.
exp11_rearm_via_trampoline:
    push {r0, r1, lr}
    ldr r0, =TIMER2_BASE
    ldr r1, =EXP8_TIMER_PERIOD
    adr lr, exp11_trampoline_ret
    ldr r2, =exp11_thumb_rearm
    bx r2
exp11_trampoline_ret:
    pop {r0, r1, pc}
    .ltorg

@ FIQ handler for experiment 11: the full realistic dispatch shape a
@ disassembled trace of the real transmit handler showed, not just a bare
@ re-arm (compare irq_rearm_handler_t2 above). It acknowledges its own
@ sources first, calls a nested subroutine that reads a state flag, then
@ calls a second subroutine that crosses into Thumb before re-arming Timer2.
@ See experiments.s's run_experiment_11_realistic_fiq_dispatch.
irq_rearm_handler_t2_full:
    push {r0, r1, r2, lr}
    ldr r0, =INTC_BASE
    ldr r1, [r0]                       @ HOLD
    ldr r2, [r0, #8]                   @ ENABLE
    and r1, r1, r2
    str r1, [r0, #0x10]                @ acknowledge, mirroring the real handler's own early ack

    bl exp11_flag_check
    bl exp11_rearm_via_trampoline

    ldr r0, =WRAM_REARM3_COUNTER
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    pop {r0, r1, r2, pc}
    .ltorg
