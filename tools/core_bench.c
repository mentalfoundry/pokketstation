/* Measures what a frontend actually pays per emulated instruction.

   This exists because "is this diagnostic free?" kept getting answered by
   reasoning instead of measurement, and the reasoning was wrong: the bus
   read-trace hook (see psemu_bus_read_trace_cb in core/src/memory.c) cost
   about 20% with its callback left NULL, purely because it sits once per
   byte of every bus read.

   It deliberately links plain `psemu`, not `psemu_trace`, so it reports
   the cost frontends really carry rather than the tooling build's.

   The workload is the real BIOS boot driven through psemu_run, the same
   entry point the desktop frontend uses, for a fixed cycle budget. The
   emulator is deterministic, so the executed instruction count is
   identical run to run and only wall time varies.

   usage: core_bench <bios.bin> [frames] [repeats] */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "psemu_internal.h"

/* Matches the desktop frontend's own per-frame budget: 33000 cycles at a
   nominal 32Hz refresh (see PSEMU_ASSUMED_CPU_HZ in core/src/dac.h). */
#define CYCLES_PER_FRAME 33000u

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <bios.bin> [frames] [repeats]\n", argv[0]);
        return 1;
    }
    size_t bios_size = 0;
    uint8_t *bios = read_file(argv[1], &bios_size);
    if (!bios) {
        fprintf(stderr, "failed to read bios %s\n", argv[1]);
        return 1;
    }
    long frames = argc >= 3 ? atol(argv[2]) : 3000;
    int repeats = argc >= 4 ? atoi(argv[3]) : 5;

    double best = 0.0;
    uint64_t steps = 0;
    for (int r = 0; r < repeats; r++) {
        psemu_t *ps = psemu_create();
        if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
            fprintf(stderr, "bad bios size: %zu (need %d)\n", bios_size, PSEMU_BIOS_SIZE);
            return 1;
        }
        psemu_reset(ps);
        if (r == 0) {
            printf(
                "BIOS settings-override offsets: %s\n",
                psemu_settings_offsets_known(ps) ? "known (overrides available)" : "UNKNOWN (overrides disabled)");
        }

        clock_t t0 = clock();
        for (long f = 0; f < frames; f++) {
            psemu_run(ps, CYCLES_PER_FRAME);
        }
        clock_t t1 = clock();

        double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
        steps = ps->cpu.total_steps;
        if (r == 0 || secs < best) {
            best = secs;
        }
        psemu_destroy(ps);
    }
    free(bios);

    /* Best-of-N, not the mean: this is a throughput measurement on a
       noisy desktop, where the fastest run is the one least disturbed. */
    printf(
        "%ld frames, %llu instructions, best of %d: %.4f s  (%.2f M instr/s)\n", frames,
        (unsigned long long)steps, repeats, best, steps / best / 1e6);
    return 0;
}
