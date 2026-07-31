/* Diagnostic: drive pk_timing_bench through its real continue/exit flow entirely in the emulator, and confirm
   the release-wait fix in src/ui.s (pb_prompt_confirm_exit) actually blocks departure while Action stays
   physically held, then lets it proceed once released.

   This exists because the bug it targets - departing while Action is still down, which made the real BIOS's
   browse screen read that as a fresh press and relaunch the app at once - was reported only on real hardware.
   The fix could not be verified against that report directly (this emulator has no BIOS browse-screen model to
   watch relaunch on), but the mechanism it depends on - button STATUS surviving an acknowledge, so a genuinely
   held button keeps reading as held - is now real emulator behavior (see core/src/intc.h's INT_LEVEL_MASK).
   This confirms that mechanism actually stops departure at the point that matters: while the button is still
   asserted, the CPU stays inside the small wait loop rather than reaching the departure SVC chain.

   usage: pk_exit_test <bios.bin> <pk_timing_bench.mcs> */
#include <stdio.h>
#include <stdlib.h>

#include "psemu_internal.h"

#define WRAM_SCREEN_INDEX 0x25Cu
#define WRAM_FIRE_HOLD_COUNTER 0x264u
#define WRAM_EXIT_PROMPT_SELECTION 0x268u
#define SCREEN_EXIT_PROMPT 11u
#define FIRE_HOLD_THRESHOLD 75000u

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

static int pc_in_bios(const psemu_t *ps) {
    return (ps->cpu.r[15] >> 24) == 0x04u;
}

int main(int argc, char **argv) {
    size_t bios_size = 0, app_size = 0;
    uint8_t *bios, *app;
    psemu_t *ps;
    long f;
    uint32_t last_screen;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios.bin> <pk_timing_bench.mcs>\n", argv[0]);
        return 1;
    }
    bios = read_file(argv[1], &bios_size);
    app = read_file(argv[2], &app_size);
    if (!bios || !app) {
        fprintf(stderr, "failed to read inputs\n");
        return 1;
    }

    ps = psemu_create();
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK || psemu_load_content(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr, "bad BIOS or app image\n");
        return 1;
    }
    psemu_reset(ps);

    /* Same repeating Down / Action / Right / Action power-on sequence bench_probe and tools/inspect.c's
       button_sim=3 use, confirmed end-to-end against real hardware. It repeats so a press that lands too
       early for whatever stage the BIOS animation is on still lands correctly on a later cycle. */
    for (f = 0; f < 600; f++) {
        long phase = f % 240;
        uint32_t buttons = 0;
        if (phase >= 20 && phase < 36) {
            buttons = PSEMU_BUTTON_DOWN;
        } else if (phase >= 50 && phase < 66) {
            buttons = PSEMU_BUTTON_FIRE;
        } else if (phase >= 90 && phase < 106) {
            buttons = PSEMU_BUTTON_RIGHT;
        } else if (phase >= 130 && phase < 146) {
            buttons = PSEMU_BUTTON_FIRE;
        }
        psemu_set_buttons(ps, buttons);
        psemu_run(ps, 33000u);
    }
    if (psemu_cpu_faulted(ps)) {
        fprintf(stderr, "FAIL: CPU faulted during boot/launch, pc=0x%08X\n", ps->cpu.r[15]);
        return 1;
    }

    /* Hold Fire/Action continuously until FIRE_HOLD_THRESHOLD trips and the app switches to its exit prompt
       screen. One press call asserts the edge; nothing needs to call psemu_set_buttons again to represent
       "still held" between here and the explicit release below, the same way a real held button keeps
       asserting STATUS with no further edges. */
    psemu_set_buttons(ps, PSEMU_BUTTON_FIRE);
    last_screen = psemu_bus_read32(&ps->bus, WRAM_SCREEN_INDEX);
    for (f = 0; f < 4000 && psemu_bus_read32(&ps->bus, WRAM_SCREEN_INDEX) != SCREEN_EXIT_PROMPT; f++) {
        psemu_run(ps, 33000u);
        if (psemu_cpu_faulted(ps)) {
            fprintf(stderr, "FAIL: CPU faulted while holding Action, pc=0x%08X\n", ps->cpu.r[15]);
            return 1;
        }
    }
    if (psemu_bus_read32(&ps->bus, WRAM_SCREEN_INDEX) != SCREEN_EXIT_PROMPT) {
        fprintf(stderr, "FAIL: never reached the exit prompt (hold counter stuck at %u/%u)\n",
            psemu_bus_read32(&ps->bus, WRAM_FIRE_HOLD_COUNTER), FIRE_HOLD_THRESHOLD);
        return 1;
    }
    printf("PASS: reached the exit prompt after %ld frames of held Action\n", f);

    /* Release, then select EXIT with a Down tap, matching how a real user actually operates this prompt:
       they let go of the long Action hold that opened it, before choosing anything. */
    psemu_set_buttons(ps, 0);
    psemu_run(ps, 33000u);
    psemu_set_buttons(ps, PSEMU_BUTTON_DOWN);
    psemu_run(ps, 33000u);
    psemu_set_buttons(ps, 0);
    psemu_run(ps, 33000u);
    if (psemu_bus_read32(&ps->bus, WRAM_EXIT_PROMPT_SELECTION) != 1u) {
        fprintf(stderr, "FAIL: Down did not select EXIT\n");
        return 1;
    }
    printf("PASS: EXIT selected\n");

    /* Press Action again to confirm EXIT, and KEEP HOLDING it - this is the exact scenario the bug report
       described: a real finger is still on the button the instant EXIT is confirmed. Step in small slices
       and watch the program counter. Before the fix, this would run straight through the departure SVC chain
       into BIOS space on the very next slice, with Action still asserted. With the fix, it must stay inside
       the small wait loop for as long as this test keeps Action held. */
    psemu_set_buttons(ps, PSEMU_BUTTON_FIRE);
    {
        long i;
        int reached_bios_while_held = 0;
        for (i = 0; i < 2000; i++) {
            psemu_run(ps, 200u);
            if (psemu_cpu_faulted(ps)) {
                fprintf(stderr, "FAIL: CPU faulted confirming EXIT, pc=0x%08X\n", ps->cpu.r[15]);
                return 1;
            }
            if (pc_in_bios(ps)) {
                reached_bios_while_held = 1;
                break;
            }
        }
        if (reached_bios_while_held) {
            fprintf(stderr,
                "FAIL: departed into BIOS space while Action was still held (pc=0x%08X) - the auto-relaunch "
                "bug is NOT fixed\n",
                ps->cpu.r[15]);
            return 1;
        }
    }
    printf("PASS: stayed out of BIOS space for 2000 slices with Action still held (pc=0x%08X)\n", ps->cpu.r[15]);

    /* Now release, and confirm departure actually completes: PC should reach BIOS space within a generous
       number of further slices, proving the wait loop is not simply stuck forever. */
    psemu_set_buttons(ps, 0);
    {
        long i;
        int departed = 0;
        for (i = 0; i < 2000; i++) {
            psemu_run(ps, 200u);
            if (psemu_cpu_faulted(ps)) {
                fprintf(stderr, "FAIL: CPU faulted departing after release, pc=0x%08X\n", ps->cpu.r[15]);
                return 1;
            }
            if (pc_in_bios(ps)) {
                departed = 1;
                break;
            }
        }
        if (!departed) {
            fprintf(stderr, "FAIL: never departed into BIOS space after releasing Action, pc=0x%08X\n",
                ps->cpu.r[15]);
            return 1;
        }
    }
    printf("PASS: departed into BIOS space (pc=0x%08X) only after Action was released\n", ps->cpu.r[15]);

    printf("\nALL PASS: the release-wait fix blocks departure while Action is held, and departure completes "
           "cleanly once it is released.\n");

    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
