@ Title Sector header (body offset 0x00-0xFF) - see ../../docs/app-notes.md's
@ "App file format: PSX Title Sector" section for the full container format.
    .syntax unified
    .arm

    .section .header, "ax"
    .global _header_start

_header_start:
    .ascii "SC"                 @ 0x00-0x01: Title Sector magic
    .byte 0x11                  @ 0x02: standard PS1 icon-display flag - 0x11 =
                                 @ 1 frame, no animation (0x12/0x13 = 2/3-frame
                                 @ animated icons). This is what a real PS1
                                 @ console's own memory-card manager (or PC
                                 @ tools) actually check before rendering the
                                 @ icon at 0x60/0x80 below - confirmed by diffing
                                 @ a real icon-embedding tool's output against
                                 @ this project's own build (this byte was
                                 @ already 0x11 in both, unchanged).
    .byte 1                     @ 0x03: block count. It holds how many 8KB blocks this
                                 @ save occupies. This app is exactly one block
                                 @ (BODY_SIZE = 0x2000, see pack.c).
                                 @
                                 @ NOT reserved, which is what this byte was
                                 @ previously assumed to be and why it was left
                                 @ at 0. Every real app on hand sets it to its
                                 @ true block count. The reference dumps on hand
                                 @ cover 1-, 2-, 4-, 7- and 13-block saves, and
                                 @ each one declares its own. pk_timing_bench was the
                                 @ only file in the corpus declaring 0.
                                 @
                                 @ Declaring 0 blocks corrupted the real BIOS's
                                 @ app-select screen: LEFT/RIGHT browse between
                                 @ saves normally, but UP transitioned into a
                                 @ glitched screen (DOWN transitioned back to the
                                 @ correctly-rendered icon), consistent with the
                                 @ browse UI deriving that vertical axis from this
                                 @ count and walking off the end of a save it had
                                 @ been told was zero blocks long. A reference
                                 @ dump with byte-for-byte the same container shape
                                 @ (1 block, 0x2000, chain link 0xFFFF) renders
                                 @ cleanly, which is what isolated this field.

    @ 0x04-0x4F (76 bytes): title text field, 2-byte Shift-JIS. Real
    @ hardware testing showed the on-device browse screen doesn't actually
    @ read this field at all (see ../README.md / docs/app-notes.md's
    @ "Browse-screen icon/graphic" section) - kept correct anyway since
    @ it's the documented container format and costs nothing.
    @ "PK TIMING BENCH", full-width SJIS (space=0x8140, 'A'+n=0x8260+n) -
    @ precomputed once (see pk_timing_bench's git history for the encoder
    @ that produced this), not derived at build time.
    .byte 0x82, 0x6F, 0x82, 0x6A, 0x81, 0x40, 0x82, 0x73, 0x82, 0x68
    .byte 0x82, 0x6C, 0x82, 0x68, 0x82, 0x6D, 0x82, 0x66, 0x81, 0x40
    .byte 0x82, 0x61, 0x82, 0x64, 0x82, 0x6D, 0x82, 0x62, 0x82, 0x67
    .space 0x50 - (. - _header_start)

    .byte 0, 0                   @ 0x50-0x51: PocketStation-specific icon frame-count/flags (unused by the on-device browse renderer - see icon.s)
    .ascii "MCX0"                 @ 0x52-0x55: PocketStation app identifier. MCX0, not MCX1 - an
                                    @ MCX1 conversion was tried to reserve space for SVC #9's
                                    @ snapshot write (see ui.s's poll_buttons) but broke the
                                    @ real BIOS's own browse-screen icon rendering for this app
                                    @ (confirmed: icon bytes identical between builds, garbled
                                    @ only when identified as MCX1 - a real BIOS format-handling
                                    @ difference this project hasn't decoded, not a data bug).
                                    @ Reverted after confirming SVC #9 departs just as cleanly on
                                    @ plain MCX0 in a real end-to-end test - the earlier "wild
                                    @ execution" finding that motivated MCX1 was a synthetic-test
                                    @ artifact (injected Thumb call, artificial CPU state), not a
                                    @ real problem. See ../README.md's "Return to system" section.
    .byte 1                       @ 0x56: 0x01 in every real app inspected, across all
                                    @ nine reference dumps on hand, regardless of
                                    @ block count, icon style or frame count.
                                    @ pk_timing_bench was the only file in the corpus
                                    @ leaving it 0, because this byte fell inside the
                                    @ zero-fill that runs from the end of the "MCX0"
                                    @ magic up to the entry point.
                                    @
                                    @ Exact meaning undecoded. It is NOT a frame
                                    @ count, because it stays 1 for 1-, 2-, 3- and
                                    @ 4-frame icons alike. It is most likely a format
                                    @ or version marker the browse UI checks. Set to 1 to match
                                    @ every real app; see ../README.md's
                                    @ "Real-hardware findings".
    .space 0x5C - (. - _header_start)
    .word _start                  @ 0x5C: entry point, bit0=0 (ARM state - _start is ARM code)

    @ 0x60-0x7F: standard PS1 memory-card icon palette (16 x BGR555, LE).
    @ 0x80-0xFF: standard PS1 memory-card icon bitmap (16x16, 4bpp, low
    @ nibble = left pixel of each byte's pair).
    @ This is a DIFFERENT icon from the PocketStation-specific browse icon
    @ at 0x100+ (see icon.s) - real PS1 hardware/software (a console's own
    @ memory-card manager, or PC-side memory-card management tools) render
    @ THIS one; the PocketStation device itself never reads it. Converted
    @ at build time from assets/card_icon.bmp by icon_convert.c - see
    @ ../README.md for the format and how it was reverse-engineered.
    .incbin "../assets/card_icon.bin"

.if (. - _header_start) != 0x100
    .error "header section is not exactly 0x100 bytes"
.endif
