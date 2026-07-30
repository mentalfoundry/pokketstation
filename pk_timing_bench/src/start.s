@ Entry point and main loop. See ../README.md for the full narrative of
@ what each fix here was responding to.
    .syntax unified
    .arm

    .include "constants.inc"

    .section .text, "ax"
    .global _start

_start:
    @ --- hardware safety net FIRST, before anything else: mask all interrupt
    @ sources via INTC_MASK (a plain MMIO write, not privilege-gated - works
    @ regardless of CPU mode). Ahead of the CPSR attempt below to shrink the
    @ real-hardware-only window where a stale BIOS timer/GUI-tick interrupt
    @ could still fire against RAM state our own dispatch has just zeroed -
    @ a real risk this project's own emulator can't reproduce. ---
    ldr r0, =INTC_MASK
    mvn r1, #0
    str r1, [r0]
    .ltorg

    @ --- disable IRQ/FIQ via CPSR too, for defense in depth (per ARM spec
    @ this is actually a no-op on real HW from unprivileged User mode, which
    @ is where the real BIOS always dispatches apps - kept anyway since it's
    @ harmless and the INTC_MASK write above is the real backstop) ---
    mrs r0, cpsr
    orr r0, r0, #0xC0
    msr cpsr_c, r0
    .ltorg

    @ --- LCD on: read-modify-write, NOT a blind overwrite. LCD_MODE's real
    @ pre-dispatch/POR value is undocumented - a bare "mov r1,#0x40; str"
    @ clobbers any other bit the real BIOS may already have set before
    @ jumping here (this is what caused this app's first real-hardware
    @ attempt to instantly blank the screen and hang - see ../README.md). ---
    ldr r0, =LCD_MODE_ADDR
    ldr r1, [r0]
    orr r1, r1, #0x40
    str r1, [r0]
    .ltorg

    @ checkpoint 0: LCD confirmed on, about to touch CLK_MODE
    mov r0, #0
    mov r1, #0
    bl draw_pixel

    @ --- CLK_MODE = 7, poll steady bit ---
    ldr r0, =CLK_MODE_ADDR
    mov r1, #7
    str r1, [r0]
clk_wait:
    ldr r1, [r0]
    tst r1, #0x10
    beq clk_wait
    .ltorg

    @ checkpoint 1: CLK_MODE reports steady
    mov r0, #1
    mov r1, #0
    bl draw_pixel

    @ --- Timer0: period=0xFFFFFFFF, control = divisor0 | enable (0x5) ---
    ldr r0, =TIMER0_BASE
    mvn r1, #0
    str r1, [r0]
    mov r1, #5
    str r1, [r0, #8]
    .ltorg

    @ checkpoint 2: Timer0 configured
    mov r0, #2
    mov r1, #0
    bl draw_pixel

    @ --- copy WRAM mirror blobs from FLASH1 data into WRAM ---
    ldr r0, =WRAM_ARM_MIRROR
    ldr r1, =arm_mirror_start
    mov r2, #7
    bl memcpy_words

    ldr r0, =WRAM_THUMB_MIRROR
    ldr r1, =thumb_mirror_start
    mov r2, #7
    bl memcpy_words
    .ltorg

    @ checkpoint 3: WRAM mirror blobs copied
    mov r0, #3
    mov r1, #0
    bl draw_pixel

    @ --- diagnostic: raw Timer0 before/after a single, isolated real BIOS
    @ ARM-helper call - the very FIRST thing to touch BIOS ROM, before any
    @ of the 4 experiments (which also stash their own raw before/after via
    @ measure_bios_call_loop_real, for the full 30000-iteration case) ---
    bl run_diagnostic_single_call

    @ --- run all 4 experiments once, with a checkpoint pixel after each ---
    bl run_experiment_1_sanity
    mov r0, #4
    mov r1, #0
    bl draw_pixel

    bl run_experiment_2_flashctrl
    mov r0, #5
    mov r1, #0
    bl draw_pixel

    bl run_experiment_3_bios_arm
    mov r0, #6
    mov r1, #0
    bl draw_pixel

    bl run_experiment_4_bios_thumb
    mov r0, #7
    mov r1, #0
    bl draw_pixel

    @ Experiment 6 (screen 6): Timer2 period semantics. It polls, and takes no
    @ interrupts. See experiments.s for what it separates, and why.
    bl run_experiment_6_timer_period
    mov r0, #8
    mov r1, #0
    bl draw_pixel
    .ltorg

    @ Experiment 7 (screen 7): what taking an interrupt actually costs. This is
    @ the only experiment here that un-masks an interrupt, so it registers a
    @ handler first. It then re-masks everything and restores Timer1 before it
    @ returns. See run_experiment_7_irq_latency in experiments.s.
    @
    @ Two failure modes came up while getting it working. This project's own
    @ emulator caught both of them before any hardware run. They are recorded
    @ here because either one would have corrupted or wedged a real unit, and
    @ returned no measurement:
    @
    @   1. Enabling a timer interrupt with NO app IRQ callback registered visibly
    @      corrupts the app. The kernel expects an app to have installed one
    @      first, hence the registration step.
    @   2. The registration helper first returned via "pop {..., pc}". On ARMv4T a
    @      Thumb POP into PC does not interwork, so it returned into ARM caller
    @      code while still in Thumb state and faulted on "unrecognized thumb
    @      opcode 0xEBFF" - 0xEB being the top byte of the ARM BL it landed on.
    @      It returns via BX now; see register_irq_handler in thumb_loop.s.
    @
    @ This runs unconditionally, like every other experiment here. An earlier
    @ version gated it behind holding UP at power-on, as a hedge against it
    @ hanging a unit. That gate caused more problems than it prevented: it
    @ depended on reading a live button LEVEL out of INTC_STATUS at startup,
    @ which real hardware does but this emulator only approximates (it latches
    @ button state on the press edge, and the BIOS's own boot-time interrupt
    @ acknowledges clear it again). The gate therefore behaved differently in
    @ the emulator than on hardware. Holding a direction button through launch
    @ also disturbs the BIOS's own menu navigation. Both failure modes above are
    @ now fixed rather than hidden behind a switch, so the hedge bought nothing.
    bl run_experiment_7_irq_latency
    mov r0, #9
    mov r1, #0
    bl draw_pixel
    .ltorg

    @ --- init UI state ---
    ldr r0, =WRAM_SCREEN_INDEX
    mov r1, #1
    str r1, [r0]
    ldr r0, =WRAM_BUTTON_DEBOUNCE
    mov r1, #0
    str r1, [r0]
    .ltorg

    bl redraw_screen

main_loop:
    bl poll_buttons
    cmp r0, #0
    beq main_loop
    bl redraw_screen
    b main_loop
