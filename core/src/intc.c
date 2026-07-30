#include "intc.h"

#include <stdio.h>

#include "cpu.h"

/* Diagnostic flag; see cpu.h's psemu_debug_current_pc.
   Off by default, so it costs nothing in normal use.
   tools/inspect.c's `intctrace` flag turns it on, to log every real INTC access with its real PC.
   Static disassembly cannot reliably tell ARM from Thumb code without tracking runtime mode.
   This flag is permanent diagnostic infrastructure, not tied to any single investigation. */
int psemu_intc_trace_enabled = 0;

static const char *offset_name(uint32_t word_index) {
    switch (word_index) {
    case 0:
        return "hold";
    case 1:
        return "status";
    case 2:
        return "enable";
    case 3:
        return "mask";
    default:
        return "ack";
    }
}

void intc_init(intc_t *intc) {
    intc->hold = 0;
    intc->status = 0;
    intc->enable = 0;
    intc->mask = 0;
    intc->enable_write_scratch = 0;
    intc->mask_write_scratch = 0;
    intc->ack_write_scratch = 0;
}

uint8_t intc_read8(intc_t *intc, uint32_t offset) {
    uint32_t word_index = offset / 4u;
    uint32_t shift = (offset % 4u) * 8u;
    uint32_t value;

    switch (word_index) {
    case 0:
        value = intc->hold;
        break;
    case 1:
        value = intc->status;
        break;
    case 2:
        value = intc->enable;
        break;
    default: /* mask (+0xC) and acknowledge (+0x10) read back as 0 on real hardware */
        value = 0;
        break;
    }
    if (psemu_intc_trace_enabled) {
        printf(
            "[intc trace] pc=0x%08X READ %s (+0x%X) byte@shift%u = 0x%02X (full=0x%08X)\n",
            psemu_debug_current_pc, offset_name(word_index), (unsigned)offset, (unsigned)shift,
            (unsigned)((value >> shift) & 0xFFu), value);
    }
    return (uint8_t)(value >> shift);
}

static void accumulate_byte(uint32_t *scratch, uint32_t shift, uint8_t value) {
    *scratch = (*scratch & ~(0xFFu << shift)) | ((uint32_t)value << shift);
}

void intc_write8(intc_t *intc, uint32_t offset, uint8_t value) {
    uint32_t word_index = offset / 4u;
    uint32_t shift = (offset % 4u) * 8u;

    if (psemu_intc_trace_enabled) {
        printf(
            "[intc trace] pc=0x%08X WRITE %s (+0x%X) byte@shift%u = 0x%02X\n", psemu_debug_current_pc,
            offset_name(word_index), (unsigned)offset, (unsigned)shift, (unsigned)value);
    }

    switch (word_index) {
    case 0: /* hold: invalid write on real hardware, no effect */
    case 1: /* status: invalid write on real hardware, no effect */
        break;
    case 2: /* enable: OR bits in once a full 32-bit store completes */
        accumulate_byte(&intc->enable_write_scratch, shift, value);
        if (shift == 24u) {
            intc->enable |= intc->enable_write_scratch;
        }
        break;
    case 3: /* mask: clears matching enable bits, latches the mask value */
        accumulate_byte(&intc->mask_write_scratch, shift, value);
        if (shift == 24u) {
            intc->enable &= ~intc->mask_write_scratch;
            intc->mask = intc->mask_write_scratch;
        }
        break;
    default: /* acknowledge (+0x10): clears matching bits from hold, and from status except live-level bits */
        accumulate_byte(&intc->ack_write_scratch, shift, value);
        if (shift == 24u) {
            intc->hold &= ~intc->ack_write_scratch;
            /* INT_LEVEL_MASK bits are a continuously-driven signal level, not a latched request.
               An acknowledge must not wipe them. See INT_LEVEL_MASK's comment in intc.h. */
            intc->status &= ~(intc->ack_write_scratch & ~INT_LEVEL_MASK);
        }
        break;
    }
}

void intc_set_line(intc_t *intc, uint32_t line, int state) {
    if (line == 0) {
        return;
    }
    if (state) {
        /* Every asserted source latches into `hold`.
           This drives real IRQ delivery via hold & enable & INT_IRQ_MASK.
           STATUS_MASK bits (buttons, RTC) also latch into `status`, for direct polling.

           History: this code originally put STATUS_MASK bits into `status` only, never into `hold`.
           Disassembling the real BIOS confirmed this was wrong.
           The BIOS's top-level IRQ handler tests `hold & enable & 0x200` (RTC).
           Its installed periodic callback tests `hold & 1` (Action button).
           Both land on real handlers (RTC ack plus day-rollover bookkeeping; see docs/hardware-notes.md) that
           never ran under the old status-only routing.
           Under that old routing, buttons and RTC could never interrupt-deliver at all.
           Code could only see them by polling `status` directly. */
        intc->hold |= line;
        intc->status |= line & INT_STATUS_MASK;
    } else {
        /* History: an earlier version made only STATUS follow de-assertion here.
           The reasoning: INT_INPUT ("Raw Interrupt Signal Levels") is distinct from INT_LATCH
           ("Interrupt Request Flags"), and the real RTC handler does explicitly acknowledge its own bit.

           Real-hardware tracing disproved this: the real button-action callback, traced at 0x04003784, never
           acknowledges bit 0. Leaving `hold` latched forever after a button release caused the CPU to
           re-enter the IRQ handler on nearly every subsequent instruction after a single press.
           One test measured 559034 re-entries in 20 million instructions.
           A real, usable device does not behave this way.

           Buttons clear `hold` on release, the same as `status`.
           Only RTC's real handler also acks explicitly.
           This is harmless either way, because ack already clears both `hold` and `status`. */
        intc->status &= ~line;
        intc->hold &= ~line;
    }
}

uint32_t intc_get_line(intc_t *intc, uint32_t line) {
    return intc->status & line;
}

void intc_clear_hold_only(intc_t *intc, uint32_t line) {
    intc->hold &= ~line;
}

void intc_set_level_and_pulse(intc_t *intc, uint32_t line, int level) {
    if (line == 0) {
        return;
    }
    /* Every edge latches an interrupt request, in both directions.
       See this function's comment in intc.h. */
    intc->hold |= line;
    if (level) {
        intc->status |= line & INT_STATUS_MASK;
    } else {
        intc->status &= ~line;
    }
}

int intc_irq_asserted(intc_t *intc) {
    return (intc->hold & intc->enable & INT_IRQ_MASK) != 0;
}

int intc_fiq_asserted(intc_t *intc) {
    return (intc->hold & intc->enable & INT_FIQ_MASK) != 0;
}
