#ifndef PSEMU_CLK_H
#define PSEMU_CLK_H

#include <stdint.h>

#define CLK_REG_SPAN 0x8u

/* CPU/timer clock speed control, confirmed against real hardware behavior:
   writing `mode` (bits 0-3, index into CPU_FREQ below)
   reprograms the ARM7's real oscillator rate; reading `mode` ORs in a
   "steady" bit (0x10) that a real BIOS boot loop polls before proceeding.
   Real hardware ties timer/RTC/DAC-bit-banging rates to this same shared
   clock, so all of them speed up and slow down together - see
   clk_current_hz() and psemu_run()'s use of it. */
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
