#ifndef PSEMU_IR_H
#define PSEMU_IR_H

#include <stdint.h>

#define IR_REG_SPAN 0x8u

/* IRDA_MODE (offset 0) bits and IRDA_DATA (offset 4) bit 0, per the
   documented `PMIrMode`-style register layout:
     IRDA_MODE bit0 IFMODE  0=Receive, 1=Transmit
     IRDA_MODE bit1 STDBY   0=Active, 1=Stand-by
     IRDA_MODE bit2 BGEN    0=Enable 40KHz carrier generator, 1=Disable
     IRDA_MODE bit3 BFLT    0=Enable filter, 1=Disable
     IRDA_DATA bit0 LED     Transmit data in the send direction: 0=LED off, 1=LED on
   Real pulses are long/short ON separated by short OFF gaps (long ~2x
   short); the real RX-IRQ handler (INT_IRDA, see intc.h) measures an
   incoming pulse's length by reading Timer 2's live counter (reload
   0xFFFFh) at the interrupt.

   None of that pulse-level behavior is modeled here - there's no second
   PocketStation (or transport between two emulator instances) to
   actually exchange IR data with, and nothing in this project's own test
   corpus (Chocobo World, 200M+ instructions traced) has ever been
   observed touching these registers. This struct just stores the two
   registers under their real names with documented, spec-accurate bit
   layout (see the IR_MODE_xxx / IR_DATA_LED constants below) instead of
   an opaque raw latch, so a future session building real IR communication
   starts from correct register semantics rather than rediscovering them.
   INT_IRDA is never asserted from here for the same reason - firing it
   for real needs an actual incoming pulse to react to. */
#define IR_MODE_IFMODE (1u << 0)
#define IR_MODE_STDBY (1u << 1)
#define IR_MODE_BGEN (1u << 2)
#define IR_MODE_BFLT (1u << 3)
#define IR_DATA_LED (1u << 0)

typedef struct ir {
    uint32_t mode;
    uint32_t data;
} ir_t;

void ir_init(ir_t *ir);
uint32_t ir_read(ir_t *ir, uint32_t offset);
void ir_write(ir_t *ir, uint32_t offset, uint32_t value);

#endif
