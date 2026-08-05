/* Measures the true cost of each emulated instruction to a frontend.

   This tool is necessary because the question "does this diagnostic have a
   cost?" received an answer from reasoning, and not from a measurement. That
   reasoning was incorrect: the bus read-trace hook (see
   psemu_bus_read_trace_cb in core/src/memory.c) cost approximately 20% with
   its callback set to NULL. The cause is its position: it executes one time
   for each byte of each bus read.

   This tool links the usual `psemu` library, and not `psemu_trace`. Thus it
   reports the cost that a frontend has, and not the cost of the tooling
   build.

   The workload is the boot sequence of the real BIOS, through psemu_run. That
   is the same function that the desktop frontend uses. The budget is a fixed
   number of cycles. The emulator is deterministic, thus the executed
   instruction count is the same at each run, and only the wall time changes.

   usage: core_bench <bios.bin> [frames] [repeats] */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "psemu_internal.h"

/* This value agrees with the per-frame budget of the desktop frontend: 33000
   cycles at a nominal 32Hz refresh rate (see PSEMU_ASSUMED_CPU_HZ in
   core/src/dac.h). */
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

    /* This code uses the best result of N runs, and not the average. This is
       a throughput measurement on a desktop computer with other active
       processes. Thus the fastest run has the least interference. */
    printf(
        "%ld frames, %llu instructions, best of %d: %.4f s  (%.2f M instr/s)\n", frames,
        (unsigned long long)steps, repeats, best, steps / best / 1e6);
    return 0;
}
