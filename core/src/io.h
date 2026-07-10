#ifndef PSEMU_IO_H
#define PSEMU_IO_H

#include <stdint.h>

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
