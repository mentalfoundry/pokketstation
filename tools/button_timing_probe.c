/* Diagnostic: measure how long a button press may be asserted before it breaks something, against the real
   BIOS and a real app. This exists because the desktop frontend's button-press minimum (see
   BUTTON_MIN_PRESS_FRAMES in frontends/desktop/main.c) is squeezed from both sides, and neither bound is
   guessable - both have to be measured.

   State mode: load a desktop-frontend save state parked on an app's exit screen, tap a button, and report
   what the machine does next - every syscall the app issues, whether the BIOS's app-dispatch window runs
   again (which is what "the app just relaunched" looks like from underneath), and what the LCD shows at each
   stage. Then, with the BIOS browse screen up, tap Action again to check the browse screen still registers
   an ordinary press of the same length.

   Boot mode (pass "boot" instead of a state file): drive the documented cold-boot navigation with presses of
   the given length, and report whether the app is reached at all.

   Threshold mode (pass "thresh" as the button): measure the two durations that between them decide whether
   an app's exit works, in emulated real time rather than frames, and then sweep whole-frame press lengths
   across both timing axes a press can land on.

   Measured against the J110 BIOS plus one real commercial app that has an in-app exit screen and does not
   wait for release before departing (a card image in testdata/, gitignored):

     the browse screen ignores an Action press shorter than ~35ms - close to the ~40ms real hardware tap
     docs/app-notes.md describes, so the core's own timing here is not the problem
     that app's exit screen departs ~62-94ms after the press, varying with where the press lands in its tick

   which leaves a usable window of roughly 35-62ms. A frame is 31.25ms, so a frontend sampling buttons once
   per frame can only offer 31ms, 62ms, 94ms and up, and exactly one of those fits:

     1 frame  (31ms)  exit clean 64/64 offsets, but boot navigation never reaches an app and the browse
                      screen ignores the press
     2 frames (62ms)  exit clean 64/64 offsets, boot navigation reaches the app, browse screen registers
     3 frames (94ms)  exit clean 27/64 offsets - lands in the window only by luck
     4 frames (125ms) exit clean 1/64 offsets
     5 frames (156ms) exit clean 1/64 offsets

   usage: button_timing_probe <bios.bin> <card.mcr> <state.sav|boot> [button|thresh] [hold_frames]
                              [trigger_pc_hex]
          button:         fire (default) | up | down | left | right | none | thresh
          hold_frames:    frames the button is asserted to the core (default 2)
          trigger_pc_hex: dump the CPU's PC ring when the app issues the syscall at this address */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

/* Matches frontends/desktop/main.c's quicksave_header_t. */
#define QUICKSAVE_HEADER_SIZE 16u

/* One emulated frame at the desktop frontend's 33000-cycle-per-frame budget, expressed in single steps is
   not a fixed number, so the probe counts real cycles instead. */
#define FRAME_CYCLES 33000u

/* The window tools/inspect.c watches for the real BIOS's app-dispatch routine, the code that builds
   F_BANK_FLG from a directory chain and branches to an app's entry point (its tail is at 0x04001AF8). The
   window is wider than that routine: 0x04001BC8, the SWI 0x16 handler an app calls to read the selected app
   slot, also falls inside it. So a hit here means "worth looking at", not "the app was relaunched" - use the
   trace dump for that. */
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

/* Runs one whole frame at the desktop frontend's per-frame budget, re-asserting `buttons` first exactly as
   that frontend's main loop does. */
static void run_frame(psemu_t *ps, uint32_t buttons) {
    uint32_t c = 0;
    psemu_set_buttons(ps, buttons);
    while (c < FRAME_CYCLES) {
        c += psemu_run(ps, 1u);
    }
}

/* Asserts `buttons` for `press_cycles` reference cycles, then releases, then runs `settle_frames`.
   press_cycles is a real-time duration at PSEMU_ASSUMED_CPU_HZ, so it can be shorter than one frame - which
   is the point: it measures what the core itself needs, with the frontend's once-per-frame sampling taken
   out of the picture. Buttons are re-asserted on every frame boundary inside the press, matching what the
   frontend does for any press long enough to span one. */
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
    /* Matches the desktop frontend's BUTTON_MIN_PRESS_FRAMES: a real key tap is asserted for at least this
       many emulated frames, however briefly the key itself was down. */
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
        /* Measure, in emulated milliseconds rather than whole frames, the two durations that between them
           decide whether an app's exit works:
             T_bios - how long Action must be asserted before the BIOS browse screen counts it as a press
             T_exit - how long after the press the app takes to hand control back
           A press is safe only when T_bios <= press <= T_exit. Real hardware registers a ~40ms tap, so if
           T_bios lands far above that, the core's own timing is what closed the window - not the frontend. */
        uint8_t *snapshot;
        uint32_t ms;
        long i;
        if (sav_size < QUICKSAVE_HEADER_SIZE + psemu_state_size(ps) ||
            psemu_load_state(ps, sav + QUICKSAVE_HEADER_SIZE, sav_size - QUICKSAVE_HEADER_SIZE) != PSEMU_OK) {
            fprintf(stderr, "bad state file\n");
            return 1;
        }

        /* T_exit: from a one-frame confirming press - the shortest the app itself accepts - how long until it
           issues its departure syscall. One frame, not three, so the button is certainly long gone by then
           and the exit is clean. */
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

        /* Settle on the browse screen, then snapshot it so every trial below starts from the same place.
           psemu_app_running has a one-second grace before it reports the app gone, so this waits longer than
           the screen itself needs. */
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
        /* The window above is narrow enough that whole-frame presses may only land in it by luck, depending on
           where in the app's own tick the press happens to fall. Re-run the exit with each press length and a
           sweep of sub-frame offsets, and count how often the exit actually stays clean. */
        printf("\nclean-exit rate by press length, swept across timing offsets -\n");
        for (i = 1; i <= 5; i++) {
            long preroll, jitter, clean = 0, trials = 0;
            /* Sweep both axes the press can land on: which frame it starts in relative to the app's own
               animation cycle, and where inside that frame. A press length is only usable if it survives
               every one of them, because a user has no control over either. */
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
                    /* Where the PC actually is, not psemu_app_running: that has a one-second grace before it
                       reports an app gone, which is longer than this settle window and would score a clean
                       exit as a relaunch. A clean exit leaves the BIOS browse screen executing; a relaunch
                       puts the PC back in the app's FLASH1 window. */
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
        /* "boot" mode: no save state. Drive the documented cold-boot navigation (Down, Action, Right, Action -
           see docs/app-notes.md) using presses exactly `hold_frames` long, and report whether the app is
           reached. This is the regression the desktop frontend's button latch exists to prevent, so any
           change to that latch's length has to keep passing here. */
        /* The same repeating Down / Action / Right / Action cycle tools/pk_exit_test.c and tools/inspect.c's
           button_sim=3 use, confirmed end-to-end against real hardware. It repeats so a press that lands too
           early for whatever stage the BIOS animation is on still lands correctly on a later cycle. Only the
           press length varies here; the phase offsets are pk_exit_test.c's. */
        static const long phase_start[] = {20, 50, 90, 130};
        static const uint32_t sequence[] = {
            PSEMU_BUTTON_DOWN, PSEMU_BUTTON_FIRE, PSEMU_BUTTON_RIGHT, PSEMU_BUTTON_FIRE};
        printf("boot navigation with %ld-frame presses:\n", hold_frames);
        /* Generous budget: the cycle repeats every 240 frames, and docs/app-notes.md notes the real BIOS's
           input handling sometimes needs more than one clean attempt to register a press. A press length that
           only needs a couple of extra cycles is usable; one that never gets through is not. */
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
        /* Once per frame, unconditionally, exactly as the desktop frontend's main loop does. This is not the
           same as calling it only on the press and release edges: a repeat call with the button still held
           clears its HOLD bit while leaving STATUS asserted (see psemu_set_buttons), so the BIOS sees one
           interrupt request per press rather than a permanently latched one. */
        psemu_set_buttons(ps, (frame >= 0 && frame < hold_frames) ? button : 0u);

        while (frame_cycles < FRAME_CYCLES) {
            uint32_t pc = ps->cpu.r[15];

            /* An SWI vector entry (pc==8) reached from FLASH1 is the app issuing a real syscall. Decode the
               number the same way tools/inspect.c and the real BIOS handler do. */
            if (pc == 8u && ps->cpu.r[14] >= 0x02000000u && ps->cpu.r[14] < 0x03000000u) {
                uint32_t caller_cpsr = ps->cpu.spsr_bank[arm_current_bank(&ps->cpu)];
                uint32_t swi_addr = (caller_cpsr & CPSR_T) ? ps->cpu.r[14] - 2u : ps->cpu.r[14] - 4u;
                uint32_t swi_word = (caller_cpsr & CPSR_T) ? psemu_bus_read16(&ps->bus, swi_addr)
                                                           : psemu_bus_read32(&ps->bus, swi_addr);
                printf("  f%3ld: app SWI #0x%02X from 0x%08X (r0=0x%08X)\n", frame, swi_word & 0xFFu, swi_addr,
                    ps->cpu.r[0]);
                /* trigger: dump the CPU's own PC ring the moment the app issues the syscall at this address,
                   which gives the whole path that led there. An SWI site is used rather than an arbitrary PC
                   because this loop only samples the PC between psemu_run calls, and psemu_run can retire
                   several instructions per call depending on the current CLK_MODE. Every SWI is caught,
                   because SWI entry parks the PC on the vector for a full step. */
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

    /* Second phase: with the BIOS browse screen now up (a clean exit), tap Action again for the same number
       of frames and see whether the browse screen registers it and launches the app. This measures the other
       half of the frontend's button-latch trade-off: too short a press never registers with the BIOS at all,
       too long a press outlives an app's departure and relaunches it. */
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
