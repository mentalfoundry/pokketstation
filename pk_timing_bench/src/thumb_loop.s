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

@ Registers r0 as the app's IRQ callback via SWI 1, for experiment 7. Passing 0
@ unregisters again.
@
@ This is the same call a real app uses to install its own interrupt
@ dispatcher, found by disassembling one: it fills a 16-entry handler table in
@ RAM, then issues "movs r0,#1; ldr r1,=dispatcher; svc #1" (and separately
@ "movs r0,#2; movs r1,#0; svc #1" to install no FIQ callback). So r0 selects
@ which callback slot and r1 is the handler address, with 0 meaning none.
@
@ Deliberately Thumb, and issued as "svc #1" from Thumb, matching that app
@ exactly - ARM and Thumb encode the SWI immediate differently, and there is no
@ evidence about how the kernel's handler reads it, so this copies the one form
@ known to work rather than assuming they are interchangeable.
@
@ SWI 1's contract here is INFERRED from that disassembly, not documented.
@ That is worth knowing if experiment 7 ever misbehaves on a real unit.
    .thumb_func
    .global register_irq_handler

register_irq_handler:        @ r0 = handler address, or 0 to unregister
    @ The return deliberately goes through BX, not "pop {..., pc}". On ARMv4T a
    @ Thumb POP into PC does NOT interwork. It loads the address, but the CPU
    @ stays in Thumb state.
    @ A pop straight into PC therefore returns into the ARM caller while the CPU
    @ is still in Thumb state, and the caller's ARM instructions then decode as
    @ garbage Thumb. That is exactly what happened the first time this was
    @ written: an "unrecognized thumb opcode 0xEBFF" fault, 0xEB being the top
    @ byte of the ARM BL the caller was really sitting on. measure_loop_ptr_thumb
    @ above avoids the same trap the same way.
    @ lr is kept on the stack rather than in a register, so it survives whatever
    @ the kernel's SWI handler does to the scratch registers.
    push {lr}
    adds r1, r0, #0              @ r1 = handler address (0 = unregister)
    movs r0, #1                  @ r0 = IRQ callback slot
    svc #1
    pop {r3}                     @ r3 = saved lr
    bx r3

@ Registers r0 as the app's FIQ callback via SWI 1, slot 2 - the other half of
@ the same mechanism register_irq_handler uses, per the comment above (slot 1
@ = IRQ, slot 2 = FIQ). Passing 0 unregisters again.
@
@ Confirmed the hard way, not just from the disassembly comment above:
@ experiment 10 (Timer2/FIQ re-arm latency) hung in this emulator when it
@ reused register_irq_handler (slot 1) for a FIQ-routed source. The BIOS's
@ FIQ vector handler (0x040014D4 in a real J-110 BIOS dump) reads its
@ callback from a DIFFERENT fixed RAM slot than the IRQ vector handler does -
@ confirmed directly from a disassembled BIOS dump, not inferred. With no
@ callback registered there, nothing ever acknowledged Timer2's HOLD bit, so
@ FIQ re-asserted immediately on return and the CPU never left the vector.
@ See experiments.s's run_experiment_10_fiq_rearm_latency.
    .thumb_func
    .global register_fiq_handler

register_fiq_handler:         @ r0 = handler address, or 0 to unregister
    push {lr}
    adds r1, r0, #0              @ r1 = handler address (0 = unregister)
    movs r0, #2                  @ r0 = FIQ callback slot
    svc #1
    pop {r3}                     @ r3 = saved lr
    bx r3
