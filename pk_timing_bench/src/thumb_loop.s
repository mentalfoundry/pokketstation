@ Thumb-mode twin of measure_loop_ptr (helpers.s) - used by experiment 1's
@ ARM-vs-Thumb sanity check. Placed in its own fixed section so main code
@ can call it via its own real symbol (tagged .thumb_func so the linker
@ automatically sets bit0 on any reference to it, for correct interworking).
    .syntax unified
    .thumb

    .section .thumb_loop, "ax"
    .thumb_func
    .global measure_loop_ptr_thumb

measure_loop_ptr_thumb:      @ r0=ptr, r1=count -> r0=delta
    push {r4, r5}
    mov r6, lr              @ low<-high MOV: a genuinely distinct Thumb-1 encoding, fine as "mov"
    adds r5, r0, #0         @ low<-low "MOV": ARM7TDMI's real Thumb-1 has no dedicated encoding for
                             @ this, only this always-flag-setting ADD idiom (Thumb-1's 3-bit-immediate
                             @ ADD always sets flags, hence "adds" not "add") - as -mcpu=arm7tdmi enforces this.
    ldr r4, =0x0A800004
    ldr r2, [r4]
1:
    ldr r3, [r5]
    subs r1, r1, #1
    bne 1b
    ldr r3, [r4]
    subs r0, r2, r3
    @ Mask to 16 bits (Thumb has no immediate AND, so shift-left-16 then
    @ logical-shift-right-16 instead) - see ../README.md's "Timer0 wraps at
    @ 0x10000" finding: Timer0's real COUNT wraps at 0x10000, not
    @ 0x100000000. This particular loop hasn't shown the wraparound
    @ artifact in practice (it's fast), but masking is a safe no-op for any
    @ value under 65536, so apply it here too for consistency.
    lsls r0, r0, #16
    lsrs r0, r0, #16
    pop {r4, r5}
    bx r6
