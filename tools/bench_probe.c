/* Diagnostic: run a pk_timing_bench-style app to completion and read its results straight out of WRAM,
   instead of reading hex digits off the 32x32 LCD by hand.

   pk_timing_bench runs all its measurements once at startup and leaves the raw values in a fixed WRAM block
   (see pk_timing_bench/src/constants.inc: WRAM_RESULTS_BASE and the WRAM_DIAG_* slots). Reading that block
   directly makes it possible to diff two builds of the app against each other automatically, which is what
   this tool exists for: confirming a locally rebuilt .mcs is functionally identical to the committed,
   real-hardware-verified one before trusting a modified build on real hardware.

   usage: bench_probe <bios.bin> <app.mcs> [frames]

   The app is launched the same way a user would: the BIOS boots to its date/time screen, then Down+Action
   gets past it, then Right+Action selects and launches the app from the card directory (see psemu_load_app's
   comment in psemu/psemu.h). */
#include <stdio.h>
#include <stdlib.h>

#include "psemu_internal.h"

#define WRAM_RESULTS_BASE 0x200u
#define WRAM_DIAG_SINGLE_BEFORE 0x280u

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *buf;
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static void print_framebuffer(const psemu_t *ps) {
    const uint8_t *fb = psemu_get_framebuffer(ps);
    int row, col;
    for (row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (col = 0; col < PSEMU_LCD_WIDTH; col++) {
            int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
            putchar((fb[byte_index] >> (col % 8)) & 1 ? '#' : '.');
        }
        putchar('\n');
    }
}

int main(int argc, char **argv) {
    size_t bios_size = 0, app_size = 0;
    uint8_t *bios, *app;
    psemu_t *ps;
    long frames = 600, f;
    long nav_frames = 600; /* frames spent on the BIOS launch sequence before paging starts */
    long pages = 0;        /* RIGHT taps to issue after launch, to reach a given result screen */
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios.bin> <app.mcs> [frames] [pages]\n", argv[0]);
        fprintf(stderr, "  pages: screen number to page to after launch (0 = leave wherever it lands)\n");
        return 1;
    }
    bios = read_file(argv[1], &bios_size);
    app = read_file(argv[2], &app_size);
    if (!bios || !app) {
        fprintf(stderr, "failed to read inputs\n");
        return 1;
    }
    if (argc >= 4) {
        frames = atol(argv[3]);
    }
    if (argc >= 5) {
        pages = atol(argv[4]);
    }

    ps = psemu_create();
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        fprintf(stderr, "bad BIOS\n");
        return 1;
    }
    if (psemu_load_content(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr, "bad app image\n");
        return 1;
    }
    psemu_reset(ps);

    for (f = 0; f < frames; f++) {
        /* Same repeating Down / Action / Right / Action power-on sequence tools/inspect.c's button_sim=3
           uses, which is confirmed end-to-end against real hardware - expressed in frames here rather than
           raw instruction counts. It deliberately repeats: if one cycle's press lands too early for whatever
           stage the BIOS animation is on, a later cycle still lands correctly. */
        uint32_t buttons = 0;
        if (f < nav_frames) {
            long phase = f % 240;
            if (phase >= 20 && phase < 36) {
                buttons = PSEMU_BUTTON_DOWN; /* past the date/time screen */
            } else if (phase >= 50 && phase < 66) {
                buttons = PSEMU_BUTTON_FIRE;
            } else if (phase >= 90 && phase < 106) {
                buttons = PSEMU_BUTTON_RIGHT; /* move to the first app in the card directory */
            } else if (phase >= 130 && phase < 146) {
                buttons = PSEMU_BUTTON_FIRE; /* launch it */
            }
        }
        psemu_set_buttons(ps, buttons);
        psemu_run(ps, 33000u);
    }

    /* Page to the requested screen by tapping RIGHT until the app's own screen-index variable reads back the
       target. Tapping a fixed number of times is not reliable: the launch sequence repeats, so its own RIGHT
       presses have already advanced the screen an unpredictable number of times by the time the app is up. */
    if (pages > 0) {
        int attempt;
        for (attempt = 0; attempt < 16 && psemu_bus_read32(&ps->bus, 0x25Cu) != (uint32_t)pages; attempt++) {
            int k;
            for (k = 0; k < 10; k++) { /* press */
                psemu_set_buttons(ps, PSEMU_BUTTON_RIGHT);
                psemu_run(ps, 33000u);
            }
            for (k = 0; k < 20; k++) { /* release, so the debounce clears */
                psemu_set_buttons(ps, 0);
                psemu_run(ps, 33000u);
            }
        }
    }

    printf("cpu_faulted=%d  pc=0x%08X\n", psemu_cpu_faulted(ps), ps->cpu.r[15]);
    if (psemu_cpu_faulted(ps)) {
        FILE *rep = fopen("bench_probe_fault.log", "w");
        if (rep) {
            psemu_write_crash_report(ps, rep);
            fclose(rep);
            printf("wrote bench_probe_fault.log\n");
        }
    }
    printf("results @0x%03X:\n", WRAM_RESULTS_BASE);
    for (i = 0; i < 8; i++) {
        printf("  [%d] 0x%08X\n", i, psemu_bus_read32(&ps->bus, WRAM_RESULTS_BASE + (uint32_t)i * 4u));
    }
    printf("diag @0x%03X:\n", WRAM_DIAG_SINGLE_BEFORE);
    for (i = 0; i < 4; i++) {
        printf("  [%d] 0x%08X\n", i, psemu_bus_read32(&ps->bus, WRAM_DIAG_SINGLE_BEFORE + (uint32_t)i * 4u));
    }
    /* Experiment 6 (screen 6): Timer0 stopwatch totals across a fixed number of Timer2 reloads. Absent in
       builds predating that experiment, where these slots simply read back 0. */
    printf("screen6 timer results @0x290:\n");
    printf("  A (period %u x %u reloads) 0x%08X\n", 1016u, 256u, psemu_bus_read32(&ps->bus, 0x290u));
    printf("  B (period %u x %u reloads) 0x%08X\n", 2032u, 128u, psemu_bus_read32(&ps->bus, 0x294u));
    /* Experiment 7 (screen 7): the same loop timed with interrupts masked vs with one timer interrupt live. */
    printf("screen7 irq results @0x298:\n");
    printf("  baseline (interrupts masked) 0x%08X\n", psemu_bus_read32(&ps->bus, 0x298u));
    printf("  with one timer IRQ live      0x%08X\n", psemu_bus_read32(&ps->bus, 0x29Cu));
    printf("screen:\n");
    print_framebuffer(ps);

    printf("screen index (WRAM 0x25C) = %u\n", psemu_bus_read32(&ps->bus, 0x25Cu));

    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
