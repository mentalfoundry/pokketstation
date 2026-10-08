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

    mov r0, #RTC_TOGGLES_RUN_SETTLE    @ discarded: the first pulse after leaving
    bl measure_rtc_toggles             @ program mode may be a partial one

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

@ --- Experiment 13 (screen 15): when does the COM block raise its interrupt? ---
@
@ The kernel's FIQ entry waits for Ready up to 30 times before it takes the
@ first byte of a command (0x04001086..0x040010A8 in J110). That wait is only
@ needed if the interrupt can come BEFORE the byte is complete. This
@ experiment measures it directly, with no logic analyser: the app keeps the
@ COM interrupt masked (it masks every source at startup), so the kernel never
@ answers, and it times each change of the status bits itself.
@
@ Each run:
@   1. Timer0 to /2 (0.5us at CLK_MODE 7), restored afterwards, as experiment
@      12 does with /512.
@   2. The kernel's own link setup at docking (0x0400073E..0x0400078C):
@      CTRL1=0, MODE=2, DATA=0xFF, CTRL2=3, MODE=6, then CTRL1 = the value
@      under test (3 for captures 0 to 5, 1 for capture 6, 2 for capture 7;
@      the kernel itself uses 2 while it waits).
@   3. Wait for COM_QUIET_ITERS samples with no change. Each start of that
@      wait writes CTRL2 = 3, reads DATA and acknowledges INT_COM, to clear
@      what an access left. On real hardware the STAT2 bits stay set until
@      something clears them (the first hardware run of this screen), and the
@      kernel writes CTRL2 = 1 straight after it sees Ready (0x040010A2,
@      0x0400159C in J110).
@   4. com_log_changes: sample until the first change, then for
@      COM_WINDOW_ITERS more samples, and log each change with its Timer0
@      count, up to COM_LOG_EVENTS changes. The loop does nothing else, so the
@      sample after a change comes as soon as the sample after no change.
@   5. From the log: when each bit first differs from its value before the
@      first change.
@
@ The six bits of a sample, and their slots (WRAM_COM_TIMES + 4*slot):
@   0-1  COM_STAT1 bits 0-1      (bit 1 is the release of /SEL)
@   2-3  COM_STAT2 bits 0-1      (bit 0 is Ready)
@   4    INTC HOLD bit 6         (INT_COM latched)
@   5    INTC STATUS bit 6       (INT_COM raw input)
@ STAT1 bit 2 and STAT2 bit 2 never changed in the first hardware run, so they
@ are not watched. A slot is the Timer0 /2 ticks from the first change, or
@ 0xFFFF for "never". Slot 6 = changes logged << 8 | the last sample of the
@ window. Slot 7 = the Timer0 /2 ticks of the last change.
@ WRAM_COM_INFO = the capture number << 24 | bits before the change << 16 |
@ Timer0 /2 ticks the whole window took. The window divided by
@ COM_WINDOW_ITERS is the time between two samples, which is the resolution.
@ Window 0 means no access came before the timeout.
@
@ The sampling loop is Thumb: a Thumb fetch from FLASH costs 1 cycle, against
@ 2 for ARM (screens 1 to 4).
@
@ A sample reads STAT1, STAT2, HOLD and STATUS one after the other, so a
@ change can fall between two of those reads. The next sample then shows the
@ rest of it.

@ \rd = the six bits now, in the same form as the Thumb loop. Uses r12.
.macro com_sample rd
    ldr \rd, [r4, #4]                  @ STAT1 bits 0-1
    and \rd, \rd, #3
    ldr r12, [r4, #0x14]               @ STAT2 bits 0-1, to bits 2-3
    and r12, r12, #3
    orr \rd, \rd, r12, lsl #2
    ldr r12, [r5]                      @ INTC HOLD bit 6, to bit 4
    and r12, r12, #INT_COM_BIT
    orr \rd, \rd, r12, lsr #2
    ldr r12, [r5, #4]                  @ INTC STATUS bit 6, to bit 5
    and r12, r12, #INT_COM_BIT
    orr \rd, \rd, r12, lsr #1
.endm

@ Calls a Thumb routine from ARM. ARMv4T has no BLX.
.macro call_thumb fn
    ldr r12, =\fn
    mov lr, pc
    bx r12
.endm

    .global run_com_capture
run_com_capture:
    push {r4, r5, r6, r7, r8, r9, r10, r11, lr}
    ldr r4, =COM_BASE
    ldr r5, =INTC_BASE
    ldr r6, =TIMER0_BASE

    ldr r0, =0xFFFF                    @ every bit slot starts as "never"
    ldr r2, =WRAM_COM_TIMES
    mov r1, #6
cc_clear:
    str r0, [r2], #4
    subs r1, r1, #1
    bne cc_clear
    mov r0, #0
    str r0, [r2], #4                   @ slot 6
    str r0, [r2]                       @ slot 7

    ldr r0, [r6, #8]                   @ save Timer0 control, then /2
    push {r0}
    mov r0, #0
    str r0, [r6, #8]
    mvn r0, #0
    str r0, [r6]
    str r0, [r6, #4]
    mov r0, #TIMER0_CTRL_DIV2
    str r0, [r6, #8]

    mov r0, #0                         @ the kernel's link setup at docking
    str r0, [r4, #0x10]                @ CTRL1 = 0
    mov r0, #2
    str r0, [r4]                       @ MODE = 2
    mov r0, #0xFF
    str r0, [r4, #8]                   @ DATA = 0xFF
    mov r0, #3
    str r0, [r4, #0x18]                @ CTRL2 = 3
    mov r0, #6
    str r0, [r4]                       @ MODE = 6
    ldr r0, =WRAM_COM_CTRL1_SEL        @ CTRL1 = the value under test:
    ldr r0, [r0]                       @ 3 for captures 0-5, then 1, then 2
    cmp r0, #6
    movlo r0, #3
    subhs r0, r0, #5
    str r0, [r4, #0x10]

    mov r9, #COM_QUIET_TRIES           @ quiet attempts before giving up
cc_quiet:
    mov r0, #3                         @ clear what an access left, at each new
    str r0, [r4, #0x18]                @ start of the quiet span: CTRL2 = 3 for
    ldr r0, [r4, #8]                   @ the STAT2 bits, a DATA read, and an
    mov r0, #INT_COM_BIT               @ acknowledge of INT_COM. An access that
    str r0, [r5, #0x10]                @ comes inside the span must not become the
    com_sample r7                      @ bits before the change. r7 = those bits
    ldr r2, =COM_QUIET_ITERS
    call_thumb com_wait_change
    cmp r2, #0
    beq cc_log                         @ no change for the whole quiet span
    subs r9, r9, #1
    bne cc_quiet
    mov r11, r7
    mov r9, #0                         @ no log
    b cc_store_info

cc_log:
    mov r11, r7                        @ r11 = the bits before the change
    push {r11}                         @ (the Thumb loop takes the window in r11)
    ldr r3, =WRAM_COM_LOG
    mov r8, r6                         @ Timer0 base
    add r9, r3, #8                     @ the end of the first entry
    add r10, r3, #(COM_LOG_EVENTS * 8) @ the end of the log
    ldr r11, =COM_WINDOW_ITERS
    ldr r2, =COM_ARM_ITERS
    call_thumb com_log_changes         @ -> r3 = the end of what was logged
    pop {r11}
    ldr r1, [r6, #4]                   @ the time at the end of the window

    ldr r10, =WRAM_COM_LOG
    sub r9, r3, r10
    movs r9, r9, lsr #3                @ r9 = changes logged
    beq cc_store_info                  @ none: no access came

    ldr r8, [r10, #4]                  @ t0 = the time of the first change
    sub r1, r8, r1                     @ the window, in ticks
    mov r1, r1, lsl #16
    mov r1, r1, lsr #16
    push {r1}

    ldr r7, =WRAM_COM_TIMES
    mov r2, #0                         @ bits already recorded
    mov r3, r10
cc_scan:                               @ each logged change, oldest first
    ldr r0, [r3], #4                   @ the sample
    ldr r1, [r3], #4                   @ its Timer0 count
    sub r1, r8, r1
    mov r1, r1, lsl #16
    mov r1, r1, lsr #16                @ ticks from the first change
    mov lr, r0                         @ the last sample so far
    eor r0, r0, r11
    bic r0, r0, r2                     @ bits that differ and are not recorded yet
    orr r2, r2, r0
    mov r12, #0
cc_bit:
    tst r0, #1
    strne r1, [r7, r12, lsl #2]
    add r12, r12, #1
    movs r0, r0, lsr #1
    bne cc_bit
    subs r9, r9, #1
    bne cc_scan
cc_scan_done:                          @ r1 = the ticks of the last change
    str r1, [r7, #28]                  @ slot 7
    sub r0, r3, r10
    mov r0, r0, lsr #3                 @ changes logged
    and lr, lr, #0xFF
    orr r0, lr, r0, lsl #8
    str r0, [r7, #24]                  @ slot 6
    pop {r9}                           @ r9 = the window, in ticks

cc_store_info:
    ldr r0, =WRAM_COM_CTRL1_SEL        @ the capture number: CTRL1 is its low
    ldr r0, [r0]                       @ two bits
    and r0, r0, #7
    mov r0, r0, lsl #24
    and r1, r11, #0xFF
    orr r0, r0, r1, lsl #16
    orr r0, r0, r9
    ldr r1, =WRAM_COM_INFO
    str r0, [r1]

    mov r0, #3                         @ leave the link as the kernel's end of a
    str r0, [r4, #0x18]                @ command does: CTRL2 = 3, CTRL1 = 2
    mov r0, #2
    str r0, [r4, #0x10]
    mov r0, #INT_COM_BIT
    str r0, [r5, #0x10]

    pop {r0}                           @ restore Timer0 exactly as it was found
    mov r1, #0
    str r1, [r6, #8]
    mvn r1, #0
    str r1, [r6]
    str r1, [r6, #4]
    str r0, [r6, #8]

    pop {r4, r5, r6, r7, r8, r9, r10, r11, lr}
    bx lr
    .ltorg

    .thumb

@ Samples until the seven bits differ from r7, or r2 samples have passed.
@ In: r4 = COM base, r5 = INTC base, r7 = the bits to compare, r2 = samples.
@ Out: r0 = the last sample, r2 = the samples that remain (0 = no change).
    .thumb_func
com_wait_change:
    push {r6}
    movs r6, #INT_COM_BIT
cwc_loop:
    ldr r0, [r4, #4]                   @ STAT1 bits 0-1
    lsls r0, r0, #30
    lsrs r0, r0, #30
    ldr r1, [r4, #0x14]                @ STAT2 bits 0-1, to bits 2-3
    lsls r1, r1, #30
    lsrs r1, r1, #28
    orrs r0, r1
    ldr r1, [r5]                       @ INTC HOLD bit 6, to bit 4
    ands r1, r6
    lsrs r1, r1, #2
    orrs r0, r1
    ldr r1, [r5, #4]                   @ INTC STATUS bit 6, to bit 5
    ands r1, r6
    lsrs r1, r1, #1
    orrs r0, r1
    cmp r0, r7
    bne cwc_done
    subs r2, r2, #1
    bne cwc_loop
cwc_done:
    pop {r6}
    bx lr

@ Samples like com_wait_change, and logs each change as two words: the sample
@ and the Timer0 count after it. The first change starts the window: r2 then
@ becomes the window length. Stops at the end of the window, or when the log
@ is full, or after r2 samples with no change at all.
@ In: r4 = COM base, r5 = INTC base, r7 = the bits before, r2 = samples to
@ wait for the first change, r3 = the log, r8 = Timer0 base, r9 = the end of
@ the first entry, r10 = the end of the log, r11 = the window in samples.
@ Out: r3 = the end of what was logged.
    .thumb_func
com_log_changes:
    push {r6}
    movs r6, #INT_COM_BIT
clc_loop:
    ldr r0, [r4, #4]
    lsls r0, r0, #30
    lsrs r0, r0, #30
    ldr r1, [r4, #0x14]
    lsls r1, r1, #30
    lsrs r1, r1, #28
    orrs r0, r1
    ldr r1, [r5]
    ands r1, r6
    lsrs r1, r1, #2
    orrs r0, r1
    ldr r1, [r5, #4]
    ands r1, r6
    lsrs r1, r1, #1
    orrs r0, r1
    cmp r0, r7
    bne clc_change
clc_next:
    subs r2, r2, #1
    bne clc_loop
clc_done:
    pop {r6}
    bx lr
clc_change:
    mov r1, r8
    ldr r1, [r1, #4]                   @ Timer0 count
    stmia r3!, {r0, r1}
    movs r7, r0
    cmp r3, r10
    bhs clc_done                       @ the log is full
    cmp r3, r9
    bne clc_next
    mov r2, r11                        @ the first change: the window starts
    b clc_next
    .align 2
    .arm

@ --- The unattended run of screen 15 ---
@
@ The memory card slot covers the buttons of the device. Thus DOWN arms this
@ run before the device goes into the slot, and the run then needs no button.
@ It waits for the docking level (INTC STATUS bit 11), lets the connector
@ settle, and makes COM_CAPTURES captures: CTRL1 = 3 for captures 0 to 5, 1
@ for capture 6, and 2 for capture 7. It keeps each one in WRAM_COM_TABLE. A capture that no access
@ came to is made again, for as long as the device stays in the slot. The run
@ ends when the device leaves the slot, and the screen then shows capture 0.
@ UP then shows the next capture.

    .global com_auto_run
com_auto_run:
    push {r4, r5, lr}
    ldr r4, =INTC_STATUS

    mov r5, #0                         @ every entry: no capture yet
car_clear:
    mov r0, r5
    bl com_clear_entry
    add r5, r5, #1
    cmp r5, #COM_CAPTURES
    blo car_clear

car_wait_dock:
    ldr r0, [r4]
    tst r0, #INT_IOP_BIT
    beq car_wait_dock
    ldr r0, =COM_DOCK_SETTLE           @ the connector bounces for a short time
car_settle:
    subs r0, r0, #1
    bne car_settle

    mov r5, #0
car_next:
    ldr r0, [r4]
    tst r0, #INT_IOP_BIT
    beq car_done                       @ the device left the slot
    ldr r0, =WRAM_COM_CTRL1_SEL
    str r5, [r0]                       @ the capture number (see run_com_capture)
    bl run_com_capture
    ldr r0, =WRAM_COM_INFO
    ldr r0, [r0]
    movs r0, r0, lsl #16
    beq car_next                       @ no access came: the same capture again
    mov r0, r5
    bl com_store_entry
    bl redraw_screen                   @ the capture just made
    add r5, r5, #1
    cmp r5, #COM_CAPTURES
    blo car_next

car_wait_undock:
    ldr r0, [r4]
    tst r0, #INT_IOP_BIT
    bne car_wait_undock
car_done:
    ldr r1, =WRAM_COM_PAGE
    mov r0, #0
    str r0, [r1]
    bl com_load_entry                  @ r0 = 0: show capture 0
    pop {r4, r5, lr}
    bx lr
    .ltorg

@ An entry is WRAM_COM_TIMES (8 words) and then WRAM_COM_INFO, which are the
@ next 9 words of WRAM in that order. r0 = entry number -> r0 = its address.
@ Uses r1.
com_entry_address:
    mov r1, #(COM_ENTRY_WORDS * 4)
    mul r0, r1, r0
    ldr r1, =WRAM_COM_TABLE
    add r0, r0, r1
    bx lr

@ r0 = entry number. Every slot 0, and the info word = the entry number << 24.
    .global com_clear_entry
com_clear_entry:
    push {r4, lr}
    mov r4, r0
    bl com_entry_address
    mov r1, #0
    mov r2, #(COM_ENTRY_WORDS - 1)
cce_loop:
    str r1, [r0], #4
    subs r2, r2, #1
    bne cce_loop
    mov r1, r4, lsl #24
    str r1, [r0]
    pop {r4, lr}
    bx lr

@ r0 = entry number. The capture just made, into the entry.
com_store_entry:
    push {lr}
    bl com_entry_address
    ldr r1, =WRAM_COM_TIMES
    mov r2, #COM_ENTRY_WORDS
cse_loop:
    ldr r3, [r1], #4
    str r3, [r0], #4
    subs r2, r2, #1
    bne cse_loop
    pop {lr}
    bx lr

@ r0 = entry number. The entry, into the words that screen 15 draws.
    .global com_load_entry
com_load_entry:
    push {lr}
    bl com_entry_address
    ldr r1, =WRAM_COM_TIMES
    mov r2, #COM_ENTRY_WORDS
cle_loop:
    ldr r3, [r0], #4
    str r3, [r1], #4
    subs r2, r2, #1
    bne cle_loop
    pop {lr}
    bx lr
    .ltorg
