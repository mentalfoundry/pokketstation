#ifndef PSEMU_IR_H
#define PSEMU_IR_H

#include <stdint.h>

/* IR bit-level protocol/timing is unverified against real hardware - see
   docs/hardware-notes.md. This stub only tracks register writes so the
   memory map is complete and other components can be developed/tested. */
typedef struct ir {
    uint32_t mode;
    uint32_t beam;
} ir_t;

void ir_init(ir_t *ir);
uint32_t ir_read(ir_t *ir, uint32_t offset);
void ir_write(ir_t *ir, uint32_t offset, uint32_t value);

#endif
