@ The four (plus a diagnostic) memory-timing experiments. See ../README.md
@ for what each screen shows and what its numbers mean.
    .syntax unified
    .arm

    .include "constants.inc"

    .section .text, "ax"
    .global run_experiment_1_sanity
    .global run_experiment_2_flashctrl
    .global run_experiment_3_bios_arm
    .global run_experiment_4_bios_thumb
    .global run_experiment_6_timer_period
    .global run_experiment_7_irq_latency

@ Experiment 1: sanity check - ARM vs Thumb opcode-fetch cost (~2:1 expected)
run_experiment_1_sanity:
    push {lr}
    ldr r0, =WRAM_DATA_SCRATCH
    ldr r1, =LOOP_N
    bl measure_loop_ptr
    ldr r1, =(WRAM_RESULTS_BASE + 0)
    str r0, [r1]

    ldr r0, =WRAM_DATA_SCRATCH
    ldr r1, =LOOP_N
    adr lr, exp1_thumb_ret
    ldr r2, =measure_loop_ptr_thumb    @ .thumb_func-tagged - bit0 set automatically
    bx r2
exp1_thumb_ret:
    ldr r1, =(WRAM_RESULTS_BASE + 4)
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg

@ Experiment 2: FLASH_CTRL vs WRAM data-access cost
run_experiment_2_flashctrl:
    push {lr}
    ldr r0, =FLASH_CTRL_TEST_ADDR
    ldr r1, =LOOP_N
    bl measure_loop_ptr
    ldr r1, =(WRAM_RESULTS_BASE + 8)
    str r0, [r1]

    ldr r0, =WRAM_DATA_SCRATCH
    ldr r1, =LOOP_N
    bl measure_loop_ptr
    ldr r1, =(WRAM_RESULTS_BASE + 12)
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg

@ Experiment 3: real BIOS ARM helper (0x04001BC8) vs WRAM copy
run_experiment_3_bios_arm:
    push {lr}
    ldr r1, =LOOP_N
    bl measure_bios_call_loop_real
    ldr r1, =(WRAM_RESULTS_BASE + 16)
    str r0, [r1]

    ldr r1, =LOOP_N
    bl measure_bios_call_loop_wram
    ldr r1, =(WRAM_RESULTS_BASE + 20)
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg

@ Experiment 4: real BIOS Thumb helper (0x04001320|1) vs WRAM copy
run_experiment_4_bios_thumb:
    push {lr}
    ldr r1, =LOOP_N
    bl measure_bios_thumb_call_loop_real
    ldr r1, =(WRAM_RESULTS_BASE + 24)
    str r0, [r1]

    ldr r1, =LOOP_N
    bl measure_bios_thumb_call_loop_wram
    ldr r1, =(WRAM_RESULTS_BASE + 28)
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg

@ Experiment 6: does a timer armed with period P really take P ticks?
@
@ Motivation: a real IR-using app arms Timer2 with its nominal pulse unit
@ MINUS a hardcoded 184 (1200 - 184 = 1016), clearly expecting the resulting
@ pulse to come out at the full 1200. This emulator produces only ~1041,
@ i.e. ~159 ticks short, which is exactly why IR transfers fail in it. That
@ shortfall has two candidate causes, and they need very different fixes:
@
@   (a) the timer block itself is slower than modeled - a timer armed with
@       period P actually takes P + ~184 ticks on real hardware; or
@   (b) the timer is exact, and the missing time is spent AFTER expiry, in
@       the interrupt path (exception entry, BIOS/kernel dispatch, and the
@       app's own handler prologue) before the IR LED is finally toggled.
@
@ This experiment settles it WITHOUT taking a single interrupt, by polling
@ Timer2's reloads directly (see measure_timer_periods in helpers.s). Whatever
@ this returns is the timer block's own behavior, with no interrupt cost mixed
@ in. Running it at two periods, one double the other, additionally separates a
@ fixed per-period cost from a proportional rate error.
@
@ Screen 6 shows the raw Timer0 stopwatch totals. Do the arithmetic against
@ them rather than trusting a pre-computed delta - see ../README.md's
@ "Screen 6" section for the expected values and how to read them.
run_experiment_6_timer_period:
    push {lr}
    ldr r0, =EXP6_PERIOD_A
    ldr r1, =EXP6_RELOADS_A
    bl measure_timer_periods
    ldr r1, =WRAM_TIMER_RESULT_A
    str r0, [r1]

    ldr r0, =EXP6_PERIOD_B
    ldr r1, =EXP6_RELOADS_B
    bl measure_timer_periods
    ldr r1, =WRAM_TIMER_RESULT_B
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg

@ Experiment 7: how expensive is actually TAKING an interrupt?
@
@ Screen 6 established that the timer block is exact to within 1 tick per
@ period, which means the ~184 ticks a real IR-using app compensates for are
@ NOT timer behavior - they are spent after expiry, somewhere in the interrupt
@ path (exception entry, the BIOS/kernel dispatcher, and the app's own handler
@ prologue) before the IR LED finally toggles. This measures that path.
@
@ Method: time the exact same measurement loop twice. First with every
@ interrupt masked, exactly as the rest of this app runs. Then again with a
@ single timer interrupt live, firing at a known fixed rate. Each interrupt
@ steals its full entry+dispatch+return cost from the loop, so the difference
@ between the two totals, divided by the number of interrupts that fired, is
@ the per-interrupt cost. No interrupt handler of our own is needed or
@ installed - whatever the BIOS already does on a timer interrupt IS the thing
@ being measured, and it is the same path a real app's handler sits at the end
@ of.
@
@ This is the one measurement in this app that un-masks an interrupt, which is
@ exactly what start.s's safety net otherwise exists to prevent - so it registers
@ a handler first, and re-masks every source and restores Timer1 before
@ returning, leaving nothing live behind it. See ../README.md.
@
@ Timer1's original state is saved and restored, since the real BIOS uses it
@ (docs/hardware-notes.md: Timer1 drives its audio and GUI ticks).
run_experiment_7_irq_latency:
    push {r4, r5, r6, r7, r8, lr}

    @ --- baseline: interrupts still fully masked, exactly as the rest of this
    @ app runs ---
    ldr r0, =WRAM_DATA_SCRATCH
    ldr r1, =LOOP_N
    bl measure_loop_ptr
    ldr r1, =WRAM_IRQ_BASELINE
    str r0, [r1]

    @ --- save Timer1's current state before borrowing it ---
    ldr r4, =TIMER1_BASE
    ldr r5, [r4]                      @ saved period
    ldr r6, [r4, #4]                  @ saved count
    ldr r7, [r4, #8]                  @ saved control

    @ --- arm Timer1 as the interrupt source ---
    mov r0, #0
    str r0, [r4, #8]                  @ stop before reprogramming
    ldr r0, =EXP7_TIMER_PERIOD
    str r0, [r4]
    str r0, [r4, #4]
    mov r0, #EXP7_TIMER_CTRL
    str r0, [r4, #8]

    @ --- install an interrupt handler BEFORE un-masking anything. Without this
    @ the kernel has no app callback registered, and the first interrupt taken
    @ corrupts the app - reproduced directly in this project's emulator, which
    @ is what stopped this experiment from ever reaching real hardware in that
    @ state. See register_irq_handler/irq_ack_handler in helpers.s. ---
    ldr r0, =irq_ack_handler
    adr lr, exp7_reg_ret
    ldr r1, =register_irq_handler
    bx r1
exp7_reg_ret:

    @ --- un-mask ONLY Timer1's source. A write to INTC_ENABLE ORs bits in
    @ (see docs/hardware-notes.md's memory map), so nothing else is disturbed. ---
    ldr r0, =INTC_ENABLE
    mov r1, #INT_TIMER1_BIT
    str r1, [r0]

    @ --- the same loop again, now being interrupted at a known fixed rate ---
    ldr r0, =WRAM_DATA_SCRATCH
    ldr r1, =LOOP_N
    bl measure_loop_ptr
    mov r8, r0                        @ stash the result before touching anything else

    @ --- re-mask every source immediately: the measurement window is over and
    @ nothing after this point should be able to take an interrupt ---
    ldr r0, =INTC_MASK
    mvn r1, #0
    str r1, [r0]

    @ --- unregister our handler again, so nothing after this point depends on
    @ it still being installed ---
    mov r0, #0
    adr lr, exp7_unreg_ret
    ldr r1, =register_irq_handler
    bx r1
exp7_unreg_ret:

    @ --- restore Timer1 exactly as it was found ---
    mov r0, #0
    str r0, [r4, #8]
    str r5, [r4]
    str r6, [r4, #4]
    str r7, [r4, #8]

    ldr r1, =WRAM_IRQ_WITH_IRQ
    str r8, [r1]

    pop {r4, r5, r6, r7, r8, lr}
    bx lr
    .ltorg
