#ifndef PSEMU_IO_H
#define PSEMU_IO_H

#include <stdint.h>

/* Bit 9 of INT_INPUT is polled by the real BIOS during early boot
   (LDR/LSR#10/BLO loop) as some kind of hardware-ready flag distinct from
   the button bits (0-4) - address and semantics not in any sourced doc,
   only reverse-engineered from that wait loop. Modeled as always set,
   purely to get past the observed boot sequence. */
#define INT_INPUT_READY_BIT (1u << 9)

typedef struct io {
    uint32_t buttons; /* current PSEMU_BUTTON_* bitmask */
    uint32_t int_latch;
} io_t;

void io_init(io_t *io);
void io_set_buttons(io_t *io, uint32_t buttons);
uint32_t io_read_input(io_t *io);
uint32_t io_read_latch(io_t *io);
void io_write_latch(io_t *io, uint32_t value);

#endif
