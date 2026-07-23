#include "intc.h"

#include <stdio.h>

#include "cpu.h"

/* Diagnostic flag - see cpu.h's psemu_debug_current_pc. Off by default so
   it costs nothing in normal use; tools/inspect.c's `intctrace` flag turns
   it on to log every real INTC access with its real PC, since static
   disassembly can't reliably tell ARM from Thumb code without tracking
   runtime mode. Kept as permanent diagnostic infrastructure, not tied to
   any single investigation. */
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
    default: /* acknowledge (+0x10): clears matching bits from hold and status */
        accumulate_byte(&intc->ack_write_scratch, shift, value);
        if (shift == 24u) {
            intc->hold &= ~intc->ack_write_scratch;
            intc->status &= ~intc->ack_write_scratch;
        }
        break;
    }
}

void intc_set_line(intc_t *intc, uint32_t line, int state) {
    if (line == 0) {
        return;
    }
    if (state) {
        /* Every asserted source latches into `hold` (this drives real IRQ
           delivery via hold & enable & INT_IRQ_MASK) - STATUS_MASK bits
           (buttons, RTC) ALSO latch into `status` for direct polling.
           Earlier this only put STATUS_MASK bits into `status`, NEVER
           `hold` - confirmed wrong by disassembling the real BIOS: its
           top-level IRQ handler tests `hold & enable & 0x200` (RTC) and
           its installed periodic callback tests `hold & 1` (Action
           button), both landing on real handlers (RTC ack + day-rollover
           bookkeeping; see docs/hardware-notes.md) that never ran under
           the old status-only routing - buttons/RTC could never
           interrupt-deliver at all, only be seen by code that happened
           to poll `status` directly. */
        intc->hold |= line;
        intc->status |= line & INT_STATUS_MASK;
    } else {
        /* Tried making only STATUS follow de-assertion here (INT_INPUT
           "Raw Interrupt Signal Levels" is distinct from INT_LATCH
           "Interrupt Request Flags", and the real RTC handler
           does explicitly acknowledge its own bit) - but disproved
           empirically: the real button-action callback (traced at
           0x04003784) never acknowledges bit 0, so making a button
           release leave `hold` latched forever caused the CPU to
           re-enter the IRQ handler on nearly every subsequent
           instruction after a single press (559034 re-entries in 20M
           instructions in one test - clearly not how a real, usable
           device behaves). Buttons evidently clear `hold` on release
           same as `status`; only RTC's real handler happens to also
           ack explicitly, which is harmless here either way since ack
           already clears both. */
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

int intc_irq_asserted(intc_t *intc) {
    return (intc->hold & intc->enable & INT_IRQ_MASK) != 0;
}

int intc_fiq_asserted(intc_t *intc) {
    return (intc->hold & intc->enable & INT_FIQ_MASK) != 0;
}
