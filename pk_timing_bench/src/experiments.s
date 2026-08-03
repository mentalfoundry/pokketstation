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
    .global run_experiment_8_rearm_latency
    .global run_experiment_9_irda_write
    .global run_experiment_10_fiq_rearm_latency
    .global run_experiment_11_realistic_fiq_dispatch
    .global run_stop_test
    .global run_experiment_12_rtc_rates

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
@   (a) the timer block itself is slower than modeled. A timer armed with
@       period P then takes P + ~184 ticks on real hardware. Or:
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
@ them rather than trusting a pre-computed delta. See ../README.md's
@ "Screen 6" section for the expected values, and for how to read them.
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
@ NOT timer behavior. They are spent after expiry, somewhere in the interrupt
@ path, before the IR LED finally toggles. That path covers exception entry, the
@ BIOS/kernel dispatcher, and the app's own handler prologue.
@ This measures that path.
@
@ Method: time the exact same measurement loop twice. First with every
@ interrupt masked, exactly as the rest of this app runs. Then again with a
@ single timer interrupt live, firing at a known fixed rate. Each interrupt
@ steals its full entry+dispatch+return cost from the loop, so the difference
@ between the two totals, divided by the number of interrupts that fired, is
@ the per-interrupt cost. No interrupt handler of our own is needed or
@ installed. Whatever the BIOS already does on a timer interrupt IS the thing
@ this measures. A real app's handler sits at the end of that same path.
@
@ This is the one measurement in this app that un-masks an interrupt, which is
@ exactly what start.s's safety net otherwise exists to prevent.
@ It therefore registers a handler first. It then re-masks every source and
@ restores Timer1 before it returns, so it leaves nothing live behind it.
@ See ../README.md.
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
    @ corrupts the app. This project's emulator reproduced that directly, which
    @ is what stopped this experiment from ever reaching real hardware in that
    @ state. See register_irq_handler and irq_ack_handler in helpers.s. ---
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

@ Experiment 8: how long does a timer expiry take to reach its handler's re-arm?
@
@ Screens 6 and 7 each ruled out a candidate for the ~184 ticks a real IR-using
@ app compensates for. Screen 6 showed the timer block itself is exact to within
@ 1 tick per period. Screen 7 showed a bare interrupt round trip costs about the
@ same on hardware as in this emulator. Neither explained the shortfall.
@
@ Tracing that app showed why: its transmit handler RE-ARMS the timer on every
@ interrupt instead of letting it free-run. That changes what latency does. A
@ free-running timer keeps its period no matter how late the handler runs, so
@ latency cancels. A re-armed timer does not start its next period until the
@ handler reaches the re-arm, so latency is added to every single period.
@
@ This measures that directly. Timer1 is armed with the same 1016 and /2 divisor
@ the real app uses, its handler re-arms it with the same value, and Timer0 times
@ how long 64 of those periods really take.
@
@   effective period = Timer0 delta * 32 / 64 raw cycles, then / 2 for Timer1 ticks
@   latency          = effective period - (1016 + 1)
@
@ This emulator produces about 26 ticks of latency. The real app's own arithmetic
@ implies about 184. See ../README.md's "Screen 8".
run_experiment_8_rearm_latency:
    push {r4, r5, r6, r7, r8, lr}

    ldr r0, =WRAM_REARM_COUNTER        @ counter starts at zero
    mov r1, #0
    str r1, [r0]

    ldr r4, =TIMER1_BASE               @ save Timer1 before borrowing it
    ldr r5, [r4]
    ldr r6, [r4, #4]
    ldr r7, [r4, #8]

    ldr r0, =irq_rearm_handler         @ install the handler before un-masking
    adr lr, exp8_reg_ret
    ldr r1, =register_irq_handler
    bx r1
exp8_reg_ret:

    mov r0, #0                         @ arm Timer1 exactly as the real app does
    str r0, [r4, #8]
    ldr r0, =EXP8_TIMER_PERIOD
    str r0, [r4]
    str r0, [r4, #4]
    mov r0, #EXP8_TIMER_CTRL
    str r0, [r4, #8]

    ldr r0, =INTC_ENABLE               @ un-mask only Timer1
    mov r1, #INT_TIMER1_BIT
    str r1, [r0]

    ldr r8, =WRAM_REARM_COUNTER
exp8_sync:                             @ start on an interrupt boundary
    ldr r0, [r8]
    cmp r0, #0
    beq exp8_sync

    mov r0, #0                         @ restart the count, then take the stopwatch
    str r0, [r8]
    ldr r2, =TIMER0_COUNT
    ldr r3, [r2]

exp8_wait:
    ldr r0, [r8]
    cmp r0, #EXP8_INTERRUPTS
    blo exp8_wait
    ldr r0, [r2]
    sub r0, r3, r0                     @ Timer0 counts down, so before minus after
    mov r0, r0, lsl #16                @ 16-bit mask; Timer0 wraps at 0x10000
    mov r0, r0, lsr #16

    @ Store the result NOW, before anything else runs. register_irq_handler
    @ returns through "pop {r3}; bx r3", so it clobbers r3 - stashing the delta
    @ in a scratch register across the unregister call below silently replaced
    @ it with a return address the first time this was written.
    ldr r1, =WRAM_REARM_DELTA
    str r0, [r1]

    ldr r0, =INTC_MASK                 @ re-mask every source at once
    mvn r1, #0
    str r1, [r0]

    mov r0, #0                         @ unregister our handler again
    adr lr, exp8_unreg_ret
    ldr r1, =register_irq_handler
    bx r1
exp8_unreg_ret:

    mov r0, #0                         @ restore Timer1 as it was found
    str r0, [r4, #8]
    str r5, [r4]
    str r6, [r4, #4]
    str r7, [r4, #8]

    pop {r4, r5, r6, r7, r8, lr}
    bx lr
    .ltorg

@ Experiment 9: does IRDA_DATA's own MMIO write cost more than a plain WRAM
@ store?
@
@ Screens 6, 7, and 8 each measured a generic interrupt/timer-path cost
@ against real hardware, and all three matched (or undershot) this emulator.
@ None of them explain the ~184-tick IR pulse-width shortfall. See
@ docs/hardware-notes.md's "Unresolved" bullet.
@
@ What is left is specific to the real transmit handler's own work. The one
@ MMIO write on that handler's hot path is IRDA_DATA, the LED bit, toggled on
@ every pulse edge. This measures that write's cost directly: a tight loop of
@ 30000 stores to IRDA_DATA (test), against the same loop storing to a WRAM
@ scratch address instead (control). Same method screen 2 already used to
@ settle FLASH_CTRL's data-access rate, just stores instead of loads.
@
@ IRDA_MODE is left untouched, so this always runs in whatever mode the
@ device powers up in. IFMODE defaults to receive (this emulator's own
@ default; the real POR value is undocumented - see core/src/ir.h), and a
@ DATA write only drives the transmit LED while IFMODE=transmit. Storing a
@ constant 0 here measures the register's own bus-access cost, not any
@ transmit side effect, so it does not need transmit mode armed to be valid.
run_experiment_9_irda_write:
    push {lr}
    ldr r0, =IRDA_DATA_ADDR
    ldr r1, =LOOP_N
    bl measure_loop_ptr_store
    ldr r1, =WRAM_IRDA_TEST_RESULT
    str r0, [r1]

    ldr r0, =WRAM_DATA_SCRATCH
    ldr r1, =LOOP_N
    bl measure_loop_ptr_store
    ldr r1, =WRAM_IRDA_CTRL_RESULT
    str r0, [r1]
    pop {lr}
    bx lr
    .ltorg

@ Experiment 10: does expiry-to-re-arm latency come out differently over FIQ
@ than over IRQ?
@
@ A disassembled trace of the real IR transmit handler (not a synthetic one)
@ shows it runs on FIQ: Timer2 is hardwired to FIQ (INT_FIQ_MASK, see
@ docs/hardware-notes.md's "Interrupt controller"), and the real handler is
@ reached through the FIQ vector (0x1C), confirmed by CPSR mode 0x11 at the
@ point of its IRDA_DATA write. Screens 7 and 8 only ever measured IRQ
@ (Timer1). FIQ's own exception-entry cost has never been measured on real
@ hardware, only assumed identical to IRQ's (see core/src/cpu.c: both use the
@ same "2S+1N" pipeline-refill formula).
@
@ This is screen 8's exact method - same period (1016), same /2 divisor,
@ same 64-reload count, same re-arm-in-handler shape - moved onto Timer2, the
@ one timer that is actually FIQ-routed. Directly comparable to screen 8: the
@ only thing that changes is the exception type.
@
@ This uses register_fiq_handler (thumb_loop.s), not register_irq_handler.
@ The two are different SWI 1 callback slots, confirmed by disassembling a
@ real J-110 BIOS dump: the IRQ vector handler (0x04001414) reads its
@ callback from RAM offset 0xFC, the FIQ vector handler (0x040014D4) reads
@ its own from offset 0x100 - a different slot entirely. This was found by
@ trying register_irq_handler here first: it hung, in this emulator, because
@ nothing ever acknowledged Timer2's HOLD bit, so FIQ re-asserted immediately
@ on return and the CPU never left the vector. See register_fiq_handler's own
@ comment for the full story.
run_experiment_10_fiq_rearm_latency:
    push {r4, r5, r6, r7, r8, lr}

    ldr r0, =WRAM_REARM2_COUNTER       @ counter starts at zero
    mov r1, #0
    str r1, [r0]

    ldr r4, =TIMER2_BASE                @ save Timer2 before borrowing it
    ldr r5, [r4]
    ldr r6, [r4, #4]
    ldr r7, [r4, #8]

    ldr r0, =irq_rearm_handler_t2       @ install the handler before un-masking
    adr lr, exp10_reg_ret
    ldr r1, =register_fiq_handler
    bx r1
exp10_reg_ret:

    mov r0, #0                          @ arm Timer2 exactly as the real app does
    str r0, [r4, #8]
    ldr r0, =EXP8_TIMER_PERIOD
    str r0, [r4]
    str r0, [r4, #4]
    mov r0, #EXP8_TIMER_CTRL
    str r0, [r4, #8]

    ldr r0, =INTC_ENABLE                @ un-mask only Timer2's FIQ-side bit
    ldr r1, =INT_TIMER2_BIT
    str r1, [r0]

    ldr r8, =WRAM_REARM2_COUNTER
exp10_sync:                             @ start on an interrupt boundary
    ldr r0, [r8]
    cmp r0, #0
    beq exp10_sync

    mov r0, #0                          @ restart the count, then take the stopwatch
    str r0, [r8]
    ldr r2, =TIMER0_COUNT
    ldr r3, [r2]

exp10_wait:
    ldr r0, [r8]
    cmp r0, #EXP8_INTERRUPTS
    blo exp10_wait
    ldr r0, [r2]
    sub r0, r3, r0                      @ Timer0 counts down, so before minus after
    mov r0, r0, lsl #16                 @ 16-bit mask; Timer0 wraps at 0x10000
    mov r0, r0, lsr #16

    ldr r1, =WRAM_REARM2_DELTA          @ store the result now, before anything else
    str r0, [r1]                        @ runs - see run_experiment_8_rearm_latency's
                                         @ comment on register_irq_handler clobbering r3

    ldr r0, =INTC_MASK                  @ re-mask every source at once
    mvn r1, #0
    str r1, [r0]

    mov r0, #0                          @ unregister our handler again
    adr lr, exp10_unreg_ret
    ldr r1, =register_fiq_handler
    bx r1
exp10_unreg_ret:

    mov r0, #0                          @ restore Timer2 as it was found
    str r0, [r4, #8]
    str r5, [r4]
    str r6, [r4, #4]
    str r7, [r4, #8]

    pop {r4, r5, r6, r7, r8, lr}
    bx lr
    .ltorg

@ Experiment 11: what does the real transmit handler's FULL dispatch chain
@ cost, not just a bare re-arm?
@
@ Screen 10 measured a bare re-arm over FIQ and found it costs the same as
@ IRQ (0 ticks on real hardware). But a disassembled trace of the real
@ transmit handler shows its actual dispatch is not bare: it acknowledges
@ its own interrupt sources, calls through a jump table indexed by INTC bit,
@ calls a nested subroutine that reads a state flag, then calls a second
@ subroutine that crosses from ARM to Thumb through an interworking BX
@ before it re-arms Timer2. Measured directly in this emulator (not yet on
@ real hardware - see docs/hardware-notes.md's "Unresolved" bullet), that
@ full chain costs 128-160 Timer2 ticks for the steady-state bulk of a real
@ transmission: most of the app's 184-tick budget, far more than screen
@ 10's bare re-arm.
@
@ This reproduces that same shape - acknowledge, nested ARM call, ARM-to-
@ Thumb trampoline, re-arm - using screen 8/10's exact measurement method,
@ so the result is directly comparable across all three: screen 8 (bare,
@ IRQ), screen 10 (bare, FIQ), screen 11 (full realistic dispatch, FIQ).
@ See irq_rearm_handler_t2_full, exp11_flag_check, and
@ exp11_rearm_via_trampoline in helpers.s, and exp11_thumb_rearm in
@ thumb_loop.s.
run_experiment_11_realistic_fiq_dispatch:
    push {r4, r5, r6, r7, r8, lr}

    ldr r0, =WRAM_REARM3_COUNTER       @ counter starts at zero
    mov r1, #0
    str r1, [r0]
    ldr r0, =WRAM_EXP11_FLAG           @ dummy state byte the flag-check subroutine reads
    mov r1, #1
    strb r1, [r0]

    ldr r4, =TIMER2_BASE                @ save Timer2 before borrowing it
    ldr r5, [r4]
    ldr r6, [r4, #4]
    ldr r7, [r4, #8]

    ldr r0, =irq_rearm_handler_t2_full  @ install the handler before un-masking
    adr lr, exp11_reg_ret
    ldr r1, =register_fiq_handler
    bx r1
exp11_reg_ret:

    mov r0, #0                          @ arm Timer2 exactly as the real app does
    str r0, [r4, #8]
    ldr r0, =EXP8_TIMER_PERIOD
    str r0, [r4]
    str r0, [r4, #4]
    mov r0, #EXP8_TIMER_CTRL
    str r0, [r4, #8]

    ldr r0, =INTC_ENABLE                @ un-mask only Timer2's FIQ-side bit
    ldr r1, =INT_TIMER2_BIT
    str r1, [r0]

    ldr r8, =WRAM_REARM3_COUNTER
exp11_sync:                             @ start on an interrupt boundary
    ldr r0, [r8]
    cmp r0, #0
    beq exp11_sync

    mov r0, #0                          @ restart the count, then take the stopwatch
    str r0, [r8]
    ldr r2, =TIMER0_COUNT
    ldr r3, [r2]

exp11_wait:
    ldr r0, [r8]
    cmp r0, #EXP8_INTERRUPTS
    blo exp11_wait
    ldr r0, [r2]
    sub r0, r3, r0                      @ Timer0 counts down, so before minus after
    mov r0, r0, lsl #16                 @ 16-bit mask; Timer0 wraps at 0x10000
    mov r0, r0, lsr #16

    ldr r1, =WRAM_REARM3_DELTA          @ store the result now, before anything else
    str r0, [r1]                        @ runs - see run_experiment_8_rearm_latency's
                                         @ comment on register_irq_handler clobbering r3

    ldr r0, =INTC_MASK                  @ re-mask every source at once
    mvn r1, #0
    str r1, [r0]

    mov r0, #0                          @ unregister our handler again
    adr lr, exp11_unreg_ret
    ldr r1, =register_fiq_handler
    bx r1
exp11_unreg_ret:

    mov r0, #0                          @ restore Timer2 as it was found
    str r0, [r4, #8]
    str r5, [r4]
    str r6, [r4, #4]
    str r7, [r4, #8]

    pop {r4, r5, r6, r7, r8, lr}
    bx lr
    .ltorg

@ Screen 13: does CLK control (0x0B000004) bit 0 stop the CPU, and what
@ happens around it?
@
@ This is the only interactive test in this app. Every other experiment runs
@ once at startup and leaves a number behind. This one cannot: the quantity
@ being measured is how long the CPU stayed stopped, and on a device with no
@ other input that interval is ended by a human pressing a button. So it runs
@ on a Down press while screen 13 is showing (see poll_buttons in ui.s).
@
@ Four questions, one run. See ../README.md for the decision table that maps
@ the three numbers this leaves behind onto answers, and
@ docs/hardware-notes.md's "CLK control" section for why they matter.
@
@ Deliberately, this does NOT reproduce the rest of the real app's power-down
@ sequence - no IOP_STOP, no INTC mask of the RTC, no LCD_MODE change. The one
@ store under test is the CLK write, alone. If the CPU stops anyway, that
@ isolates this register from everything else the real app happens to write
@ around it, which no amount of tracing the real app can do.
@
@ The LCD is deliberately left ON, unlike the real app's sequence, so the
@ result is readable afterwards.
@
@ Recovery: if the CPU stops and nothing can wake it, the device needs its
@ physical reset button. Nothing here writes flash, so that is the whole cost.
run_stop_test:
    push {r4, r5, r6, r7, r8, lr}

    ldr r0, =WRAM_STOP_IRQCOUNT        @ counter starts at zero
    mov r1, #0
    str r1, [r0]

    ldr r4, =TIMER1_BASE               @ save Timer1 before borrowing it
    ldr r5, [r4]
    ldr r6, [r4, #4]
    ldr r7, [r4, #8]

    ldr r0, =irq_count_handler         @ install the handler before un-masking.
    adr lr, stop_reg_ret               @ Un-masking a timer interrupt with no
    ldr r1, =register_irq_handler      @ app callback registered visibly
    bx r1                              @ corrupts the app - see experiment 7.
stop_reg_ret:

    mov r0, #0                         @ arm Timer1, slowly (see constants.inc)
    str r0, [r4, #8]
    ldr r0, =STOP_TIMER_PERIOD
    str r0, [r4]
    str r0, [r4, #4]
    mov r0, #STOP_TIMER_CTRL
    str r0, [r4, #8]

    @ Un-mask the buttons AND Timer1. Buttons, so a press can end the stop.
    @ Timer1, so its count afterwards says whether the timers kept running -
    @ and, if they also wake the CPU, the stop will end on its own with no
    @ button pressed at all, which is itself one of the four answers.
    ldr r0, =INTC_ENABLE
    ldr r1, =(INT_BUTTON_BITS | INT_TIMER1_BIT)
    str r1, [r0]

    @ A solid bar across the middle of the screen, drawn immediately before the
    @ store. If the clock really stops, this is the last thing this app draws
    @ and it stays on screen until something wakes the CPU. If the store does
    @ nothing, the bar is replaced by the result screen too fast to see.
    mov r0, #14
    mov r1, #0
    mov r2, #31
    bl draw_hline
    mov r0, #15
    mov r1, #0
    mov r2, #31
    bl draw_hline
    mov r0, #16
    mov r1, #0
    mov r2, #31
    bl draw_hline

    ldr r0, =RTC_TIME_ADDR             @ stash the "before" seconds in WRAM, not
    ldr r0, [r0]                       @ a register: whatever wakes the CPU runs
    and r0, r0, #0xFF                  @ an interrupt handler first, and only
    ldr r1, =WRAM_STOP_SECONDS         @ memory is guaranteed to survive that
    str r0, [r1]

    @ --- the single store under test ---
    @ A full 32-bit STR, matching exactly what the real app issues. Byte-wide
    @ access to MMIO is a known real-hardware hazard here (see ../README.md).
    ldr r0, =CLK_CONTROL_ADDR
    mov r1, #1
    str r1, [r0]

    @ --- nothing below runs until the CPU is running again ---

    ldr r0, =WRAM_STOP_IRQCOUNT        @ snapshot the count FIRST: the timer
    ldr r8, [r0]                       @ interrupt is still live, and every
                                       @ instruction from here adds to it

    ldr r0, =RTC_TIME_ADDR
    ldr r0, [r0]
    and r0, r0, #0xFF
    mov r1, r0                         @ r1 = "after", BCD

    ldr r0, =CLK_CONTROL_ADDR          @ read back before restoring anything,
    ldr r2, [r0]                       @ to see whether the bit self-cleared
    ldr r0, =WRAM_STOP_READBACK
    str r2, [r0]

    ldr r0, =INTC_MASK                 @ re-mask every source at once
    mvn r2, #0
    str r2, [r0]

    ldr r0, =WRAM_STOP_IRQCOUNT        @ store the snapshot, not the live value
    str r8, [r0]

    @ seconds delta, as (after - before) mod 60
    mov r0, r1
    bl bcd8_to_bin
    mov r8, r0                         @ r8 = after, binary
    ldr r0, =WRAM_STOP_SECONDS
    ldr r0, [r0]
    bl bcd8_to_bin                     @ r0 = before, binary
    subs r0, r8, r0
    addmi r0, r0, #60
    ldr r1, =WRAM_STOP_SECONDS
    str r0, [r1]

    mov r0, #0                         @ unregister our handler again
    adr lr, stop_unreg_ret
    ldr r1, =register_irq_handler
    bx r1
stop_unreg_ret:

    mov r0, #0                         @ restore Timer1 as it was found
    str r0, [r4, #8]
    str r5, [r4]
    str r6, [r4, #4]
    str r7, [r4, #8]

    pop {r4, r5, r6, r7, r8, lr}
    bx lr
    .ltorg

@ Experiment 12 (screen 14): what are the RTC's two interrupt-line rates?
@
@ The documentation says the line runs at approximately 1Hz while the RTC is
@ running, and approximately 4096Hz while it is paused (mode bit0, PRGSEL, the
@ state the BIOS puts it in so RTC_ADJUST can step one field without the clock
@ moving underneath it). Neither number has ever been measured on real
@ hardware, and "approximately" is doing real work in that sentence: this
@ emulator derives its paused rate as exactly 4096x its running rate, so if the
@ real ratio is anything else, every RTC_ADJUST-driven wait in the BIOS is
@ mistimed here.
@
@ The running rate matters for a different reason. It is what makes an emulated
@ second last a real second, so the emulated device's own clock keeps or loses
@ time by exactly this ratio. Getting it wrong is not subtle: the constant
@ behind it was 3.79x off for a long time, and the emulated clock lost about 45
@ minutes an hour.
@
@ Method: poll INT_STATUS's RTC bit and time a fixed number of transitions
@ against Timer0 (see measure_rtc_toggles in helpers.s). Nothing is un-masked
@ and no interrupt is taken - the status register reports the raw signal level,
@ so the line can be watched directly.
@
@ Screen 14 shows the two raw Timer0 tick counts. Do the arithmetic against
@ them rather than trusting a pre-computed rate; ../README.md's "Screen 14"
@ section has the expected values and how to convert.
run_experiment_12_rtc_rates:
    push {r4, r5, r6, lr}

    @ --- paused/program mode, at Timer0's normal /32 divisor ---
    ldr r4, =RTC_MODE_ADDR
    ldr r5, [r4]                       @ save the whole mode word: bits 1-3 are
    orr r0, r5, #1                     @ CNTSEL and must come back untouched
    str r0, [r4]

    mov r0, #RTC_TOGGLES_PAUSED
    bl measure_rtc_toggles
    ldr r1, =WRAM_RTC_PAUSED_TICKS
    str r0, [r1]

    str r5, [r4]                       @ back to running before anything else

    @ --- running mode. A toggle is a whole second here, which overflows
    @ Timer0's real 16-bit count at /32, so slow Timer0 to /512 first. ---
    ldr r4, =TIMER0_BASE
    ldr r6, [r4, #8]                   @ save Timer0 control
    mov r0, #0
    str r0, [r4, #8]                   @ stop before reprogramming
    mvn r0, #0
    str r0, [r4]
    str r0, [r4, #4]
    mov r0, #TIMER0_CTRL_DIV512
    str r0, [r4, #8]

    mov r0, #RTC_TOGGLES_RUN
    bl measure_rtc_toggles
    ldr r1, =WRAM_RTC_RUN_TICKS
    str r0, [r1]

    mov r0, #0                         @ restore Timer0 exactly as it was found:
    str r0, [r4, #8]                   @ every other screen's stopwatch is this
    mvn r0, #0                         @ timer, and this app keeps running after
    str r0, [r4]
    str r0, [r4, #4]
    str r6, [r4, #8]

    pop {r4, r5, r6, lr}
    bx lr
    .ltorg
