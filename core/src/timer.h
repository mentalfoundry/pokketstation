#ifndef PSEMU_TIMER_H
#define PSEMU_TIMER_H

#include <stdint.h>

struct intc;

/* Real PocketStation timer block: 3 independent timers.
   Confirmed against real hardware behavior; see docs/hardware-notes.md.

   Each timer occupies 16 bytes at 0x0A800000 + n*0x10:
   - period (+0x0)
   - count (+0x4)
   - control (+0x8)

   On expiry, count reloads from period.
   The timer then asserts INT_TIMERn through the interrupt controller.

   control bits 0-1 select a clock divisor: 0 or 3 = /2, 1 = /32, 2 = /512.
   This matches real hardware's timer-start behavior and the documented "0=Div2, 1=Div32, 2=Div512, 3=Div2 too"
   description.
   control bit 2 enables/runs the timer.

   History: an earlier version of this file did not model the divisor.
   It decremented count by raw cycles regardless of divisor.
   This was a confirmed bug: it made any timer using a slower divisor fire far too often relative to real hardware.

   Not modeled: a read-side decrement of a running timer's count by 1, as a side effect of software reading it.
   Documented behavior describes plain "current value (decrementing)" reads, with no mention of a read-side
   side effect.
   This looks like an unconfirmed quirk, not confirmed real hardware behavior.
   This emulator deliberately does not model it. */
#define TIMER_COUNT 3u
#define TIMER_BLOCK_SIZE 0x10u
#define TIMER_REG_SPAN (TIMER_COUNT * TIMER_BLOCK_SIZE)
#define TIMER_CTRL_ENABLE (1u << 2)
#define TIMER_CTRL_DIVIDER_MASK 0x3u

typedef struct {
    uint32_t period;
    uint32_t count;
    uint32_t control;
    uint32_t cycle_accumulator; /* raw cycles not yet consumed by the divisor */
} single_timer_t;

typedef struct timer {
    single_timer_t timers[TIMER_COUNT];
} timer_t;

void timer_init(timer_t *timer);
uint8_t timer_read8(timer_t *timer, uint32_t offset);
void timer_write8(timer_t *timer, uint32_t offset, uint8_t value);

/* Advances every running timer by `cycles`.
   Asserts the matching INT_TIMERn line through `intc` on each expiry. */
void timer_tick(timer_t *timer, struct intc *intc, uint32_t cycles);

#endif
