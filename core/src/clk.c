#include "clk.h"

#define CLK_STEADY 0x10u

/* PMFrequency/CLK_MODE and SWI 04h SetCpuSpeed's argument table: FREQ 0 = 32.768kHz, doubling
   each step, FREQ 7 = ~4MHz, FREQ 8 and above all alias the max ~8MHz -
   a clean power-of-two PLL progression. An earlier version of this table
   used CPU-frequency values that were ~2x too high at every index
   (e.g. "mode 7" read ~7.995MHz, not the confirmed ~4MHz) - those higher numbers were
   never independently confirmed and shouldn't be relied on (see the
   note at the top of docs/hardware-notes.md). Tried setting mode 7 to
   8MHz instead, to compare against real hardware - the
   user reported it back out, so the confirmed ~4MHz value stands. */
static const uint32_t CPU_FREQ[16] = {
    32768u,   65536u,   131072u,  262144u,  524288u,  1048576u, 2097152u, 4194304u,
    8388608u, 8388608u, 8388608u, 8388608u, 8388608u, 8388608u, 8388608u, 8388608u,
};

void clk_init(clk_t *clk) {
    clk->mode = 0;
    clk->control = 0;
    clk->mode_write_scratch = 0;
    clk->control_write_scratch = 0;
}

uint8_t clk_read8(clk_t *clk, uint32_t offset) {
    uint32_t word_index = offset / 4u;
    uint32_t shift = (offset % 4u) * 8u;
    uint32_t value = (word_index == 0u) ? (clk->mode | CLK_STEADY) : clk->control;
    return (uint8_t)(value >> shift);
}

void clk_write8(clk_t *clk, uint32_t offset, uint8_t value) {
    uint32_t word_index = offset / 4u;
    uint32_t shift = (offset % 4u) * 8u;

    if (word_index == 0u) {
        clk->mode_write_scratch = (clk->mode_write_scratch & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        if (shift == 24u) {
            clk->mode = clk->mode_write_scratch;
        }
    } else {
        clk->control_write_scratch = (clk->control_write_scratch & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        if (shift == 24u) {
            clk->control = clk->control_write_scratch;
        }
    }
}

uint32_t clk_current_hz(const clk_t *clk) {
    return CPU_FREQ[clk->mode & 0x0Fu];
}
