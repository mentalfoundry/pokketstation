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

    @ 0x200-0x2FF (256 bytes): real, nonzero, but still UNIDENTIFIED in
    @ every real app inspected so far (see ../README.md's "Known caveats").
    @ Zero-filled here rather than embedding another real app's actual
    @ bytes (this region was originally sourced verbatim from a real
    @ reference app during development, and confirmed working on real
    @ hardware in that form) - zero-filling instead, for a clean/shareable
    @ build with no third-party game data baked in, has NOT itself been
    @ re-verified on real hardware. If the browse-screen icon ever fails to
    @ render (or renders differently) on a real unit with this exact
    @ build, this trailer is the first thing to suspect.
    .space 0x200 - (. - _icon_start)

.if (. - _icon_start) != 0x200
    .error "icon section is not exactly 0x200 (512) bytes"
.endif
