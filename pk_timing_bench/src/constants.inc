@ Shared hardware/WRAM-layout constants for pk_timing_bench.
@ See ../../docs/hardware-notes.md for the MMIO register map, and
@ ../README.md for the WRAM scratch layout these addresses live in.

.equ WRAM_RESULTS_BASE,       0x200   @ 8 words: [testA,ctrlA, testB,ctrlB, testC,ctrlC, testD,ctrlD]
.equ WRAM_DATA_SCRATCH,       0x220
.equ WRAM_ARM_MIRROR,         0x224   @ 28 bytes (7 ARM instructions)
.equ WRAM_THUMB_MIRROR,       0x240   @ 28 bytes (12 thumb instrs + 4-byte literal)
.equ WRAM_SCREEN_INDEX,       0x25C
.equ WRAM_BUTTON_DEBOUNCE,    0x260
@ Counts consecutive poll_buttons calls with Action (Fire) held - see
@ ui.s's poll_buttons and ../README.md's "Return to system" section. Not
@ real-time-calibrated (main_loop has no fixed frame rate) - tuned
@ empirically against the emulator instead.
.equ WRAM_FIRE_HOLD_COUNTER,  0x264
@ Roughly a few seconds of continuous holding (empirically calibrated
@ against the emulator, since main_loop has no fixed frame rate) - matches
@ a real reference app's own real-hardware feel closely enough.
@ Not exact - adjust if it feels too short/long in real interactive use.
.equ FIRE_HOLD_THRESHOLD,     75000
@ Upper bound on the wait for Action to be released before this app departs
@ back to the system (see ui.s's pb_prompt_confirm_exit). A real release ends
@ that wait long before this, so this only exists so a stuck contact cannot
@ spin forever - recovering from that would need the physical reset button.
.equ EXIT_RELEASE_TIMEOUT,    2000000
@ Screen index used for the continue/exit prompt (see ui.s's poll_buttons
@ and redraw_screen) - distinct from the 5 normal result screens and the
@ screen 5 diagnostic screen. Reaching FIRE_HOLD_THRESHOLD switches to
@ this screen instead of exiting immediately, so the user can choose
@ CONTINUE or EXIT - enabling usrs to quit gracefully (it shows
@ its own bespoke continue/exit choice before actually departing,
@ rather than exiting unconditionally on a hold).
.equ SCREEN_EXIT_PROMPT,        9
@ 0 = CONTINUE selected, 1 = EXIT selected. Defaults to CONTINUE when the
@ prompt first appears, matching the real reference app.
.equ WRAM_EXIT_PROMPT_SELECTION, 0x268
@ The screen to restore if CONTINUE is chosen.
.equ WRAM_SAVED_SCREEN_INDEX,    0x26C

@ Diagnostic screen 5 (see ../README.md's "Timer0 wraps at 0x10000" finding) -
@ raw Timer0 snapshots, not deltas, around a single isolated real BIOS call
@ and around the full 30000-iteration measurement experiment 3 also does.
.equ WRAM_DIAG_SINGLE_BEFORE, 0x280
.equ WRAM_DIAG_SINGLE_AFTER,  0x284
.equ WRAM_DIAG_BEFORE,        0x288
.equ WRAM_DIAG_AFTER,         0x28C

@ Screen 6 (experiment 6) results: Timer0 stopwatch ticks elapsed across a
@ fixed number of Timer2 reloads, at two different Timer2 periods.
.equ WRAM_TIMER_RESULT_A,     0x290
.equ WRAM_TIMER_RESULT_B,     0x294

@ Screen 7 (experiment 7) results: the same measurement loop timed twice, once
@ with all interrupts masked and once with a single timer interrupt live.
.equ WRAM_IRQ_BASELINE,       0x298
.equ WRAM_IRQ_WITH_IRQ,       0x29C

@ Screen 8 (experiment 8) results, and the counter its handler increments.
.equ WRAM_REARM_DELTA,        0x2A0
.equ WRAM_REARM_COUNTER,      0x2A4

.equ TIMER0_BASE,   0x0A800000
.equ TIMER0_PERIOD, TIMER0_BASE + 0x0
.equ TIMER0_COUNT,  TIMER0_BASE + 0x4
.equ TIMER0_CTRL,   TIMER0_BASE + 0x8

@ Timer2 - the device under test for experiment 6 (see experiments.s). Timer
@ channels are 0x10 bytes apart.
.equ TIMER2_BASE,   TIMER0_BASE + 0x20
.equ TIMER2_PERIOD, TIMER2_BASE + 0x0
.equ TIMER2_COUNT,  TIMER2_BASE + 0x4
.equ TIMER2_CTRL,   TIMER2_BASE + 0x8
@ control value used by experiment 6: divisor bits 0 (= /2) | enable (bit 2).
@ Deliberately the exact control value a real IR-using app programs Timer2
@ with, so the measurement matches real usage rather than a synthetic case.
.equ TIMER2_CTRL_DIV2_ENABLE, 0x4

@ Experiment 6 periods. 1016 is not arbitrary: it is the exact period a real
@ IR-using app arms Timer2 with while transmitting (its nominal pulse unit of
@ 1200, minus a hardcoded 184-tick compensation it applies before arming).
@ 2032 is that doubled, so the two results together separate an ADDITIVE
@ per-period error (a fixed cost, e.g. latency) from a PROPORTIONAL one (a
@ rate/divisor error): an additive error shows the same absolute excess at
@ both periods, a proportional one shows double the excess at 2032.
@ Reload counts are chosen so both runs span a similar total, and so the
@ Timer0 stopwatch delta stays below its real 16-bit wrap (see README.md's
@ "Timer0 wraps at 0x10000" finding).
.equ EXP6_PERIOD_A,  1016
.equ EXP6_RELOADS_A, 256
.equ EXP6_PERIOD_B,  2032
.equ EXP6_RELOADS_B, 128

.equ INTC_BASE,   0x0A000000
.equ INTC_STATUS, INTC_BASE + 0x4
.equ INTC_ENABLE, INTC_BASE + 0x8
.equ INTC_MASK,   INTC_BASE + 0xC

@ Timer1 - the interrupt source experiment 7 briefly enables. Timer0 is the
@ stopwatch and Timer2 belongs to experiment 6, so Timer1 is the only free one.
.equ TIMER1_BASE,   TIMER0_BASE + 0x10
.equ INT_TIMER1_BIT, 0x100

@ Experiment 7 (see start.s). Timer1 at /32 with period 99 fires every
@ (99+1)*32 = 3200 raw cycles, so a LOOP_N-iteration run (~360000 raw cycles)
@ takes roughly 112 interrupts. A per-interrupt cost of a few hundred cycles
@ then changes the loop's duration by more than 10%, instead of disappearing
@ into measurement noise.
.equ EXP7_TIMER_PERIOD, 99
.equ EXP7_TIMER_CTRL,   0x5      @ divisor bits 1 (= /32) | enable
@ Raw cycles between Timer1 interrupts, for the screen 7 arithmetic in README.md.
.equ EXP7_IRQ_RAW_CYCLES, 3200

@ Experiment 8: how long from a timer expiring to its handler re-arming it.
@ 1016 and the /2 divisor are not arbitrary. They are exactly what a real
@ IR-using app arms Timer2 with while transmitting, and that app re-arms the
@ timer inside the handler on every interrupt rather than letting it free-run.
@ A re-armed timer does NOT hide interrupt latency the way a free-running one
@ does: with a free-running timer the latency is the same on every period and
@ cancels out, but re-arming adds it to every period. The app's own nominal
@ unit is 1200 ticks and it arms 1016, so it expects 184 ticks of latency.
@ 64 interrupts keeps the Timer0 stopwatch delta below its 16-bit wrap.
.equ EXP8_TIMER_PERIOD, 1016
.equ EXP8_TIMER_CTRL,   0x4      @ divisor bits 0 (= /2) | enable
.equ EXP8_INTERRUPTS,   64

.equ CLK_MODE_ADDR, 0x0B000000
.equ LCD_MODE_ADDR, 0x0D000000
.equ LCD_VRAM_ADDR, 0x0D000100

.equ FLASH_CTRL_TEST_ADDR, 0x06000100

@ Real BIOS ROM addresses (external to this app's own linked image - these
@ are fixed, hardcoded, NOT link-time symbols) - see docs/app-notes.md.
.equ BIOS_ARM_HELPER,        0x04001BC8
.equ BIOS_THUMB_HELPER_BIT1, 0x04001321   @ bit0 set = thumb call target

.equ LOOP_N, 30000

.equ FLASH1_BASE, 0x02000000
