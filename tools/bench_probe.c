/* A diagnostic tool. It executes an app in the pk_timing_bench form to completion, and it reads the
   results directly from WRAM. Thus a person does not have to read hex digits from the 32x32 LCD.

   pk_timing_bench does each of its measurements one time at startup. It leaves the raw values in a
   fixed WRAM block (see pk_timing_bench/src/constants.inc: WRAM_RESULTS_BASE and the WRAM_DIAG_*
   slots). A direct read of that block permits an automatic comparison of two builds of the app. That
   comparison is the purpose of this tool: it confirms that a local rebuild of the .mcs file operates
   the same as the committed file, which a test on real hardware confirmed. Do this comparison before
   you use a changed build on real hardware.

   usage: bench_probe <bios.bin> <app.mcs> [frames]

   This tool starts the app the same way that a user does. The BIOS boots to its date/time screen.
   Down and Action then move past that screen. Right and Action then select the app in the card
   directory and start it (see the comment on psemu_load_app in psemu/psemu.h).

   Set BENCH_PROBE_DUMP_BIOS=1 to also write the raw BIOS ROM bytes, for a disassembly on a computer,
   and the live INTC and CPSR state. This is useful when the app stops during its operation, and the
   cause is in the BIOS code and not in the app code. */
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
        /* The same repeating power-on sequence of Down, Action, Right, and Action that button_sim=3
           in tools/inspect.c uses. A test against real hardware confirms this sequence.
           This code gives the sequence in frames, and not in raw instruction counts.
           The sequence repeats deliberately. If a press in one cycle occurs too early for the current
           stage of the BIOS animation, a press in a later cycle occurs at the correct time. */
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

    /* Move to the requested screen with short RIGHT presses, until the screen-index variable of the app
       gives the target value. A fixed number of presses is not reliable: the start sequence repeats,
       thus its own RIGHT presses already advanced the screen an unknown number of times before the app
       was ready. */
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
    /* Experiment 6 (screen 6): the Timer0 totals across a fixed number of Timer2 reloads. A build from
       before that experiment does not have these values, and these slots then read back as 0. */
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
    /* Experiment 9 (screen 9): the cost of an IRDA_DATA write (the test) against the cost of a WRAM
       write (the control). A build from before that experiment does not have these values, and these
       slots then read back as 0. */
    printf("screen9 irda write results @0x2A8:\n");
    printf("  IRDA_DATA (test) 0x%08X\n", psemu_bus_read32(&ps->bus, 0x2A8u));
    printf("  WRAM (control)   0x%08X\n", psemu_bus_read32(&ps->bus, 0x2ACu));

    /* Experiment 10 (screen 10): the Timer0 ticks across 64 Timer2 and FIQ periods that the app arms
       again each time. The arithmetic is the same as screen 8, but it uses FIQ in place of IRQ. */
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
    /* Experiment 11 (screen 11): the same shape as screen 10. But here the handler does the full
       realistic dispatch before it arms the timer again. That dispatch is an acknowledge, a nested
       call, and an ARM-to-Thumb transition. It is not a minimal dispatch. */
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

    /* Set BENCH_PROBE_DUMP_BIOS=1 to write the raw BIOS ROM bytes for a disassembly on a computer, and
       the live INTC and CPSR state. This function found the separate callback-slot addresses of the
       real BIOS: 0xFC for IRQ, and 0x100 for FIQ. It found them during the diagnosis of a stop in
       experiment 10: the PC stopped inside the FIQ vector handler. This dump permitted a disassembly
       of the exact code at that address. Without it, a person can only make an assumption from a PC
       trace. */
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
