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
   comment in psemu/psemu.h).

   Set BENCH_PROBE_DUMP_BIOS=1 to also dump raw BIOS ROM bytes (for offline disassembly) plus live INTC/CPSR
   state. Useful when the app hangs partway through and the reason lives in BIOS code, not app code. */
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
        /* The same repeating Down, Action, Right, Action power-on sequence that tools/inspect.c's
           button_sim=3 uses. Real hardware confirms that sequence end to end.
           This expresses it in frames rather than in raw instruction counts.
           It repeats on purpose. If one cycle's press lands too early for the stage the BIOS animation has
           reached, a later cycle still lands correctly. */
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
    /* Experiment 8 (screen 8): Timer0 ticks across 64 re-armed Timer1 periods. */
    {
        uint32_t delta = psemu_bus_read32(&ps->bus, 0x2A0u);
        printf("screen8 re-arm latency @0x2A0:\n");
        printf("  Timer0 delta over 64 periods 0x%08X\n", delta);
        if (delta) {
            double ticks = (double)delta * 32.0 / 64.0 / 2.0;
            printf("  -> effective period %.1f Timer1 ticks (armed 1016, so 1017 without latency)\n", ticks);
            printf("  -> expiry-to-re-arm latency %.1f ticks\n", ticks - 1017.0);
        }
    }
    /* Experiment 9 (screen 9): IRDA_DATA write cost (test) vs WRAM write cost (control). Absent in builds
       predating that experiment, where these slots simply read back 0. */
    printf("screen9 irda write results @0x2A8:\n");
    printf("  IRDA_DATA (test) 0x%08X\n", psemu_bus_read32(&ps->bus, 0x2A8u));
    printf("  WRAM (control)   0x%08X\n", psemu_bus_read32(&ps->bus, 0x2ACu));

    /* Experiment 10 (screen 10): Timer0 ticks across 64 re-armed Timer2/FIQ periods. Same arithmetic as
       screen 8, over FIQ instead of IRQ. */
    {
        uint32_t delta = psemu_bus_read32(&ps->bus, 0x2B0u);
        printf("screen10 FIQ re-arm latency @0x2B0:\n");
        printf("  Timer0 delta over 64 periods 0x%08X\n", delta);
        if (delta) {
            double ticks = (double)delta * 32.0 / 64.0 / 2.0;
            printf("  -> effective period %.1f Timer1 ticks (armed 1016, so 1017 without latency)\n", ticks);
            printf("  -> expiry-to-re-arm latency %.1f ticks\n", ticks - 1017.0);
        }
    }
    /* Experiment 11 (screen 11): same shape as screen 10, but the handler does the full realistic dispatch
       (acknowledge, nested call, ARM-to-Thumb trampoline) before its re-arm, not a bare one. */
    {
        uint32_t delta = psemu_bus_read32(&ps->bus, 0x2B8u);
        printf("screen11 full-dispatch FIQ latency @0x2B8:\n");
        printf("  Timer0 delta over 64 periods 0x%08X\n", delta);
        if (delta) {
            double ticks = (double)delta * 32.0 / 64.0 / 2.0;
            printf("  -> effective period %.1f Timer1 ticks (armed 1016, so 1017 without latency)\n", ticks);
            printf("  -> expiry-to-write latency %.1f ticks\n", ticks - 1017.0);
        }
    }

    printf("screen:\n");
    print_framebuffer(ps);

    printf("screen index (WRAM 0x25C) = %u\n", psemu_bus_read32(&ps->bus, 0x25Cu));

    /* Set BENCH_PROBE_DUMP_BIOS=1 to dump raw BIOS ROM bytes for offline disassembly, plus live INTC/CPSR
       state. This is what located the real BIOS's separate IRQ (0xFC) vs FIQ (0x100) callback-slot addresses
       while debugging experiment 10's hang: pc got stuck inside the FIQ vector handler, and this dump made it
       possible to disassemble exactly what it was doing instead of guessing from a PC trace alone. */
    if (getenv("BENCH_PROBE_DUMP_BIOS")) {
        FILE *dump = fopen("bios_code_dump.bin", "wb");
        if (dump) {
            uint32_t addr;
            for (addr = 0x04001000u; addr < 0x04002000u; addr++) {
                uint8_t byte = psemu_bus_read8(&ps->bus, addr);
                fwrite(&byte, 1, 1, dump);
            }
            fclose(dump);
            printf("wrote bios_code_dump.bin (0x04001000-0x04002000)\n");
        }
        printf("intc: enable=0x%08X hold=0x%08X status=0x%08X mask=0x%08X\n",
            psemu_bus_read32(&ps->bus, 0x0A000008u), psemu_bus_read32(&ps->bus, 0x0A000000u),
            psemu_bus_read32(&ps->bus, 0x0A000004u), psemu_bus_read32(&ps->bus, 0x0A00000Cu));
        printf("cpsr=0x%08X\n", ps->cpu.cpsr);
    }

    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
