#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include "psemu/psemu.h"

#define SCALE 6
#define VITA_SCREEN_WIDTH 960
#define VITA_SCREEN_HEIGHT 544

static void render_framebuffer(const psemu_t *ps, uint32_t *pixels) {
    const uint8_t *fb = psemu_get_framebuffer(ps);
    for (int row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
            int bit_index = col % 8;
            int on = (fb[byte_index] >> bit_index) & 1;
            pixels[row * PSEMU_LCD_WIDTH + col] = on ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    vita2d_init();
    vita2d_set_clear_color(RGBA8(0xFF, 0xFF, 0xFF, 0xFF));

    psemu_t *ps = psemu_create();
    /* TODO: load a BIOS dump and app from ux0:data/pokketstation/ once a
       file picker exists - for now this only exercises the render loop. */
    psemu_reset(ps);

    vita2d_texture *texture =
        vita2d_create_empty_texture_format(PSEMU_LCD_WIDTH, PSEMU_LCD_HEIGHT, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    uint32_t *pixels = (uint32_t *)vita2d_texture_get_datap(texture);

    int running = 1;
    while (running) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t buttons = 0;
        if (pad.buttons & SCE_CTRL_UP) buttons |= PSEMU_BUTTON_UP;
        if (pad.buttons & SCE_CTRL_DOWN) buttons |= PSEMU_BUTTON_DOWN;
        if (pad.buttons & SCE_CTRL_LEFT) buttons |= PSEMU_BUTTON_LEFT;
        if (pad.buttons & SCE_CTRL_RIGHT) buttons |= PSEMU_BUTTON_RIGHT;
        if (pad.buttons & SCE_CTRL_CROSS) buttons |= PSEMU_BUTTON_FIRE;
        if (pad.buttons & SCE_CTRL_START) running = 0;
        psemu_set_buttons(ps, buttons);

        psemu_run(ps, 33000);
        render_framebuffer(ps, pixels);

        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_draw_texture_scale(texture, VITA_SCREEN_WIDTH / 2.0f - (PSEMU_LCD_WIDTH * SCALE) / 2.0f,
            VITA_SCREEN_HEIGHT / 2.0f - (PSEMU_LCD_HEIGHT * SCALE) / 2.0f, SCALE, SCALE);
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    vita2d_free_texture(texture);
    psemu_destroy(ps);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
