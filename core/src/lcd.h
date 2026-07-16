#ifndef PSEMU_LCD_H
#define PSEMU_LCD_H

#include <stdint.h>

#define LCD_VRAM_SIZE 128u
/* LCD_MODE(+0x0, R/W) and LCD_CAL(+0x4). LCD_MODE bit6 is DISON (display on/off) and
   bit7 is ROT (rotate display 180 degrees, set for docked mode to match
   INT_INPUT.11's docking flag) - both real, previously entirely unmodeled
   (this address range had no bus handler at all; writes silently vanished
   and psemu_get_framebuffer always returned raw VRAM unconditionally). */
#define LCD_MODE_REG_SPAN 0x8u

#define LCD_MODE_DISON 0x40u
#define LCD_MODE_ROT 0x80u

/* VRAM is already the packed 1bpp framebuffer: 32 rows of 4 bytes each,
   bit0 = leftmost pixel, 0 = white / 1 = black. `presented` is the same
   format after applying LCD_MODE's DISON/ROT bits - what
   psemu_get_framebuffer actually returns - recomputed on every VRAM or
   LCD_MODE write. Default `mode` has DISON set (not ROT) rather than the
   real, undocumented POR value: this matches every real-BIOS trace
   already validated this session, which always showed VRAM rendered
   unconditionally, and preserves that behavior for any app that never
   touches LCD_MODE. */
typedef struct lcd {
    uint8_t vram[LCD_VRAM_SIZE];
    uint8_t presented[LCD_VRAM_SIZE];
    uint32_t mode;
    uint32_t cal;
    int dirty;
} lcd_t;

void lcd_init(lcd_t *lcd);
uint8_t lcd_read8(lcd_t *lcd, uint32_t offset);
void lcd_write8(lcd_t *lcd, uint32_t offset, uint8_t value);
uint8_t lcd_mode_read8(lcd_t *lcd, uint32_t offset);
void lcd_mode_write8(lcd_t *lcd, uint32_t offset, uint8_t value);

#endif
