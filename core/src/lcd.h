#ifndef PSEMU_LCD_H
#define PSEMU_LCD_H

#include <stdint.h>

#define LCD_VRAM_SIZE 128u

/* VRAM is already the packed 1bpp framebuffer: 32 rows of 4 bytes each,
   bit0 = leftmost pixel, 0 = white / 1 = black. */
typedef struct lcd {
    uint8_t vram[LCD_VRAM_SIZE];
    int dirty;
} lcd_t;

void lcd_init(lcd_t *lcd);
uint8_t lcd_read8(lcd_t *lcd, uint32_t offset);
void lcd_write8(lcd_t *lcd, uint32_t offset, uint8_t value);

#endif
