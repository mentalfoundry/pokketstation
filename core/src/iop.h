#ifndef PSEMU_IOP_H
#define PSEMU_IOP_H

#include <stdint.h>

#define IOP_REG_SPAN 0x10u

/* PocketStation IOP power-control register.
   A single underlying bitmask, IOP_DATA, is written via two complementary write-only ports.
   IOP_STOP (+0x4) ORs bits in. IOP_START (+0x8) ANDs bits out.
   A set bit means that subsystem is stopped.

   Bit 5 is documented as "Sound Enable (START=On, STOP=Off)".
   Audio needs both this bit set and DAC_CTRL bit0 set (see dac.h).
   Other bits (LED, IR) are not modeled, because they do not affect emulated behavior.
   Software reads the bitmask back via IOP_STAT, aliased to the same address as IOP_STOP (+0x4).

   Each byte writes straight into `data` at its own bit position: OR for STOP, AND-NOT for START.
   This emulator does not accumulate writes into a scratch register and commit only once a full 32-bit
   store completes.

   History: an earlier version of this file did use that scratch-and-commit approach.
   This was a confirmed bug: direct BIOS/app tracing (see docs/hardware-notes.md) showed real code writes
   these registers via single-byte stores.
   Single-byte stores never reached that word-complete gate, so they were silently discarded.
   Applying each byte immediately is equivalent to accumulate-then-commit for a full 32-bit store anyway,
   because OR/AND-NOT are per-bit operations that commute across bytes.
   There was no actual need for the deferred-commit step. */
typedef struct iop {
    uint32_t data;
} iop_t;

#define IOP_BIT_SOUND_STOPPED 0x20u

void iop_init(iop_t *iop);
uint8_t iop_read8(iop_t *iop, uint32_t offset);
void iop_write8(iop_t *iop, uint32_t offset, uint8_t value);

/* True if bit 5 (Sound Enable) is currently in its STARTED (enabled) state.
   This means the bit is not stopped. */
int iop_sound_enabled(const iop_t *iop);

#endif
