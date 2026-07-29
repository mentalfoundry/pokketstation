#ifndef PSEMU_LCD_H
#define PSEMU_LCD_H

#include <stdint.h>

#define LCD_VRAM_SIZE 128u
/* LCD_MODE (+0x0, R/W) and LCD_CAL (+0x4).
   LCD_MODE bit 6 is DISON (display on/off).
   LCD_MODE bit 7 is ROT (rotate display 180 degrees). Real hardware sets ROT for docked mode, to match
   INT_INPUT.11's docking flag. Both bits are real hardware bits.

   History: this address range previously had no bus handler at all.
   Writes to it silently vanished, and psemu_get_framebuffer always returned raw VRAM unconditionally. */
#define LCD_MODE_REG_SPAN 0x8u

#define LCD_MODE_DISON 0x40u
#define LCD_MODE_ROT 0x80u

/* VRAM is already the packed 1bpp framebuffer: 32 rows of 4 bytes each.
   Bit 0 is the leftmost pixel. 0 = white, 1 = black.

   `presented` holds the same format, after applying LCD_MODE's DISON/ROT bits.
   psemu_get_framebuffer returns `presented`, not raw VRAM.
   This emulator recomputes `presented` on every VRAM or LCD_MODE write.

   The default `mode` has DISON set, and ROT clear.
   The real POR value is undocumented; this default is not that value.
   This default matches every real-BIOS trace validated so far, which always showed VRAM rendered unconditionally.
   It preserves that behavior for any app that never touches LCD_MODE. */
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
