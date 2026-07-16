#include "iop.h"

void iop_init(iop_t *iop) {
    iop->data = 0;
}

uint8_t iop_read8(iop_t *iop, uint32_t offset) {
    uint32_t shift = (offset % 4u) * 8u;
    /* Only IOP_STAT (+0x4, aliased with IOP_STOP) and IOP_DATA (+0xC)
       are documented as readable; IOP_CTRL (+0x0) and IOP_START (+0x8)
       are write-only/undocumented on read, so just return the current
       bitmask for anything in this span - harmless, since no known BIOS
       code reads +0x0 or +0x8. */
    return (uint8_t)(iop->data >> shift);
}

void iop_write8(iop_t *iop, uint32_t offset, uint8_t value) {
    uint32_t word_index = offset / 4u;
    uint32_t shift = (offset % 4u) * 8u;

    if (word_index == 1u) { /* IOP_STOP: ORs this byte's bits into data immediately */
        iop->data |= ((uint32_t)value << shift);
    } else if (word_index == 2u) { /* IOP_START: ANDs this byte's bits out of data immediately */
        iop->data &= ~((uint32_t)value << shift);
    } else if (word_index == 3u) { /* IOP_DATA: not used by the real BIOS, but harmless to store directly */
        iop->data = (iop->data & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    }
    /* word_index 0 (IOP_CTRL): no known effect, not modeled. */
}

int iop_sound_enabled(const iop_t *iop) {
    return (iop->data & IOP_BIT_SOUND_STOPPED) == 0u;
}
