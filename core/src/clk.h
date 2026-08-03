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

/* `control` (+0x4) bit 0 is a stop/standby request: it halts the CPU, and with it everything else clocked
   from the same oscillator, until an external signal wakes it. psemu_run implements the stop; see its
   comment for which sources wake it and which peripherals keep running.

   Two separate claims here, at two different evidence levels.

   THE BEHAVIOUR IS CONFIRMED ON REAL HARDWARE. A real retail unit running the app described below blanks
   its screen about 37 seconds after the last button press, sleeps, and comes back on the next press. This
   emulator has to stop the CPU somewhere for that to happen at all.

   WHICH REGISTER DOES IT IS INFERRED, not confirmed. This register is undocumented, and this project has no
   way to probe it directly. The app writes 1 here as the final step of an unmistakable power-down sequence,
   in this order:

     IOP_STOP      = 0x62          sound and other IOP subsystems off
     INTC mask     = 0x200         RTC interrupt disabled
     LCD_MODE     &= ~0x48         DISON cleared - display off
     CLK control   = 1             <- this

   and then runs a short delay loop and returns. Nothing else in that sequence could stop the CPU, and
   treating this write as the stop reproduces the confirmed behaviour exactly.

   Leaving it inert - which is what this emulator did - is not a harmless simplification. The app's idle
   countdown is only written back AFTER its sleep call returns, so a CPU that keeps running re-enters the
   same tick, reads the same expired count, and calls sleep again. That recursion is unbounded at 28 bytes a
   level, and the app has only 388 bytes of stack before it reaches its own globals. It overruns them,
   corrupts a saved return address, and the CPU ends up executing data. See docs/hardware-notes.md,
   "CLK control (0x0B000004): stop/standby". */
int clk_stop_requested(const clk_t *clk);
void clk_clear_stop(clk_t *clk);

#endif
