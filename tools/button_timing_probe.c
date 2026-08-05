/* A diagnostic tool. It measures the maximum time that a button press can stay asserted before it
   causes a fault, against the real BIOS and a real app. This tool is necessary because the
   button-press minimum of the desktop frontend (see BUTTON_MIN_PRESS_FRAMES in
   frontends/desktop/main.c) has a limit on each side. Neither limit is possible to estimate, thus a
   measurement must give both.

   State mode: this mode loads a save state from the desktop frontend, on the exit screen of an app.
   It then presses a button, and reports the subsequent operations of the machine. Those operations
   are each syscall that the app issues, whether the app-dispatch window of the BIOS executes again
   (which is the internal form of "the app started again"), and the LCD content at each stage. Then,
   with the BIOS browse screen active, this mode presses Action again. Thus it confirms that the
   browse screen still accepts a usual press of the same length.

   Boot mode (supply "boot" in place of a state file): this mode does the recorded cold-boot
   navigation with presses of the given length, and it reports whether the navigation gets to the app.

   Threshold mode (supply "thresh" as the button): this mode measures the two durations that together
   decide whether the exit of an app operates. The measurement is in emulated real time, and not in
   frames. The mode then tests each whole-frame press length across both timing axes of a press.

   The measurements below use the J110 BIOS and one real commercial app. That app has an exit screen,
   and it does not wait for the button release before it exits. The card image is in testdata/, which
   .gitignore excludes.

     The browse screen ignores an Action press of less than approximately 35ms. That limit is near to
     the real hardware press of approximately 40ms in docs/app-notes.md. Thus the timing of the core
     is not the cause of the fault.
     The exit screen of that app exits approximately 62ms to 94ms after the press. The exact time
     depends on the position of the press in the tick of the app.

   Thus the usable window is approximately 35ms to 62ms. A frame is 31.25ms. A frontend that samples
   the buttons one time for each frame can give only 31ms, 62ms, 94ms, and longer. Exactly one of
   those durations is in the window:

     1 frame  (31ms)  the exit is correct at 64 of 64 offsets. But the boot navigation never gets to
                      an app, and the browse screen ignores the press.
     2 frames (62ms)  the exit is correct at 64 of 64 offsets, the boot navigation gets to the app,
                      and the browse screen accepts the press.
     3 frames (94ms)  the exit is correct at 27 of 64 offsets. It is in the window only by chance.
     4 frames (125ms) the exit is correct at 1 of 64 offsets.
     5 frames (156ms) the exit is correct at 1 of 64 offsets.

   usage: button_timing_probe <bios.bin> <card.mcr> <state.sav|boot> [button|thresh] [hold_frames]
                              [trigger_pc_hex]
          button:         fire (default) | up | down | left | right | none | thresh
          hold_frames:    the number of frames that the button stays asserted to the core (default 2)
          trigger_pc_hex: write the PC ring of the CPU when the app issues the syscall at this
                          address */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

/* Matches frontends/desktop/main.c's quicksave_header_t. */
#define QUICKSAVE_HEADER_SIZE 16u

/* One emulated frame uses the 33000-cycle budget of the desktop frontend. That budget is not a fixed
   number of single steps. Thus this tool counts real cycles. */
#define FRAME_CYCLES 33000u

/* The address window that tools/inspect.c monitors for the app-dispatch routine of the real BIOS. That
   routine builds F_BANK_FLG from a directory chain, and it branches to the entry point of an app. Its
   end is at 0x04001AF8. This window is larger than that routine: it also contains 0x04001BC8, which is
   the SWI 0x16 handler that an app calls to read the selected app slot. Thus a hit in this window
   means "examine this event". It does not mean "the app started again". Use the trace output to decide
   that. */
#define DISPATCH_LO 0x04001900u
#define DISPATCH_HI 0x04001C00u

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

static void dump_lcd(const psemu_t *ps, const char *label) {
    const uint8_t *fb = psemu_get_framebuffer(ps);
    int y, x;
    printf("  LCD (%s):\n", label);
    for (y = 0; y < PSEMU_LCD_HEIGHT; y++) {
        printf("    |");
        for (x = 0; x < PSEMU_LCD_WIDTH; x++) {
            int bit = (fb[y * PSEMU_LCD_STRIDE + (x / 8)] >> (x % 8)) & 1;
            putchar(bit ? '#' : '.');
        }
        printf("|\n");
    }
}

/* Executes one full frame at the per-frame budget of the desktop frontend. It asserts `buttons` again
   first, exactly as the main loop of that frontend does. */
static void run_frame(psemu_t *ps, uint32_t buttons) {
    uint32_t c = 0;
    psemu_set_buttons(ps, buttons);
    while (c < FRAME_CYCLES) {
        c += psemu_run(ps, 1u);
    }
}

/* Asserts `buttons` for `press_cycles` reference cycles. It then releases the buttons, and executes
   `settle_frames` frames.
   press_cycles is a real-time duration at PSEMU_ASSUMED_CPU_HZ. Thus it can be shorter than one frame,
   which is the purpose of this function: it measures the requirement of the core, without the
   once-per-frame sampling of the frontend. This function asserts the buttons again at each frame
   boundary in the press. The frontend does the same for each press that is longer than one frame. */
static void press_for_cycles(psemu_t *ps, uint32_t buttons, uint32_t press_cycles, long settle_frames) {
    uint32_t done = 0;
    long f;
    while (done < press_cycles) {
        uint32_t slice = press_cycles - done;
        uint32_t c = 0;
        if (slice > FRAME_CYCLES) {
            slice = FRAME_CYCLES;
        }
        psemu_set_buttons(ps, buttons);
        while (c < slice) {
            c += psemu_run(ps, 1u);
        }
        done += c;
    }
    for (f = 0; f < settle_frames; f++) {
        run_frame(ps, 0u);
    }
}

static uint32_t parse_button(const char *name) {
    if (!strcmp(name, "fire")) {
        return PSEMU_BUTTON_FIRE;
    }
    if (!strcmp(name, "up")) {
        return PSEMU_BUTTON_UP;
    }
    if (!strcmp(name, "down")) {
        return PSEMU_BUTTON_DOWN;
    }
    if (!strcmp(name, "left")) {
        return PSEMU_BUTTON_LEFT;
    }
    if (!strcmp(name, "right")) {
        return PSEMU_BUTTON_RIGHT;
    }
    return 0;
}

int main(int argc, char **argv) {
    size_t bios_size = 0, card_size = 0, sav_size = 0;
    uint8_t *bios, *card, *sav;
    psemu_t *ps;
    uint32_t button = PSEMU_BUTTON_FIRE;
    /* This value agrees with BUTTON_MIN_PRESS_FRAMES in the desktop frontend: a real key press stays
       asserted for this number of emulated frames or more, for each real key-down duration. */
    long hold_frames = 2;
    long frame;
    uint32_t last_bank_mask;
    long dispatch_entries = 0;
    uint32_t trigger_pc = 0;
    int dumped_trigger = 0;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <bios.bin> <card.mcr> <state.sav> [button] [hold_frames] [trigger_pc_hex]\n",
            argv[0]);
        return 1;
    }
    if (argc >= 5) {
        button = parse_button(argv[4]);
    }
    if (argc >= 6) {
        hold_frames = strtol(argv[5], NULL, 10);
    }
    if (argc >= 7) {
        trigger_pc = (uint32_t)strtoul(argv[6], NULL, 16);
    }
    bios = read_file(argv[1], &bios_size);
    card = read_file(argv[2], &card_size);
    sav = strcmp(argv[3], "boot") ? read_file(argv[3], &sav_size) : NULL;
    if (!bios || !card || (!sav && strcmp(argv[3], "boot") != 0)) {
        fprintf(stderr, "failed to read inputs\n");
        return 1;
    }

    ps = psemu_create();
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK || psemu_load_content(ps, card, card_size) != PSEMU_OK) {
        fprintf(stderr, "bad BIOS or card image\n");
        return 1;
    }
    psemu_reset(ps);

    if (sav && argc >= 5 && !strcmp(argv[4], "thresh")) {
        /* Measure the two durations that together decide whether the exit of an app operates. The
           measurement is in emulated milliseconds, and not in whole frames.
             T_bios is the time that Action must stay asserted before the BIOS browse screen counts
             it as a press.
             T_exit is the time after the press that the app needs to return control.
           A press is safe only when T_bios <= press <= T_exit. Real hardware accepts a press of
           approximately 40ms. Thus, if T_bios is much more than 40ms, the timing of the core closed
           the window, and not the frontend. */
        uint8_t *snapshot;
        uint32_t ms;
        long i;
        if (sav_size < QUICKSAVE_HEADER_SIZE + psemu_state_size(ps) ||
            psemu_load_state(ps, sav + QUICKSAVE_HEADER_SIZE, sav_size - QUICKSAVE_HEADER_SIZE) != PSEMU_OK) {
            fprintf(stderr, "bad state file\n");
            return 1;
        }

        /* T_exit: the time from a confirming press of one frame until the app issues its exit
           syscall. One frame is the shortest press that the app accepts. This code uses one frame,
           and not three, thus the button is certainly released before the exit, and the exit is
           correct. */
        {
            int departed = 0;
            for (i = 0; i < 40 && !departed; i++) {
                uint32_t c = 0;
                psemu_set_buttons(ps, i == 0 ? PSEMU_BUTTON_FIRE : 0u);
                while (c < FRAME_CYCLES) {
                    uint32_t pc = ps->cpu.r[15];
                    if (!departed && pc == 8u && ps->cpu.r[14] >= 0x02000000u && ps->cpu.r[14] < 0x03000000u) {
                        uint32_t caller_cpsr = ps->cpu.spsr_bank[arm_current_bank(&ps->cpu)];
                        uint32_t swi_addr = (caller_cpsr & CPSR_T) ? ps->cpu.r[14] - 2u : ps->cpu.r[14] - 4u;
                        if ((psemu_bus_read16(&ps->bus, swi_addr) & 0xFFu) == 0x16u) {
                            departed = 1;
                        }
                    }
                    c += psemu_run(ps, 1u);
                }
            }
            printf("T_exit: app issued its departure syscall during frame %lld after the press (~%.0f ms)\n",
                (long long)(i - 1), (double)(i - 1) * 1000.0 * FRAME_CYCLES / PSEMU_ASSUMED_CPU_HZ);
        }

        /* Wait for the browse screen, and then record a state. Thus each trial below starts from the
           same condition. psemu_app_running has a grace period of one second before it reports that
           the app stopped. Thus this code waits longer than the screen needs. */
        for (i = 0; i < 80; i++) {
            run_frame(ps, 0u);
        }
        if (psemu_app_running(ps)) {
            fprintf(stderr, "expected the BIOS browse screen here, but an app is running\n");
            return 1;
        }
        snapshot = (uint8_t *)malloc(psemu_state_size(ps));
        psemu_save_state(ps, snapshot, psemu_state_size(ps));

        printf("T_bios: shortest Action press the browse screen registers -\n");
        for (ms = 5; ms <= 160; ms += 5) {
            uint32_t cycles = (uint32_t)((uint64_t)PSEMU_ASSUMED_CPU_HZ * ms / 1000u);
            psemu_load_state(ps, snapshot, psemu_state_size(ps));
            press_for_cycles(ps, PSEMU_BUTTON_FIRE, cycles, 60);
            printf("  %3u ms (%5u cycles, %4.1f frames): %s\n", ms, cycles,
                (double)cycles / FRAME_CYCLES, psemu_app_running(ps) ? "REGISTERED" : "ignored");
        }
        /* The window above is narrow. Thus a whole-frame press can be in the window only by chance,
           and the result depends on the position of the press in the tick of the app. This code does
           the exit again with each press length, and with a range of sub-frame offsets. It counts the
           number of correct exits. */
        printf("\nclean-exit rate by press length, swept across timing offsets -\n");
        for (i = 1; i <= 5; i++) {
            long preroll, jitter, clean = 0, trials = 0;
            /* Test both axes of a press: the frame where it starts, against the animation cycle of
               the app, and its position in that frame. A press length is usable only if it operates
               correctly at each combination, because a user controls neither axis. */
            for (preroll = 0; preroll < 8; preroll++) {
                for (jitter = 0; jitter < FRAME_CYCLES; jitter += FRAME_CYCLES / 8) {
                    long f;
                    uint32_t c = 0;
                    psemu_load_state(ps, sav + QUICKSAVE_HEADER_SIZE, sav_size - QUICKSAVE_HEADER_SIZE);
                    for (f = 0; f < preroll; f++) {
                        run_frame(ps, 0u);
                    }
                    psemu_set_buttons(ps, 0u);
                    while (c < (uint32_t)jitter) {
                        c += psemu_run(ps, 1u);
                    }
                    for (f = 0; f < 70; f++) {
                        run_frame(ps, f < i ? PSEMU_BUTTON_FIRE : 0u);
                    }
                    trials++;
                    /* This code uses the true PC value, and not psemu_app_running. That function has
                       a grace period of one second before it reports that an app stopped. That period
                       is longer than this wait, thus the function reports a correct exit as a new
                       start. After a correct exit, the BIOS browse screen executes. After a new start,
                       the PC is in the FLASH1 window of the app. */
                    if ((ps->cpu.r[15] >> 24) == 0x04u) {
                        clean++;
                    }
                }
            }
            printf("  %ld-frame press (%.0f ms): clean exit in %ld of %ld timing offsets\n", i,
                (double)i * 1000.0 * FRAME_CYCLES / PSEMU_ASSUMED_CPU_HZ, clean, trials);
        }

        free(snapshot);
        psemu_destroy(ps);
        return 0;
    }

    if (!sav) {
        /* "boot" mode: this mode uses no save state. It does the recorded cold-boot navigation
           (Down, Action, Right, Action - see docs/app-notes.md), with presses of exactly
           `hold_frames` frames. It then reports whether the navigation gets to the app. The button
           latch of the desktop frontend prevents this fault. Thus each change to the length of that
           latch must continue to pass this test. */
        /* The same repeating cycle of Down, Action, Right, and Action that tools/pk_exit_test.c and
           button_sim=3 in tools/inspect.c use. A test against real hardware confirms this sequence.
           The sequence repeats: thus a press that occurs too early for the current stage of the BIOS
           animation still occurs at the correct time in a later cycle. Only the press length changes
           here. The phase offsets are the offsets from pk_exit_test.c. */
        static const long phase_start[] = {20, 50, 90, 130};
        static const uint32_t sequence[] = {
            PSEMU_BUTTON_DOWN, PSEMU_BUTTON_FIRE, PSEMU_BUTTON_RIGHT, PSEMU_BUTTON_FIRE};
        printf("boot navigation with %ld-frame presses:\n", hold_frames);
        /* A large budget: the cycle repeats each 240 frames, and docs/app-notes.md gives that the
           input code of the real BIOS sometimes needs more than one correct attempt to accept a
           press. A press length that needs a few more cycles is usable. A press length that never
           succeeds is not usable. */
        for (frame = 0; frame < 2400 && !psemu_app_running(ps); frame++) {
            long phase = frame % 240;
            uint32_t buttons = 0;
            uint32_t c = 0;
            size_t s;
            for (s = 0; s < 4; s++) {
                if (phase >= phase_start[s] && phase < phase_start[s] + hold_frames) {
                    buttons = sequence[s];
                }
            }
            psemu_set_buttons(ps, buttons);
            while (c < FRAME_CYCLES) {
                c += psemu_run(ps, 1u);
            }
        }
        printf("  stopped at frame %ld, pc=0x%08X\n", frame, ps->cpu.r[15]);
        {
            int reached = psemu_app_running(ps);
            printf("boot navigation %s with %ld-frame presses\n", reached ? "REACHED THE APP" : "FAILED",
                hold_frames);
            dump_lcd(ps, "end of boot navigation");
            psemu_destroy(ps);
            return reached ? 0 : 1;
        }
    }

    if (sav_size < QUICKSAVE_HEADER_SIZE + psemu_state_size(ps)) {
        fprintf(stderr, "state file too small: %u bytes, need %u+%u\n", (unsigned)sav_size,
            (unsigned)QUICKSAVE_HEADER_SIZE, (unsigned)psemu_state_size(ps));
        return 1;
    }
    if (psemu_load_state(ps, sav + QUICKSAVE_HEADER_SIZE, sav_size - QUICKSAVE_HEADER_SIZE) != PSEMU_OK) {
        fprintf(stderr, "psemu_load_state rejected the blob\n");
        return 1;
    }
    last_bank_mask = ps->flash.bank_mask;

    printf("loaded: pc=0x%08X app_running=%d buttons=0x%02X bank_mask=0x%08X\n", ps->cpu.r[15],
        psemu_app_running(ps), ps->buttons, ps->flash.bank_mask);
    dump_lcd(ps, "at state load");

    for (frame = -5; frame < 400; frame++) {
        uint32_t frame_cycles = 0;
        if (frame == 0) {
            printf("\n--- pressing button 0x%02X (holding %ld frames) ---\n", button, button ? (long)hold_frames : 0);
        }
        if (frame == hold_frames) {
            printf("--- releasing ---\n");
        }
        /* This call occurs one time for each frame, in each condition, exactly as the main loop of the
           desktop frontend does. This is different from a call at only the press edge and the release
           edge. A second call while the user still holds the button clears the HOLD bit, and it leaves
           STATUS asserted (see psemu_set_buttons). Thus the BIOS receives one interrupt request for
           each press, and not one permanently latched request. */
        psemu_set_buttons(ps, (frame >= 0 && frame < hold_frames) ? button : 0u);

        while (frame_cycles < FRAME_CYCLES) {
            uint32_t pc = ps->cpu.r[15];

            /* An SWI vector entry (pc == 8) from FLASH1 shows that the app issues a real syscall.
               Decode the syscall number the same way that tools/inspect.c and the real BIOS handler
               do. */
            if (pc == 8u && ps->cpu.r[14] >= 0x02000000u && ps->cpu.r[14] < 0x03000000u) {
                uint32_t caller_cpsr = ps->cpu.spsr_bank[arm_current_bank(&ps->cpu)];
                uint32_t swi_addr = (caller_cpsr & CPSR_T) ? ps->cpu.r[14] - 2u : ps->cpu.r[14] - 4u;
                uint32_t swi_word = (caller_cpsr & CPSR_T) ? psemu_bus_read16(&ps->bus, swi_addr)
                                                           : psemu_bus_read32(&ps->bus, swi_addr);
                printf("  f%3ld: app SWI #0x%02X from 0x%08X (r0=0x%08X)\n", frame, swi_word & 0xFFu, swi_addr,
                    ps->cpu.r[0]);
                /* The trigger: write the PC ring of the CPU at the moment that the app issues the
                   syscall at this address. The ring gives the full path to that point. This code uses
                   an SWI address, and not an arbitrary PC value, because this loop samples the PC only
                   between psemu_run calls. psemu_run can complete several instructions in one call,
                   and the number depends on the current CLK_MODE. This code finds each SWI, because
                   SWI entry keeps the PC on the vector for a full step. */
                if (trigger_pc != 0 && swi_addr == trigger_pc && !dumped_trigger) {
                    dumped_trigger = 1;
                    printf("\n=== trace leading to the SWI at 0x%08X (frame %ld) ===\n", swi_addr, frame);
                    psemu_write_crash_report(ps, stdout);
                    printf("=== end trace ===\n\n");
                }
            }
            if (pc >= DISPATCH_LO && pc < DISPATCH_HI && !(ps->cpu.cpsr & CPSR_T)) {
                if (dispatch_entries == 0) {
                    printf("  f%3ld: *** BIOS 0x04001900-0x04001C00 window entered, pc=0x%08X ***\n", frame, pc);
                }
                dispatch_entries++;
            }

            frame_cycles += psemu_run(ps, 1u);
            if (psemu_cpu_faulted(ps)) {
                printf("  f%3ld: CPU FAULTED pc=0x%08X\n", frame, ps->cpu.r[15]);
                goto done;
            }
        }

        if (ps->flash.bank_mask != last_bank_mask) {
            printf("  f%3ld: bank_mask 0x%08X -> 0x%08X\n", frame, last_bank_mask, ps->flash.bank_mask);
            last_bank_mask = ps->flash.bank_mask;
        }
        if (frame == 10 || frame == 60 || frame == 150 || frame == 399) {
            char label[64];
            snprintf(label, sizeof(label), "frame %ld", frame);
            dump_lcd(ps, label);
        }
    }

    /* The second phase: the BIOS browse screen is now active, which shows a correct exit. This code
       presses Action again for the same number of frames, and finds whether the browse screen accepts
       the press and starts the app. This measures the other half of the button-latch compromise of the
       frontend: a press that is too short never registers with the BIOS, and a press that is too long
       continues after the exit of an app and starts that app again. */
    printf("\n--- phase 2: tapping Action on the browse screen for %ld frames ---\n", hold_frames);
    for (frame = 0; frame < hold_frames; frame++) {
        uint32_t frame_cycles = 0;
        psemu_set_buttons(ps, PSEMU_BUTTON_FIRE);
        while (frame_cycles < FRAME_CYCLES) {
            frame_cycles += psemu_run(ps, 1u);
        }
    }
    for (frame = 0; frame < 200; frame++) {
        uint32_t frame_cycles = 0;
        psemu_set_buttons(ps, 0);
        while (frame_cycles < FRAME_CYCLES) {
            frame_cycles += psemu_run(ps, 1u);
        }
        if (psemu_app_running(ps)) {
            printf("phase 2: browse screen REGISTERED the press - app launched at frame %ld\n", frame);
            break;
        }
    }
    if (!psemu_app_running(ps)) {
        printf("phase 2: browse screen did NOT register a %ld-frame press\n", hold_frames);
    }

done:
    printf("\nfinal: pc=0x%08X app_running=%d dispatch_instructions=%ld\n", ps->cpu.r[15], psemu_app_running(ps),
        dispatch_entries);

    psemu_destroy(ps);
    free(bios);
    free(card);
    free(sav);
    return 0;
}
