/* SPDX-FileCopyrightText: Copyright (c) 2026 Darien Liu (mentalfoundry)
   SPDX-License-Identifier: MIT */

#include "lcd.h"

#include <string.h>

/* Reverses the order of the 32 bits: bit 0 exchanges with bit 31, and so on. Each step exchanges
   groups of half the size of the step before. */
static uint32_t lcd_reverse_bits32(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

/* Calculates the part of `presented` that one scanline of VRAM gives. A scanline is 4 bytes. */
static void lcd_present_row(lcd_t *lcd, uint32_t row) {
    uint32_t rows = LCD_VRAM_SIZE / 4u;
    const uint8_t *src = &lcd->vram[row * 4u];

    if (!(lcd->mode & LCD_MODE_DISON)) {
        memset(&lcd->presented[row * 4u], 0, 4u);
        return;
    }
    if (!(lcd->mode & LCD_MODE_ROT)) {
        memcpy(&lcd->presented[row * 4u], src, 4u);
        return;
    }

    /* ROT: rotate the display 180 degrees.
       Reverse the order of the scanlines, and reverse the 32 pixels in each scanline. */
    {
        uint32_t src_word =
            (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        uint32_t reversed = lcd_reverse_bits32(src_word);
        uint8_t *dst = &lcd->presented[((rows - 1u) - row) * 4u];
        dst[0] = (uint8_t)reversed;
        dst[1] = (uint8_t)(reversed >> 8);
        dst[2] = (uint8_t)(reversed >> 16);
        dst[3] = (uint8_t)(reversed >> 24);
    }
}

static void lcd_recompute_presented(lcd_t *lcd) {
    uint32_t row;
    for (row = 0; row < LCD_VRAM_SIZE / 4u; row++) {
        lcd_present_row(lcd, row);
    }
}

void lcd_init(lcd_t *lcd) {
    memset(lcd->vram, 0, sizeof(lcd->vram));
    lcd->mode = LCD_MODE_DISON;
    lcd->cal = 0;
    lcd->dirty = 0;
    lcd_recompute_presented(lcd);
}

uint8_t lcd_read8(lcd_t *lcd, uint32_t offset) {
    return lcd->vram[offset % LCD_VRAM_SIZE];
}

void lcd_write8(lcd_t *lcd, uint32_t offset, uint8_t value) {
    /* A write changes one scanline, thus only that scanline of `presented` changes. */
    lcd->vram[offset % LCD_VRAM_SIZE] = value;
    lcd_present_row(lcd, (offset % LCD_VRAM_SIZE) / 4u);
    lcd->dirty = 1;
}

uint8_t lcd_mode_read8(lcd_t *lcd, uint32_t offset) {
    uint32_t word_index = offset / 4u;
    uint32_t shift = (offset % 4u) * 8u;
    uint32_t reg = (word_index == 1u) ? lcd->cal : lcd->mode;
    return (uint8_t)(reg >> shift);
}

void lcd_mode_write8(lcd_t *lcd, uint32_t offset, uint8_t value) {
    uint32_t word_index = offset / 4u;
    uint32_t shift = (offset % 4u) * 8u;

    if (word_index == 1u) { /* LCD_CAL: a stored value with no known side effect */
        lcd->cal = (lcd->cal & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        return;
    }

    lcd->mode = (lcd->mode & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    lcd_recompute_presented(lcd);
    lcd->dirty = 1;
}
