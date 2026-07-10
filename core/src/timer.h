#ifndef PSEMU_TIMER_H
#define PSEMU_TIMER_H

#include <stdint.h>

#define TIMER_CTRL_ENABLE (1u << 0)
#define TIMER_CTRL_IRQ_ENABLE (1u << 1)
#define TIMER_REG_SPAN 12u /* count, reload, ctrl: 3 x 32-bit registers */

/* Register layout (count @ +0, reload @ +4, ctrl @ +8) is a best-effort
   approximation, not sourced from primary PocketStation documentation -
   see docs/hardware-notes.md. Revise once real register details surface. */
typedef struct timer {
    uint32_t count;
    uint32_t reload;
    uint32_t ctrl;
} timer_t;

void timer_init(timer_t *timer);
uint8_t timer_read8(timer_t *timer, uint32_t offset);
void timer_write8(timer_t *timer, uint32_t offset, uint8_t value);

/* Advances the timer by `cycles`; returns nonzero exactly when it just
   underflowed with IRQs enabled. */
int timer_tick(timer_t *timer, uint32_t cycles);

#endif
