#include "ir.h"

void ir_init(ir_t *ir) {
    ir->mode = 0;
    ir->data = 0;
}

uint32_t ir_read(ir_t *ir, uint32_t offset) {
    return offset == 0 ? ir->mode : ir->data;
}

void ir_write(ir_t *ir, uint32_t offset, uint32_t value) {
    if (offset == 0) {
        ir->mode = value;
    } else {
        ir->data = value;
    }
}
