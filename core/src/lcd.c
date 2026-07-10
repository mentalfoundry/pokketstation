#include "lcd.h"

#include <string.h>

void lcd_init(lcd_t *lcd) {
    memset(lcd->vram, 0, sizeof(lcd->vram));
    lcd->dirty = 0;
}

uint8_t lcd_read8(lcd_t *lcd, uint32_t offset) {
    return lcd->vram[offset % LCD_VRAM_SIZE];
}

void lcd_write8(lcd_t *lcd, uint32_t offset, uint8_t value) {
    lcd->vram[offset % LCD_VRAM_SIZE] = value;
    lcd->dirty = 1;
}
