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
@ Screen index used for the continue/exit prompt (see ui.s's poll_buttons
@ and redraw_screen) - distinct from the 5 normal result screens and the
@ screen 5 diagnostic screen. Reaching FIRE_HOLD_THRESHOLD switches to
@ this screen instead of exiting immediately, so the user can choose
@ CONTINUE or EXIT - enabling usrs to quit gracefully (it shows
@ its own bespoke continue/exit choice before actually departing,
@ rather than exiting unconditionally on a hold).
.equ SCREEN_EXIT_PROMPT,        6
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

.equ TIMER0_BASE,   0x0A800000
.equ TIMER0_PERIOD, TIMER0_BASE + 0x0
.equ TIMER0_COUNT,  TIMER0_BASE + 0x4
.equ TIMER0_CTRL,   TIMER0_BASE + 0x8

.equ INTC_BASE,   0x0A000000
.equ INTC_STATUS, INTC_BASE + 0x4
.equ INTC_MASK,   INTC_BASE + 0xC

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
