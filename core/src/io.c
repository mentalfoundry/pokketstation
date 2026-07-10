#include "io.h"

void io_init(io_t *io) {
    io->buttons = 0;
    io->int_latch = 0;
}

void io_set_buttons(io_t *io, uint32_t buttons) {
    io->buttons = buttons;
}

uint32_t io_read_input(io_t *io) {
    return (io->buttons & 0x1Fu) | INT_INPUT_READY_BIT;
}

uint32_t io_read_latch(io_t *io) {
    return io->int_latch;
}

void io_write_latch(io_t *io, uint32_t value) {
    io->int_latch = value;
}
