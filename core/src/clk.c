#include "clk.h"

#define CLK_STEADY 0x10u

/* PMFrequency/CLK_MODE and SWI 04h SetCpuSpeed's argument table. Indices
   1-8 use the exact documented Hz values (see docs/hardware-notes.md's
   CLK_MODE section for the full cross-check and verification) rather
   than a clean power-of-two guess - note the table isn't a pure
   doubling ladder (index 5->6 only steps ~1.97x, not 2x), so
   approximating it as one is measurably wrong at several indices.
   Indices 9-15 alias index 8's rate, also per the documented table.

   Index 0 is left at its prior, real-hardware-confirmed-by-ear value
   (32768, treated as the idle default) rather than a documentation-
   derived number, because the documentation describes index 0 as
   "hangs hardware" (an invalid/reserved PLL setting) rather than giving
   it a frequency - there's nothing numeric to swap in. Confirmed
   harmless in practice (see docs/hardware-notes.md): a 20M-instruction
   real-BIOS trace never once writes CLK_MODE=0, only ever 7/4/3. An
   even earlier version of this table used values ~2x too high at every
   index (e.g. "mode 7" read ~7.995MHz) - independently disproved by
   real-hardware A/B testing (see docs/hardware-notes.md), unrelated to
   the documentation cross-check done here. */
static const uint32_t CPU_FREQ[16] = {
    32768u,   63488u,   126976u,  253952u,  507904u,  1015808u, 1998848u, 3997696u,
    7995392u, 7995392u, 7995392u, 7995392u, 7995392u, 7995392u, 7995392u, 7995392u,
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
