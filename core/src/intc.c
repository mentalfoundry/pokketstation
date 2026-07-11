#include "intc.h"

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
    return (uint8_t)(value >> shift);
}

static void accumulate_byte(uint32_t *scratch, uint32_t shift, uint8_t value) {
    *scratch = (*scratch & ~(0xFFu << shift)) | ((uint32_t)value << shift);
}

void intc_write8(intc_t *intc, uint32_t offset, uint8_t value) {
    uint32_t word_index = offset / 4u;
    uint32_t shift = (offset % 4u) * 8u;

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
        intc->status |= line & INT_STATUS_MASK;
        intc->hold |= line & ~INT_STATUS_MASK;
    } else {
        intc->status &= ~line;
        intc->hold &= ~line;
    }
}

uint32_t intc_get_line(intc_t *intc, uint32_t line) {
    return intc->status & line;
}

int intc_irq_asserted(intc_t *intc) {
    return (intc->hold & intc->enable & INT_IRQ_MASK) != 0;
}

int intc_fiq_asserted(intc_t *intc) {
    return (intc->hold & intc->enable & INT_FIQ_MASK) != 0;
}
