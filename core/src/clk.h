#ifndef PSEMU_CLK_H
#define PSEMU_CLK_H

#include <stdint.h>

#define CLK_REG_SPAN 0x8u

/* CPU/timer clock speed control.
   Confirmed against real hardware behavior.

   Writing `mode` (bits 0-3, an index into CPU_FREQ below) reprograms the ARM7's real oscillator rate.
   Reading `mode` ORs in a "steady" bit (0x10). A real BIOS boot loop polls this bit before proceeding.

   Real hardware ties the timer, RTC, and DAC-bit-banging rates to this same shared clock.
   All of them speed up and slow down together.
   See clk_current_hz() and psemu_run()'s use of it. */
typedef struct clk {
    uint32_t mode;
    uint32_t control;
    uint32_t mode_write_scratch;
    uint32_t control_write_scratch;
} clk_t;

void clk_init(clk_t *clk);
uint8_t clk_read8(clk_t *clk, uint32_t offset);
void clk_write8(clk_t *clk, uint32_t offset, uint8_t value);

/* Effective CPU frequency in Hz for the currently-programmed mode. */
uint32_t clk_current_hz(const clk_t *clk);

#endif
