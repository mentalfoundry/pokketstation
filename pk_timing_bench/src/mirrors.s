@ ARM and Thumb "mirror" blobs, placed at fixed FLASH1 offsets (0x300 and
@ 0x31C) so they can be copied into WRAM at runtime (see start.s) and timed
@ identically to their real-BIOS counterparts, from a region (WRAM) with
@ known, fast access timing.
    .syntax unified

    .section .arm_mirror, "ax"
    .arm
    .global arm_mirror_start

@ Byte-identical to the real BIOS routine at 0x04001BC8 (the "get selected
@ app slot" helper) - verified against a real J110 BIOS dump. Reads
@ *(u16*)0xD0 if nonzero, else *(u8*)0xCE, returning it in r0.
arm_mirror_start:
    mov r0, #0xd0
    ldrh r0, [r0]
    cmp r0, #0
    bxne lr
    mov r0, #0xce
    ldrb r0, [r0]
    bx lr
arm_mirror_end:

.if (arm_mirror_end - arm_mirror_start) != 28
    .error "arm_mirror blob is not 28 bytes"
.endif

    .section .thumb_mirror, "ax"
    .thumb
    .balign 4   @ satisfies the assembler's static PC-relative-load safety check;
                @ link.ld already places this section at 0x0200031C, which is
                @ word-aligned, so this is a no-op in the final placement.
    @ Deliberately NOT .thumb_func: this symbol is only ever used as a
    @ memcpy_words SOURCE pointer (start.s copies it into WRAM), never as a
    @ direct BX branch target by its own name - the WRAM copy is what gets
    @ jumped to (as WRAM_THUMB_MIRROR|1, a plain runtime constant). Tagging
    @ it .thumb_func would set bit0 on every reference to it (correct for a
    @ real branch target, via the standard ARM/Thumb interworking
    @ convention), which would make "ldr r1, =thumb_mirror_start" load an
    @ ODD address - memcpy_words would then read from a misaligned source,
    @ silently corrupting every byte it copies into WRAM. Caught by
    @ comparing this build's emulator results against the known-good
    @ Python-built reference before ever touching real hardware.
    .global thumb_mirror_start

@ Our own compact reproduction of the real BIOS routine's actual dynamic
@ path at 0x04001320 (r0=0, a directory-marker check guaranteed not to
@ match - see docs/app-notes.md's "App-selection and dispatch").
thumb_mirror_start:
    push {r1, r2}
    ldr r2, [pc, #20]
    adds r1, r0, #0
    lsls r1, r1, #7
    adds r1, r1, r2
    ldr r1, [r1]
    cmp r1, #0x51
    bne 1f
1:
    movs r0, #0
    mvns r0, r0
    pop {r1, r2}
    bx lr
    .word 0x08000000            @ literal read by "ldr r2, [pc, #20]" above
thumb_mirror_end:

.if (thumb_mirror_end - thumb_mirror_start) != 28
    .error "thumb_mirror blob is not 28 bytes"
.endif
