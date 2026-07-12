/* Diagnostic: mimic the desktop frontend's real frame loop
   (psemu_run(ps, 33000) once per nominal 31ms frame) and report DAC
   sample production and DAC_CTRL (audio enable) transitions per frame -
   used to verify that audio's real-time pacing/content-rate stays
   independent of CLK_MODE (see docs/hardware-notes.md and
   core/src/psemu.c's psemu_run comment for the full investigation).
   Pass a 5th argument to force ps->clk.mode to a fixed value every
   frame (overriding whatever the BIOS wrote), to directly compare
   audio behavior across CLK_MODE values from an otherwise-identical
   execution. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

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
        fprintf(stderr, "usage: %s <bios.bin> <raw_flash.bin> [frames] [force_clk_mode]\n", argv[0]);
        return 1;
    }
    size_t bios_size = 0, app_size = 0;
    uint8_t *bios = read_file(argv[1], &bios_size);
    uint8_t *app = read_file(argv[2], &app_size);

    psemu_t *ps = psemu_create();
    psemu_load_bios(ps, bios, bios_size);
    psemu_load_flash_image(ps, app, app_size);
    psemu_reset(ps);

    long frames = argc >= 4 ? atol(argv[3]) : 300;
    int force_mode = argc >= 5 ? atoi(argv[4]) : -1;
    int16_t buf[4096];
    uint32_t last_ctrl = 0xFFFFFFFFu;
    long total_samples = 0;

    for (long f = 0; f < frames; f++) {
        if (force_mode >= 0) {
            ps->clk.mode = (uint32_t)force_mode;
        }
        int16_t last_sample = ps->dac.current_sample;
        psemu_run(ps, 33000u);
        int content_changed = ps->dac.current_sample != last_sample;
        uint32_t n = psemu_get_audio_samples(ps, buf, 4096u);
        total_samples += n;

        if (ps->dac.ctrl != last_ctrl) {
            printf("frame %ld: DAC_CTRL 0x%08X -> 0x%08X, clk.mode=%u (hz=%u)\n", f, last_ctrl, ps->dac.ctrl,
                   ps->clk.mode, clk_current_hz(&ps->clk));
            last_ctrl = ps->dac.ctrl;
        }
        if (ps->dac.ctrl & 1u) {
            printf("  frame %ld: content_changed=%d samples=%u clk.mode=%u\n", f, content_changed, n, ps->clk.mode);
        }
    }

    printf("\ntotal frames=%ld total_samples=%ld\n", frames, total_samples);

    psemu_destroy(ps);
    return 0;
}
