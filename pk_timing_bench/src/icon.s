@ Browse-screen icon region (Title Sector body offset 0x100-0x2FF, 512
@ bytes) - see ../../docs/app-notes.md's "Browse-screen icon/graphic"
@ section for the full reverse-engineered format and how this was found.
    .syntax unified
    .arm

    .section .icon, "ax"
    .global _icon_start

_icon_start:
    @ word@0x100: flags/count word - real example value from a working
    @ real app. Exact bit meaning not yet decoded (see docs/app-notes.md).
    .word 0x00010001
    @ word@0x104: absolute FLASH1 pointer to the 32x32 1bpp bitmap below
    @ (computed via the linker, not hardcoded).
    .word _icon_start + 0x80
    .space 0x80 - (. - _icon_start)   @ 0x108-0x17F: reserved/padding (zero in every real app inspected)

    @ 0x180-0x1FF (128 bytes): the actual 32x32 1bpp bitmap - byte-for-byte
    @ the same packing LCD VRAM itself uses (bit0=leftmost pixel). Our own
    @ stopwatch design, not a real app's icon.
    .incbin "../assets/stopwatch_bitmap.bin"

    @ 0x200-0x2FF (256 bytes): still UNIDENTIFIED, and non-zero in every real
    @ app inspected. The nine reference dumps on hand hold between 93 and 224
    @ nonzero bytes here. The contents differ per app, so this is not one fixed
    @ structure. Some hold structured save records. Others hold further
    @ 128-byte 32x32 1bpp icon frames, or frame data.
    @
    @ Zero-filled here rather than embedding another real app's actual bytes
    @ (the region was originally sourced verbatim from a real reference app
    @ during development, and confirmed working on real hardware in that form) -
    @ zero-filling instead, for a clean/shareable build with no third-party game
    @ data baked in.
    @
    @ INVESTIGATED AND RULED OUT as the cause of a real-hardware app-select
    @ screen glitch: filling this region with two copies of this app's own icon
    @ bitmap changed what garbage the glitched screen showed but did not fix it.
    @ The actual cause was the block-count byte at header offset 0x03 being left
    @ at 0 - see header.s. Kept zero-filled, since nothing now suggests the
    @ contents matter for a single-frame icon.
    .space 0x200 - (. - _icon_start)

.if (. - _icon_start) != 0x200
    .error "icon section is not exactly 0x200 (512) bytes"
.endif
