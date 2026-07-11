#include "timer.h"

#include "intc.h"

void timer_init(timer_t *timer) {
    uint32_t i;
    for (i = 0; i < TIMER_COUNT; i++) {
        timer->timers[i].period = 0;
        timer->timers[i].count = 0;
        timer->timers[i].control = 0;
        timer->timers[i].cycle_accumulator = 0;
    }
}

/* Control bits 0-1: 0 or 3 = /2, 1 = /32, 2 = /512 - confirmed matching
   real hardware's timer-start behavior and documented divider values. */
static uint32_t timer_divisor(uint32_t control) {
    switch (control & TIMER_CTRL_DIVIDER_MASK) {
    case 1:
        return 32u;
    case 2:
        return 512u;
    default: /* 0 and 3 */
        return 2u;
    }
}

uint8_t timer_read8(timer_t *timer, uint32_t offset) {
    uint32_t index = offset / TIMER_BLOCK_SIZE;
    uint32_t local = offset % TIMER_BLOCK_SIZE;
    uint32_t word_index = local / 4u;
    uint32_t shift = (local % 4u) * 8u;
    uint32_t value;

    if (index >= TIMER_COUNT) {
        return 0;
    }
    switch (word_index) {
    case 0:
        value = timer->timers[index].period;
        break;
    case 1:
        value = timer->timers[index].count;
        break;
    case 2:
        value = timer->timers[index].control;
        break;
    default:
        value = 0;
        break;
    }
    return (uint8_t)(value >> shift);
}

void timer_write8(timer_t *timer, uint32_t offset, uint8_t value) {
    uint32_t index = offset / TIMER_BLOCK_SIZE;
    uint32_t local = offset % TIMER_BLOCK_SIZE;
    uint32_t word_index = local / 4u;
    uint32_t shift = (local % 4u) * 8u;
    uint32_t *reg;

    if (index >= TIMER_COUNT) {
        return;
    }
    switch (word_index) {
    case 0:
        reg = &timer->timers[index].period;
        break;
    case 1:
        reg = &timer->timers[index].count;
        break;
    case 2:
        reg = &timer->timers[index].control;
        *reg = (*reg & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        /* Real hardware restarts the prescaler whenever control is
           rewritten (matches this codebase's own timer-start logic being
           re-invoked from its control-write handling on every control
           write) - drop any partial divisor progress so a mode/enable
           change starts from a clean edge. */
        timer->timers[index].cycle_accumulator = 0;
        return;
    default:
        return;
    }
    *reg = (*reg & ~(0xFFu << shift)) | ((uint32_t)value << shift);
}

void timer_tick(timer_t *timer, struct intc *intc, uint32_t cycles) {
    static const uint32_t int_lines[TIMER_COUNT] = {INT_TIMER0, INT_TIMER1, INT_TIMER2};
    uint32_t i;

    for (i = 0; i < TIMER_COUNT; i++) {
        single_timer_t *t = &timer->timers[i];
        uint32_t divisor;
        uint32_t ticks;

        if (!(t->control & TIMER_CTRL_ENABLE) || t->period == 0) {
            continue;
        }

        /* The timer's own count only decrements once per `divisor` raw
           cycles (control bits 0-1 select /2, /32, or /512) - confirmed
           against real hardware's timer-start behavior and its documented
           divider table. An earlier version of this
           function decremented count by raw cycles directly, ignoring
           the divisor entirely, which made any timer using a slower
           divisor fire far more often than real hardware. */
        divisor = timer_divisor(t->control);
        t->cycle_accumulator += cycles;
        ticks = t->cycle_accumulator / divisor;
        t->cycle_accumulator %= divisor;

        while (ticks > 0) {
            if (t->count <= ticks) {
                ticks -= t->count;
                t->count = t->period;
                intc_set_line(intc, int_lines[i], 1);
            } else {
                t->count -= ticks;
                ticks = 0;
            }
        }
    }
}
