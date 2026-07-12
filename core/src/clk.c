#include "clk.h"

#define CLK_STEADY 0x10u

/* Confirmed via an earlier, unconfirmed source's CPU-frequency table - indices 7-15 all
   alias the same maximum rate (~7.995MHz); index 0 is the low-power idle
   rate (~63.5kHz). */
static const uint32_t CPU_FREQ[16] = {
    0x00f800u, 0x01f000u, 0x03e000u, 0x07c000u, 0x0f8000u, 0x1e8000u, 0x3d0000u, 0x7a0000u,
    0x7a0000u, 0x7a0000u, 0x7a0000u, 0x7a0000u, 0x7a0000u, 0x7a0000u, 0x7a0000u, 0x7a0000u,
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
