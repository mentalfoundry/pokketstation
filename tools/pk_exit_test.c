/* A diagnostic tool. It operates pk_timing_bench through its real continue and exit sequence, fully in
   the emulator. It confirms the release-wait correction in src/ui.s (pb_prompt_confirm_exit): that
   correction must prevent the exit while the user holds Action, and permit the exit after the release.

   This tool is necessary because a report of the target fault came only from real hardware. In that
   fault, the app exited while Action was still down. The browse screen of the real BIOS then read that
   condition as a new press, and it started the app again immediately. This emulator has since
   reproduced that report against the browse screen of the real BIOS. See
   tools/button_timing_probe.c: an app that exits with Action still asserted starts again from the
   browse screen at the release edge, exactly as the report gives.
   This test depends on one mechanism: the button STATUS bit continues after an acknowledge, thus a
   held button continues to read as held. That mechanism is real emulator behavior (see INT_LEVEL_MASK
   in core/src/intc.h).
   This test confirms that the mechanism stops the exit at the necessary point: while the button is
   asserted, the CPU stays in the small wait loop, and it does not get to the exit SVC sequence.

   usage: pk_exit_test <bios.bin> <pk_timing_bench.mcs> */
#include <stdio.h>
#include <stdlib.h>

#include "psemu_internal.h"

#define WRAM_SCREEN_INDEX 0x25Cu
#define WRAM_FIRE_HOLD_COUNTER 0x264u
#define WRAM_EXIT_PROMPT_SELECTION 0x268u
#define SCREEN_EXIT_PROMPT 12u
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

    /* The same repeating power-on sequence of Down, Action, Right, and Action that bench_probe and
       button_sim=3 in tools/inspect.c use. A test against real hardware confirms this sequence. The
       sequence repeats: thus a press that occurs too early for the current stage of the BIOS
       animation still occurs at the correct time in a later cycle. */
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

    /* Hold the Fire (Action) button until FIRE_HOLD_THRESHOLD occurs and the app changes to its exit
       prompt screen. One press call asserts the edge. No code must call psemu_set_buttons again for
       the "still held" condition, between this point and the explicit release below. A real held
       button also continues to assert STATUS with no more edges. */
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

    /* Release the button, and then select EXIT with a short Down press. A real user operates this
       prompt the same way: the user releases the long Action hold that opened the prompt, before the
       user selects an item. */
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

    /* Press Action again to confirm EXIT, and CONTINUE TO HOLD the button. This is the exact condition
       in the fault report: the finger of the user is still on the button at the moment of the EXIT
       confirmation. Execute in small periods, and monitor the program counter. Before the correction,
       the CPU executed the exit SVC sequence into BIOS memory at the next period, with Action still
       asserted. With the correction, the CPU must stay in the small wait loop while this test holds
       Action. */
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

    /* Now release the button, and confirm that the exit completes. The PC must get to BIOS memory in a
       large number of further periods. That result proves that the wait loop does not continue for an
       unlimited time. */
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
