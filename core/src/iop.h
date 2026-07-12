#ifndef PSEMU_IOP_H
#define PSEMU_IOP_H

#include <stdint.h>

#define IOP_REG_SPAN 0x10u

/* PocketStation IOP power-control register, confirmed via the documented documentation: a single underlying bitmask (IOP_DATA) written via
   two complementary write-only ports - IOP_STOP (+0x4) ORs bits in,
   IOP_START (+0x8) ANDs them out (a bit set = that subsystem is
   stopped). Bit 5 is documented as "Sound Enable (START=On, STOP=Off)" -
   audio must be enabled via both this bit AND DAC_CTRL bit0 (see dac.h);
   other bits (LED, IR) are not modeled since they don't affect emulated
   behavior. Read back via IOP_STAT, aliased to the same address as
   IOP_STOP (+0x4). */
typedef struct iop {
    uint32_t data;
    uint32_t stop_write_scratch;
    uint32_t start_write_scratch;
} iop_t;

#define IOP_BIT_SOUND_STOPPED 0x20u

void iop_init(iop_t *iop);
uint8_t iop_read8(iop_t *iop, uint32_t offset);
void iop_write8(iop_t *iop, uint32_t offset, uint8_t value);

/* True if bit5 (Sound Enable) is currently in its STARTED (enabled)
   state - i.e. NOT stopped. */
int iop_sound_enabled(const iop_t *iop);

#endif
