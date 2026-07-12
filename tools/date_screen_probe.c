/* Diagnostic: run the real boot sequence through psemu_run's actual
   per-frame loop (like the desktop frontend), navigate to the
   time-setting/blink screen via button presses timed in FRAMES (not
   raw instructions, since CLK_MODE now makes instructions-per-frame
   variable), and report how many real frames elapse between RTC
   int_line toggles (the documented blink driver) alongside CLK_MODE,
   to check whether blink pacing matches the pre-CLK_MODE-work
   baseline or has drifted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

static void print_framebuffer(const psemu_t *ps) {
    const uint8_t *fb = psemu_get_framebuffer(ps);
    for (int row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
            int bit_index = col % 8;
            int on = (fb[byte_index] >> bit_index) & 1;
            putchar(on ? '#' : '.');
        }
        putchar('\n');
    }
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    fread(buf, 1, (size_t)size, f);
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios.bin> <raw_flash.bin> [frames]\n", argv[0]);
        return 1;
    }
    size_t bios_size = 0, app_size = 0;
    uint8_t *bios = read_file(argv[1], &bios_size);
    uint8_t *app = read_file(argv[2], &app_size);

    psemu_t *ps = psemu_create();
    psemu_load_bios(ps, bios, bios_size);
    psemu_load_flash_image(ps, app, app_size);
    psemu_reset(ps);

    long frames = argc >= 4 ? atol(argv[3]) : 500;
    int last_rtc_int = -1;
    long last_toggle_frame = -1;
    uint32_t last_clk_mode = 0xFFFFFFFFu;
    uint32_t last_rtc_mode = 0xFFFFFFFFu;
    uint32_t last_3f0 = 0xFFFFFFFFu;
    long last_3f0_frame = -1;
    uint32_t last_260 = 0xFFFFFFFFu;

    for (long f = 0; f < frames; f++) {
        /* Down for a stretch around frame 100 (well after the beep
           ends, ~frame 55), then Action around frame 140 - generous
           margins, exact real timing doesn't matter for this probe. */
        uint32_t buttons = 0;
        if (f >= 100 && f < 110) {
            buttons = PSEMU_BUTTON_DOWN;
        } else if (f >= 140 && f < 150) {
            buttons = PSEMU_BUTTON_FIRE;
        }
        psemu_set_buttons(ps, buttons);
        psemu_run(ps, 33000u);

        if (ps->rtc.mode != last_rtc_mode) {
            printf("frame %ld: RTC mode 0x%08X -> 0x%08X (PRGSEL=%u paused=%s)\n", f, last_rtc_mode, ps->rtc.mode,
                   ps->rtc.mode & 1u, (ps->rtc.mode & 1u) ? "yes" : "no");
            last_rtc_mode = ps->rtc.mode;
        }
        if (ps->clk.mode != last_clk_mode) {
            printf("frame %ld: clk.mode -> %u (hz=%u)\n", f, ps->clk.mode, clk_current_hz(&ps->clk));
            last_clk_mode = ps->clk.mode;
        }
        if (ps->rtc.int_line != last_rtc_int) {
            long delta = (last_toggle_frame < 0) ? -1 : (f - last_toggle_frame);
            printf("frame %ld: RTC int_line -> %d (delta_frames=%ld) clk.mode=%u rtc.mode=0x%08X\n", f,
                   ps->rtc.int_line, delta, ps->clk.mode, ps->rtc.mode);
            last_toggle_frame = f;
            last_rtc_int = ps->rtc.int_line;
        }

        uint32_t v3f0 = psemu_bus_read32(&ps->bus, 0x3F0u);
        if (v3f0 != last_3f0) {
            long delta = (last_3f0_frame < 0) ? -1 : (f - last_3f0_frame);
            printf("frame %ld: [0x3F0] 0x%08X -> 0x%08X (delta_frames=%ld) clk.mode=%u\n", f, last_3f0, v3f0, delta,
                   ps->clk.mode);
            last_3f0_frame = f;
            last_3f0 = v3f0;
        }
        uint32_t v260 = psemu_bus_read32(&ps->bus, 0x260u);
        if (v260 != last_260) {
            printf("frame %ld: [0x260] 0x%08X -> 0x%08X clk.mode=%u\n", f, last_260, v260, ps->clk.mode);
            last_260 = v260;
        }

        if (f == 90 || f == 130 || f == 160 || f == 200 || f == 300) {
            printf("frame %ld: framebuffer:\n", f);
            print_framebuffer(ps);
        }
    }

    psemu_destroy(ps);
    return 0;
}
