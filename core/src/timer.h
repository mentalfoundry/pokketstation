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

/* period and count are 16-bit registers, not 32-bit.
   Confirmed against real hardware: every raw `count` snapshot captured on a real unit had its
   upper 16 bits at zero, and a counter read before/after a long loop came back numerically
   larger, which only happens if the counter wrapped and reloaded at a 16-bit boundary.
   See docs/hardware-notes.md, "Timers".

   History: this emulator modeled both as full uint32_t. That silently broke any app whose
   timer setup wrote a value wider than 16 bits, because the surviving upper bits stretched
   the period enormously instead of being discarded.
   Confirmed via Pop'n Music (testdata/popnmusic.mcr): its FIQ-driven audio timer (Timer2)
   held period 0x03240353 under the 32-bit model, about 52.6 million ticks, so the audio
   interrupt effectively never fired and the game played in complete silence with its DAC
   gate open the whole time. Masking to the real 16-bit width leaves period 0x0353 (851),
   matching Timer1's 0x34F (847) programmed alongside it, and the music plays.
   See test_timer_registers_are_16_bit. */
#define TIMER_REG_MASK 0xFFFFu

typedef struct {
    uint32_t period;
    uint32_t count;
    uint32_t control;
    uint32_t cycle_accumulator; /* raw cycles not yet consumed by the divisor */
} single_timer_t;

/* Named psemu_timer_t, not timer_t: POSIX already declares timer_t in <sys/types.h>
   (via <time.h>/<stdlib.h>), so the shorter name is a redefinition error on every
   non-Windows target. MSVC has no POSIX timer_t, which is why this only ever broke
   the Linux/macOS/Android/webOS CI jobs and never the Windows ones. */
typedef struct timer {
    single_timer_t timers[TIMER_COUNT];
} psemu_timer_t;

void timer_init(psemu_timer_t *timer);
uint8_t timer_read8(psemu_timer_t *timer, uint32_t offset);
void timer_write8(psemu_timer_t *timer, uint32_t offset, uint8_t value);

/* Advances every running timer by `cycles`.
   Asserts the matching INT_TIMERn line through `intc` on each expiry. */
void timer_tick(psemu_timer_t *timer, struct intc *intc, uint32_t cycles);

#endif
