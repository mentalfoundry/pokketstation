#ifndef PSEMU_IOP_H
#define PSEMU_IOP_H

#include <stdint.h>

#define IOP_REG_SPAN 0x10u

/* PocketStation IOP power-control register: a single underlying
   bitmask (IOP_DATA) written via
   two complementary write-only ports - IOP_STOP (+0x4) ORs bits in,
   IOP_START (+0x8) ANDs them out (a bit set = that subsystem is
   stopped). Bit 5 is documented as "Sound Enable (START=On, STOP=Off)" -
   audio must be enabled via both this bit AND DAC_CTRL bit0 (see dac.h);
   other bits (LED, IR) are not modeled since they don't affect emulated
   behavior. Read back via IOP_STAT, aliased to the same address as
   IOP_STOP (+0x4).

   Each byte writes straight into `data` at its own position (OR for
   STOP, AND-NOT for START) rather than accumulating into a scratch
   register and committing only once a full 32-bit store completes - an
   earlier version of this file did the latter, which was a real,
   confirmed bug: direct BIOS/app tracing (see docs/hardware-notes.md)
   showed real code writes these registers via single-byte stores, which
   never reached that word-complete gate and were silently discarded.
   Applying each byte immediately is equivalent to accumulate-then-commit
   for a 32-bit store anyway (OR/AND-NOT are per-bit and commute across
   bytes), so there was no actual need for the deferred-commit step. */
typedef struct iop {
    uint32_t data;
} iop_t;

#define IOP_BIT_SOUND_STOPPED 0x20u

void iop_init(iop_t *iop);
uint8_t iop_read8(iop_t *iop, uint32_t offset);
void iop_write8(iop_t *iop, uint32_t offset, uint8_t value);

/* True if bit5 (Sound Enable) is currently in its STARTED (enabled)
   state - i.e. NOT stopped. */
int iop_sound_enabled(const iop_t *iop);

#endif
