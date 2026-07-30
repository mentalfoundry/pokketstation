@ LCD drawing primitives and the 5-screen UI (button polling + screen
@ selection). See ../README.md for the "byte-wide access" real-hardware
@ finding behind draw_pixel/draw_glyph's word-wide-only bus access.
    .syntax unified
    .arm

    .include "constants.inc"

    .section .text, "ax"
    .global draw_pixel
    .global draw_glyph
    .global draw_hex_u32
    .global redraw_screen
    .global poll_buttons
    .global screen_next
    .global screen_prev
    .global draw_hline
    .global draw_vline
    .global draw_rect_border
    .global draw_continue_icon
    .global draw_exit_icon
    .global draw_selection_box

draw_pixel:               @ r0=row, r1=col -> sets VRAM bit (OR)
    @ Word-wide (32-bit) read-modify-write ONLY - never touch VRAM at byte
    @ granularity. Real VRAM hardware does not tolerate byte-wide access
    @ (confirmed the hard way - see ../README.md): a byte store lands
    @ across the whole containing word instead of just the target bit.
    @ Each VRAM row is exactly one 32-bit word, so row*4 is always
    @ word-aligned and col (0-31) is directly the bit position within that
    @ word on a little-endian CPU - no per-byte splitting needed at all.
    push {r2, r3, r4, lr}
    mov r2, r0, lsl #2
    ldr r3, =LCD_VRAM_ADDR
    add r2, r2, r3
    mov r4, #1
    mov r4, r4, lsl r1
    ldr r3, [r2]
    orr r3, r3, r4
    str r3, [r2]
    pop {r2, r3, r4, lr}
    bx lr
    .ltorg

draw_glyph:                @ r0=value(0-15), r1=row0, r2=col0
    @ Font table lives in FLASH1, which only tolerates 16/32-bit reads, not
    @ 8-bit (confirmed the hard way - see ../README.md). Word-align down,
    @ LDR the containing word, then extract the target byte with a shift -
    @ never issue an 8-bit bus access to FLASH1.
    push {r4, r5, r6, r7, r8, r9, r10, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
    ldr r7, =font_table
    add r8, r4, r4, lsl #2
    add r7, r7, r8
    mov r8, #0
dg_row_loop:
    cmp r8, #5
    bge dg_done
    add r10, r7, r8
    and r9, r10, #3
    mov r9, r9, lsl #3
    bic r10, r10, #3
    ldr r10, [r10]
    mov r9, r10, lsr r9
    tst r9, #4
    beq dg_skip0
    add r0, r5, r8
    add r1, r6, #0
    bl draw_pixel
dg_skip0:
    tst r9, #2
    beq dg_skip1
    add r0, r5, r8
    add r1, r6, #1
    bl draw_pixel
dg_skip1:
    tst r9, #1
    beq dg_skip2
    add r0, r5, r8
    add r1, r6, #2
    bl draw_pixel
dg_skip2:
    add r8, r8, #1
    b dg_row_loop
dg_done:
    pop {r4, r5, r6, r7, r8, r9, r10, lr}
    bx lr
    .ltorg

draw_hex_u32:                @ r0=value32, r1=row0, r2=col0
    push {r4, r5, r6, r7, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
    mov r7, #8
duh_loop:
    cmp r7, #0
    beq duh_done
    mov r0, r4, lsr #28
    and r0, r0, #0xF
    mov r1, r5
    mov r2, r6
    bl draw_glyph
    mov r4, r4, lsl #4
    add r6, r6, #4
    sub r7, r7, #1
    b duh_loop
duh_done:
    pop {r4, r5, r6, r7, lr}
    bx lr
    .ltorg

redraw_screen:
    push {r4, r5, r6, lr}
    ldr r0, =LCD_VRAM_ADDR
    mov r1, #0
    mov r2, #32
clr_loop:
    str r1, [r0], #4
    subs r2, r2, #1
    bne clr_loop

    ldr r0, =WRAM_SCREEN_INDEX
    ldr r4, [r0]

    cmp r4, #SCREEN_EXIT_PROMPT
    beq rs_exit_prompt

    mov r0, r4
    mov r1, #0
    mov r2, #0
    bl draw_glyph

    cmp r4, #5
    beq rs_diag_screen
    cmp r4, #6
    beq rs_timer_screen
    cmp r4, #7
    beq rs_irq_screen
    cmp r4, #8
    beq rs_rearm_screen

    sub r5, r4, #1
    lsl r5, r5, #3
    ldr r6, =WRAM_RESULTS_BASE
    add r5, r5, r6

    ldr r0, [r5]
    mov r1, #9
    mov r2, #0
    bl draw_hex_u32

    ldr r0, [r5, #4]
    mov r1, #17
    mov r2, #0
    bl draw_hex_u32
    b rs_done

rs_exit_prompt:
    @ Bespoke continue/exit choice screen - a play-arrow icon for CONTINUE,
    @ an X icon for EXIT, with a hollow box drawn around whichever is
    @ currently selected (see draw_continue_icon/draw_exit_icon/
    @ draw_selection_box below, and ../README.md's "Return to system"
    @ section for why this exists instead of exiting unconditionally).
    bl draw_continue_icon
    bl draw_exit_icon
    ldr r6, =WRAM_EXIT_PROMPT_SELECTION
    ldr r0, [r6]
    bl draw_selection_box
    b rs_done

rs_timer_screen:
    @ Experiment 6: raw Timer0 stopwatch totals for a fixed number of Timer2
    @ reloads, at two Timer2 periods. Same two-row layout as screens 1-4:
    @ top = period 1016 x 256 reloads, bottom = period 2032 x 128 reloads.
    ldr r5, =WRAM_TIMER_RESULT_A
    ldr r0, [r5]
    mov r1, #9
    mov r2, #0
    bl draw_hex_u32

    ldr r5, =WRAM_TIMER_RESULT_B
    ldr r0, [r5]
    mov r1, #17
    mov r2, #0
    bl draw_hex_u32
    b rs_done

rs_irq_screen:
    @ Experiment 7: the same measurement loop timed with interrupts masked
    @ (top) and with one timer interrupt live (bottom). Both read 0 when
    @ experiment 7 did not run. See start.s.
    ldr r5, =WRAM_IRQ_BASELINE
    ldr r0, [r5]
    mov r1, #9
    mov r2, #0
    bl draw_hex_u32

    ldr r5, =WRAM_IRQ_WITH_IRQ
    ldr r0, [r5]
    mov r1, #17
    mov r2, #0
    bl draw_hex_u32
    b rs_done

rs_rearm_screen:
    @ Experiment 8: Timer0 stopwatch ticks across EXP8_INTERRUPTS re-armed
    @ Timer1 periods (top), and the period each one was armed with (bottom).
    ldr r5, =WRAM_REARM_DELTA
    ldr r0, [r5]
    mov r1, #9
    mov r2, #0
    bl draw_hex_u32

    ldr r0, =EXP8_TIMER_PERIOD
    mov r1, #17
    mov r2, #0
    bl draw_hex_u32
    b rs_done

rs_diag_screen:
    @ Diagnostic screen: raw (not delta) Timer0 snapshots, 4 rows -
    @ single-call before/after, then full-30000-loop before/after.
    ldr r5, =WRAM_DIAG_SINGLE_BEFORE
    ldr r0, [r5]
    mov r1, #7
    mov r2, #0
    bl draw_hex_u32

    ldr r5, =WRAM_DIAG_SINGLE_AFTER
    ldr r0, [r5]
    mov r1, #13
    mov r2, #0
    bl draw_hex_u32

    ldr r5, =WRAM_DIAG_BEFORE
    ldr r0, [r5]
    mov r1, #19
    mov r2, #0
    bl draw_hex_u32

    ldr r5, =WRAM_DIAG_AFTER
    ldr r0, [r5]
    mov r1, #25
    mov r2, #0
    bl draw_hex_u32

rs_done:
    pop {r4, r5, r6, lr}
    bx lr
    .ltorg

poll_buttons:                @ -> r0 = 1 if screen changed else 0
    push {r4, r5, r6, lr}
    ldr r4, =INTC_STATUS
    ldr r5, [r4]              @ r5 = this frame's raw button bits - kept live
                               @ for the whole function (bit 0x01 is
                               @ INT_BTN_ACTION's real hardware bit, per
                               @ core/src/intc.h; 0x02=Right, 0x04=Left,
                               @ 0x08=Down, 0x10=Up)

    ldr r6, =WRAM_SCREEN_INDEX
    ldr r4, [r6]               @ r4 = current screen index

    cmp r4, #SCREEN_EXIT_PROMPT
    beq pb_in_prompt

    @ --- normal (non-prompt) mode: hold Action -> switch to this app's
    @ own bespoke continue/exit prompt (see redraw_screen's rs_exit_prompt
    @ and draw_continue_icon/draw_exit_icon/draw_selection_box below)
    @ instead of exiting unconditionally - matching a real reference app's
    @ own behavior (it lets the user choose CONTINUE vs EXIT before actually
    @ departing - see ../README.md's "Return to system" section for the
    @ full investigation). ---
    tst r5, #0x01
    beq pb_reset_hold
    ldr r6, =WRAM_FIRE_HOLD_COUNTER
    ldr r0, [r6]
    add r0, r0, #1
    str r0, [r6]
    ldr r1, =FIRE_HOLD_THRESHOLD
    cmp r0, r1
    blt pb_check_right
    mov r0, #0
    str r0, [r6]                          @ reset hold counter
    ldr r6, =WRAM_SAVED_SCREEN_INDEX
    str r4, [r6]                          @ remember the screen to resume
    ldr r6, =WRAM_EXIT_PROMPT_SELECTION
    mov r0, #0
    str r0, [r6]                          @ default selection: CONTINUE
    ldr r6, =WRAM_SCREEN_INDEX
    mov r0, #SCREEN_EXIT_PROMPT
    str r0, [r6]
    mov r1, #1
    b pb_store
pb_reset_hold:
    ldr r6, =WRAM_FIRE_HOLD_COUNTER
    mov r0, #0
    str r0, [r6]

pb_check_right:
    ldr r6, =WRAM_BUTTON_DEBOUNCE
    ldr r0, [r6]

    tst r5, #2
    beq pb_check_left
    tst r0, #2
    bne pb_check_left
    bl screen_next
    mov r1, #1
    b pb_store
pb_check_left:
    tst r5, #4
    beq pb_none
    tst r0, #4
    bne pb_none
    bl screen_prev
    mov r1, #1
    b pb_store
pb_none:
    mov r1, #0
    b pb_store

    @ --- continue/exit prompt mode: Down selects EXIT, Up selects
    @ CONTINUE (defaults to CONTINUE - see the threshold-reached block
    @ above), Action confirms the current selection. Matches the real
    @ Down-then-Action convention a real reference app uses. ---
pb_in_prompt:
    ldr r6, =WRAM_BUTTON_DEBOUNCE
    ldr r0, [r6]               @ r0 = previous frame's raw button bits

    tst r5, #8                 @ Down -> select EXIT
    beq pb_prompt_check_up
    tst r0, #8
    bne pb_prompt_check_up
    ldr r6, =WRAM_EXIT_PROMPT_SELECTION
    mov r1, #1
    str r1, [r6]
    mov r1, #1
    b pb_store

pb_prompt_check_up:
    tst r5, #0x10               @ Up -> select CONTINUE
    beq pb_prompt_check_action
    tst r0, #0x10
    bne pb_prompt_check_action
    ldr r6, =WRAM_EXIT_PROMPT_SELECTION
    mov r1, #0
    str r1, [r6]
    mov r1, #1
    b pb_store

pb_prompt_check_action:
    tst r5, #0x01                @ Action tap -> confirm current selection
    beq pb_prompt_none
    tst r0, #0x01
    bne pb_prompt_none
    ldr r6, =WRAM_EXIT_PROMPT_SELECTION
    ldr r4, [r6]
    cmp r4, #1
    beq pb_prompt_confirm_exit
    @ CONTINUE confirmed: restore the screen that was showing before the
    @ prompt appeared.
    ldr r6, =WRAM_SAVED_SCREEN_INDEX
    ldr r4, [r6]
    ldr r6, =WRAM_SCREEN_INDEX
    str r4, [r6]
    mov r1, #1
    b pb_store
pb_prompt_confirm_exit:
    @ EXIT is confirmed on the Action PRESS edge, so at this point the button is
    @ still physically held down. Departing immediately hands control back to the
    @ system with a held Action, and the system's own browse screen reads that as
    @ a fresh press and relaunches this app at once.
    @
    @ Wait for the release first. INT_INPUT reports a live button level on real
    @ hardware, which is what makes this terminate: this app's own hold-to-open
    @ gesture already depends on that, since FIRE_HOLD_THRESHOLD counts 75000
    @ consecutive polls with Action held and only a live level can accumulate
    @ that. See docs/hardware-notes.md, "Buttons".
    @
    @ Confirmed end to end in the emulator, not only on real hardware: see
    @ tools/pk_exit_test.c. It confirms EXIT while holding Action, and checks that
    @ the CPU stays out of BIOS space for as long as Action stays down, then
    @ departs into BIOS space once it is released. Removing this wait loop makes
    @ that test fail exactly the way the real-hardware report described: it
    @ departs into BIOS space with Action still held.
    @
    @ The wait is bounded. A stuck or failing contact would otherwise spin here
    @ forever, and recovering from that needs the physical reset button. The
    @ bound is generous enough that no human press reaches it, so a normal
    @ release always wins the race and the timeout never fires in practice.
    ldr r6, =INTC_STATUS
    ldr r5, =EXIT_RELEASE_TIMEOUT
pb_exit_wait_release:
    ldr r4, [r6]
    tst r4, #0x01
    beq pb_exit_released
    subs r5, r5, #1
    bne pb_exit_wait_release
pb_exit_released:

    @ Then clear every latched button source, so no HOLD bit left over from this
    @ press or from the earlier hold survives into the system's own browse screen.
    @ Acknowledge (+0x10) clears both HOLD and STATUS for the bits written.
    ldr r6, =INTC_BASE
    mov r4, #0x1F                @ buttons are INTC bits 0-4
    str r4, [r6, #0x10]

    @ Now restore the stack to where real dispatch left
    @ it (User SP=0x800, per docs/app-notes.md) before departing - this
    @ poll_buttons call pushed {r4,r5,r6,lr} on entry and was never going
    @ to pop them (SVC #9 doesn't return), leaving SP at 0x7F0 instead of
    @ 0x800 for this routine's whole execution. Found by comparing full
    @ CPU register state against a real reference app's own compiled code
    @ at the equivalent point - see ../README.md's "Return to system" section.
    @ (core/src/arm_exec.c's exec_msr also had a related bug, since fixed:
    @ it let a User-mode "msr cpsr_c" actually disable IRQ/FIQ globally,
    @ when real ARM7TDMI silicon makes that a no-op from unprivileged
    @ mode - start.s's boot-time CPSR write was relying on that no-op and
    @ getting the opposite.)
    add sp, sp, #16
    @ The rest is the FULL departure sequence, cross-validated against
    @ a real reference app's own compiled code (it executes this identical
    @ shape immediately before permanently leaving FLASH1, not just the bare
    @ final SVC #9): SVC #0x11 (r0=0), then two raw MMIO
    @ writes that same real code makes here (INTC_MASK=0x3FFF, and
    @ TIMER0_BASE+0x8/+0x18/+0x28 all cleared to 0 - likely Timer0/1/2
    @ control), then SVC #0x16 (r0=0) whose return value feeds
    @ "r2 = r0 + 48" ahead of SVC #8 (r0=1, r1=0, r2=that computed
    @ value), and only then "mov r0,#0; svc #9". This app is plain MCX0
    @ (see header.s) - an MCX1 conversion was tried at one point on the
    @ theory that SVC #9's snapshot write needed reserved space, but that
    @ broke the real BIOS's own browse-icon rendering and turned out to
    @ be unnecessary; both real reference apps are MCX0 too.
    mov r0, #0
    svc #0x11
    ldr r0, =INTC_BASE
    ldr r1, =0x3FFF
    str r1, [r0, #0xC]
    @ Also reset Timer0's own PERIOD/COUNT (+0x0/+0x4), not just its
    @ control register - this app's own experiments configure Timer0 with
    @ an extreme benchmarking value (period=0xFFFFFFFF, see start.s) that
    @ that real reference app's own code never touches (its own Timer0 period read back
    @ as a normal, small value at the equivalent point, confirmed via a
    @ full machine-state diff, not just CPU registers - see ../README.md's
    @ "Return to system" section). Left at 0xFFFFFFFF, a real wrap/
    @ interrupt the kernel's own post-departure logic may depend on would
    @ never practically happen.
    ldr r0, =TIMER0_BASE
    mov r1, #0
    str r1, [r0, #0x0]
    str r1, [r0, #0x4]
    str r1, [r0, #0x8]
    str r1, [r0, #0x18]
    str r1, [r0, #0x28]
    mov r0, #0
    svc #0x16
    add r2, r0, #48
    mov r0, #1
    mov r1, #0
    svc #8
    mov r0, #0
    svc #9
    mov r1, #0
    b pb_store

pb_prompt_none:
    mov r1, #0

pb_store:
    ldr r6, =WRAM_BUTTON_DEBOUNCE
    str r5, [r6]
    mov r0, r1
    pop {r4, r5, r6, lr}
    bx lr
    .ltorg

screen_next:
    push {r0, r1, lr}
    ldr r0, =WRAM_SCREEN_INDEX
    ldr r1, [r0]
    add r1, r1, #1
    cmp r1, #8
    ble sn_store
    mov r1, #1
sn_store:
    str r1, [r0]
    pop {r0, r1, lr}
    bx lr
    .ltorg

screen_prev:
    push {r0, r1, lr}
    ldr r0, =WRAM_SCREEN_INDEX
    ldr r1, [r0]
    sub r1, r1, #1
    cmp r1, #1
    bge sp_store
    mov r1, #8
sp_store:
    str r1, [r0]
    pop {r0, r1, lr}
    bx lr
    .ltorg

@ --- Continue/exit prompt drawing primitives. Bespoke procedural icons
@ (not text - see ../README.md's "Return to system" section for why: this
@ app's font only has hex-digit glyphs 0-F, no letters), same procedural
@ approach as the browse-screen stopwatch icon. ---

draw_hline:                   @ r0=row, r1=col_start, r2=col_end (inclusive)
    push {r4, r5, r6, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
dh_loop:
    cmp r5, r6
    bgt dh_done
    mov r0, r4
    mov r1, r5
    bl draw_pixel
    add r5, r5, #1
    b dh_loop
dh_done:
    pop {r4, r5, r6, lr}
    bx lr
    .ltorg

draw_vline:                   @ r0=col, r1=row_start, r2=row_end (inclusive)
    push {r4, r5, r6, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
dv_loop:
    cmp r5, r6
    bgt dv_done
    mov r0, r5
    mov r1, r4
    bl draw_pixel
    add r5, r5, #1
    b dv_loop
dv_done:
    pop {r4, r5, r6, lr}
    bx lr
    .ltorg

draw_rect_border:             @ r0=row_top, r1=row_bottom, r2=col_left, r3=col_right
    push {r4, r5, r6, r7, lr}
    mov r4, r0
    mov r5, r1
    mov r6, r2
    mov r7, r3
    mov r0, r4
    mov r1, r6
    mov r2, r7
    bl draw_hline
    mov r0, r5
    mov r1, r6
    mov r2, r7
    bl draw_hline
    mov r0, r6
    mov r1, r4
    mov r2, r5
    bl draw_vline
    mov r0, r7
    mov r1, r4
    mov r2, r5
    bl draw_vline
    pop {r4, r5, r6, r7, lr}
    bx lr
    .ltorg

draw_continue_icon:           @ right-pointing play-arrow, rows 5-13, cols 8-12
    push {lr}
    mov r0, #5
    mov r1, #8
    mov r2, #8
    bl draw_hline
    mov r0, #6
    mov r1, #8
    mov r2, #9
    bl draw_hline
    mov r0, #7
    mov r1, #8
    mov r2, #10
    bl draw_hline
    mov r0, #8
    mov r1, #8
    mov r2, #11
    bl draw_hline
    mov r0, #9
    mov r1, #8
    mov r2, #12
    bl draw_hline
    mov r0, #10
    mov r1, #8
    mov r2, #11
    bl draw_hline
    mov r0, #11
    mov r1, #8
    mov r2, #10
    bl draw_hline
    mov r0, #12
    mov r1, #8
    mov r2, #9
    bl draw_hline
    mov r0, #13
    mov r1, #8
    mov r2, #8
    bl draw_hline
    pop {lr}
    bx lr
    .ltorg

draw_exit_icon:                @ X shape, rows 18-26, cols 8-16
    push {r4, r5, lr}
    mov r4, #0
dei_loop:
    cmp r4, #9
    bge dei_done
    mov r0, r4
    add r0, r0, #18
    mov r1, r4
    add r1, r1, #8
    bl draw_pixel              @ draw_pixel preserves r0/r4 across the call
    mov r5, #16
    sub r1, r5, r4
    bl draw_pixel
    add r4, r4, #1
    b dei_loop
dei_done:
    pop {r4, r5, lr}
    bx lr
    .ltorg

draw_selection_box:            @ r0 = selection (0=CONTINUE, 1=EXIT)
    push {lr}
    cmp r0, #1
    beq dsb_exit
    mov r0, #4
    mov r1, #14
    mov r2, #7
    mov r3, #13
    bl draw_rect_border
    b dsb_done
dsb_exit:
    mov r0, #17
    mov r1, #27
    mov r2, #7
    mov r3, #17
    bl draw_rect_border
dsb_done:
    pop {lr}
    bx lr
    .ltorg
