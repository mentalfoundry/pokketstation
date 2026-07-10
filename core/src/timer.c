#include "timer.h"

void timer_init(timer_t *timer) {
    timer->count = 0;
    timer->reload = 0;
    timer->ctrl = 0;
}

static uint32_t *timer_reg_ptr(timer_t *timer, uint32_t reg_index) {
    switch (reg_index % 3u) {
    case 0:
        return &timer->count;
    case 1:
        return &timer->reload;
    default:
        return &timer->ctrl;
    }
}

uint8_t timer_read8(timer_t *timer, uint32_t offset) {
    uint32_t *reg = timer_reg_ptr(timer, offset / 4u);
    return (uint8_t)(*reg >> ((offset % 4u) * 8u));
}

void timer_write8(timer_t *timer, uint32_t offset, uint8_t value) {
    uint32_t *reg = timer_reg_ptr(timer, offset / 4u);
    uint32_t shift = (offset % 4u) * 8u;
    *reg = (*reg & ~(0xFFu << shift)) | ((uint32_t)value << shift);
}

int timer_tick(timer_t *timer, uint32_t cycles) {
    if (!(timer->ctrl & TIMER_CTRL_ENABLE) || timer->reload == 0) {
        return 0;
    }

    int underflowed = 0;
    while (cycles > 0) {
        if (timer->count <= cycles) {
            cycles -= timer->count;
            timer->count = timer->reload;
            underflowed = 1;
        } else {
            timer->count -= cycles;
            cycles = 0;
        }
    }

    return underflowed && (timer->ctrl & TIMER_CTRL_IRQ_ENABLE);
}
