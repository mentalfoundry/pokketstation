/* Diagnostic: drive two independent psemu_t instances against each other over IR, in a single process, with a
   direct in-memory edge relay and no pipe/IPC involved at all.

   Purpose: isolate which layer an IR transfer actually fails at. The desktop frontend's IR Link relays edges
   once per rendered frame (~31ms of emulated time per psemu_run call), which is a very coarse interleave for a
   protocol that is bit-banged at microsecond scale. This probe makes the interleave granularity a parameter, so
   the same transfer can be run at frame granularity (33000 cycles, matching the frontend exactly) and at
   arbitrarily fine granularity (a few hundred cycles), with everything else held constant.

   If a transfer succeeds at fine granularity but fails at frame granularity, the frontend's per-frame relay is
   the problem, not the core IR model in core/src/ir.c. If it fails at both, the problem is in the core model
   (or in what the app expects from these registers) instead.

   Both instances start from the same save state, so the app is already sitting on its IR screen. Instance A is
   told to transmit, instance B to receive, via the same button presses a user would make.

   usage: ir_probe <bios.bin> <app.mcs> <quicksave.dat> [slice_cycles] [frames] [scriptA] [scriptB] [trace]
     slice_cycles  emulated cycles to run each instance before exchanging edges. 33000 = one frontend frame
                   (the frontend's real behavior). Smaller = finer interleaving. Default 33000.
     frames        total emulated frames to run. Default 400 (~12s of emulated time).
     scriptA/B     per-instance button script: comma-separated "button@start-end" entries, frame-numbered.
                   Buttons: up, down, left, right, fire. Example: "up@20-30,fire@40-50".
                   "-" means no input. Default "up@20-30" for A and "down@20-30" for B.
     trace         "trace" logs every IR register access on both instances with its PC; "traceA"/"traceB"
                   restrict that log to one instance, which is what makes the two sides' roles separable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

#define QUICKSAVE_HEADER_SIZE 16 /* magic[4] + version + app_size + app_hash; see frontends/desktop/main.c */
#define FRAME_CYCLES 33000u

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

/* Parses one "button@start-end" entry set (see usage above) and returns the buttons held at `frame`. */
static uint32_t buttons_at_frame(const char *script, long frame) {
    const char *p = script;
    uint32_t held = 0;
    if (!script || strcmp(script, "-") == 0) {
        return 0;
    }
    while (*p) {
        char name[16];
        size_t n = 0;
        long start, end;
        while (*p && *p != '@' && n + 1 < sizeof(name)) {
            name[n++] = *p++;
        }
        name[n] = '\0';
        if (*p != '@') {
            break;
        }
        p++;
        start = strtol(p, (char **)&p, 10);
        if (*p == '-') {
            p++;
        }
        end = strtol(p, (char **)&p, 10);
        if (frame >= start && frame < end) {
            if (strcmp(name, "up") == 0) {
                held |= PSEMU_BUTTON_UP;
            } else if (strcmp(name, "down") == 0) {
                held |= PSEMU_BUTTON_DOWN;
            } else if (strcmp(name, "left") == 0) {
                held |= PSEMU_BUTTON_LEFT;
            } else if (strcmp(name, "right") == 0) {
                held |= PSEMU_BUTTON_RIGHT;
            } else if (strcmp(name, "fire") == 0) {
                held |= PSEMU_BUTTON_FIRE;
            }
        }
        if (*p == ',') {
            p++;
        }
    }
    return held;
}

static void print_framebuffer(const psemu_t *ps, const char *label) {
    const uint8_t *fb = psemu_get_framebuffer(ps);
    int row, col;
    printf("--- %s ---\n", label);
    for (row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (col = 0; col < PSEMU_LCD_WIDTH; col++) {
            int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
            int bit_index = col % 8;
            putchar((fb[byte_index] >> bit_index) & 1 ? '#' : '.');
        }
        putchar('\n');
    }
}

static psemu_t *make_instance(const uint8_t *bios, size_t bios_size, const uint8_t *app, size_t app_size,
    const uint8_t *state, size_t state_size, const char *label) {
    psemu_t *ps = psemu_create();
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        fprintf(stderr, "%s: bad BIOS\n", label);
        exit(1);
    }
    if (psemu_load_content(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr, "%s: bad app\n", label);
        exit(1);
    }
    psemu_reset(ps);
    if (state) {
        if (state_size != psemu_state_size(ps)) {
            fprintf(stderr, "%s: quicksave state is %zu bytes, this build expects %zu - rebuild mismatch\n", label,
                state_size, psemu_state_size(ps));
            exit(1);
        }
        if (psemu_load_state(ps, state, state_size) != PSEMU_OK) {
            fprintf(stderr, "%s: psemu_load_state failed\n", label);
            exit(1);
        }
    }
    return ps;
}

/* Moves every pending TX edge from `from` into `to`'s RX queue.
   Both instances are stepped in lockstep here, with identical cycle budgets, so their IR clocks track each
   other almost exactly - an edge's timestamp is relayed as-is, with no wall-clock conversion and no playout
   delay. That is deliberately the most favorable possible case: perfect clock alignment and minimum latency.
   If a transfer still fails under these conditions, no amount of transport tuning in the frontend will fix it. */
static int relay_edges(psemu_t *from, psemu_t *to, const char *direction, int verbose) {
    ir_edge_t edge;
    int relayed = 0;
    while (ir_pop_tx_edge(&from->ir, &edge)) {
        ir_push_rx_edge(&to->ir, edge.timestamp_cycles, edge.level);
        relayed++;
        if (verbose) {
            printf("  [relay %s] t=%llu level=%d\n", direction, (unsigned long long)edge.timestamp_cycles,
                edge.level);
        }
    }
    return relayed;
}

int main(int argc, char **argv) {
    size_t bios_size = 0, app_size = 0, save_size = 0;
    uint8_t *bios, *app, *save;
    const uint8_t *state = NULL;
    size_t state_size = 0;
    psemu_t *a, *b;
    uint32_t slice_cycles = FRAME_CYCLES;
    long frames = 400;
    int trace = 0;
    long f;
    long total_a_to_b = 0, total_b_to_a = 0;
    uint32_t last_a_mode = 0xFFFFFFFFu, last_b_mode = 0xFFFFFFFFu;
    /* Reconstructs what the receive handler itself measures: Timer2's live count sampled at each change of
       B's demodulated line level, and the tick delta between consecutive changes. The handler accepts a pulse
       only when |delta - 4*unit| <= unit/2, so these deltas are directly comparable against that window. */
    int last_rx_level = -1;
    uint32_t prev_t2 = 0;
    int have_prev_t2 = 0;
#define MEAS_MAX 24
    uint32_t meas[MEAS_MAX];
    int meas_n = 0;

    const char *script_a = "up@20-30";
    const char *script_b = "down@20-30";

    if (argc < 4) {
        fprintf(stderr,
            "usage: %s <bios.bin> <app.mcs> <quicksave.dat> [slice_cycles] [frames] [scriptA] [scriptB] [trace]\n",
            argv[0]);
        return 1;
    }
    bios = read_file(argv[1], &bios_size);
    app = read_file(argv[2], &app_size);
    save = read_file(argv[3], &save_size);
    if (!bios || !app || !save) {
        fprintf(stderr, "failed to read one of the input files\n");
        return 1;
    }
    if (save_size > QUICKSAVE_HEADER_SIZE) {
        state = save + QUICKSAVE_HEADER_SIZE;
        state_size = save_size - QUICKSAVE_HEADER_SIZE;
    }
    if (argc >= 5) {
        slice_cycles = (uint32_t)atol(argv[4]);
    }
    if (argc >= 6) {
        frames = atol(argv[5]);
    }
    if (argc >= 7) {
        script_a = argv[6];
    }
    if (argc >= 8) {
        script_b = argv[7];
    }
    if (argc >= 9) {
        if (strcmp(argv[8], "trace") == 0) {
            trace = 3; /* both */
        } else if (strcmp(argv[8], "traceA") == 0) {
            trace = 1;
        } else if (strcmp(argv[8], "traceB") == 0) {
            trace = 2;
        }
    }

    a = make_instance(bios, bios_size, app, app_size, state, state_size, "A");
    b = make_instance(bios, bios_size, app, app_size, state, state_size, "B");

    printf("slice_cycles=%u (frontend frame = %u), frames=%ld, slices/frame=%u\n", slice_cycles, FRAME_CYCLES,
        frames, FRAME_CYCLES / (slice_cycles ? slice_cycles : 1u));
    printf("A script: %s\nB script: %s\n\n", script_a, script_b);

    for (f = 0; f < frames; f++) {
        uint32_t cycles_this_frame = 0;
        /* Buttons are held across a stretch of frames, the same way a real press lands across several frames
           at 32Hz (see BUTTON_LATCH_FRAMES in the desktop frontend). */
        psemu_set_buttons(a, buttons_at_frame(script_a, f));
        psemu_set_buttons(b, buttons_at_frame(script_b, f));

        while (cycles_this_frame < FRAME_CYCLES) {
            uint32_t slice = slice_cycles;
            if (slice == 0u || slice > FRAME_CYCLES - cycles_this_frame) {
                slice = FRAME_CYCLES - cycles_this_frame;
            }
            /* The trace flag is global, so it is toggled around each instance's own psemu_run - that is what
               attributes each logged register access to the instance that actually made it. */
            psemu_ir_trace_enabled = (trace & 1) != 0;
            psemu_run(a, slice);
            psemu_ir_trace_enabled = (trace & 2) != 0;
            psemu_run(b, slice);
            psemu_ir_trace_enabled = 0;
            if (b->ir.rx_level != last_rx_level) {
                uint32_t t2 = b->timer.timers[2].count;
                if (have_prev_t2 && meas_n < MEAS_MAX) {
                    /* Timer2 counts down and reloads at 0xFFFF, so an unsigned 16-bit difference handles the
                       wrap the same way the handler's own `lsl/lsr #16` truncation does. */
                    meas[meas_n++] = (prev_t2 - t2) & 0xFFFFu;
                }
                prev_t2 = t2;
                have_prev_t2 = 1;
                last_rx_level = b->ir.rx_level;
            }
            total_a_to_b += relay_edges(a, b, "A->B", trace);
            total_b_to_a += relay_edges(b, a, "B->A", trace);
            cycles_this_frame += slice;
        }

        if (a->ir.mode != last_a_mode) {
            printf("frame %ld: A IRDA_MODE 0x%08X -> 0x%08X (IFMODE=%s STDBY=%u BGEN=%u BFLT=%u)\n", f, last_a_mode,
                a->ir.mode, (a->ir.mode & IR_MODE_IFMODE) ? "TX" : "RX", !!(a->ir.mode & IR_MODE_STDBY),
                !!(a->ir.mode & IR_MODE_BGEN), !!(a->ir.mode & IR_MODE_BFLT));
            last_a_mode = a->ir.mode;
        }
        if (b->ir.mode != last_b_mode) {
            printf("frame %ld: B IRDA_MODE 0x%08X -> 0x%08X (IFMODE=%s STDBY=%u BGEN=%u BFLT=%u)\n", f, last_b_mode,
                b->ir.mode, (b->ir.mode & IR_MODE_IFMODE) ? "TX" : "RX", !!(b->ir.mode & IR_MODE_STDBY),
                !!(b->ir.mode & IR_MODE_BGEN), !!(b->ir.mode & IR_MODE_BFLT));
            last_b_mode = b->ir.mode;
        }
    }

    printf("\nedges relayed: A->B %ld, B->A %ld\n", total_a_to_b, total_b_to_a);
    printf("A: cpu_faulted=%d  B: cpu_faulted=%d\n", psemu_cpu_faulted(a), psemu_cpu_faulted(b));
    /* Whether the receiving side ever even enabled the IR interrupt decides how it is meant to be driven:
       INT_IRDA-on-edge, or polling IRDA_DATA directly. */
    printf("A intc: enable=0x%08X hold=0x%08X (INT_IRDA enabled=%s)\n", a->intc.enable, a->intc.hold,
        (a->intc.enable & INT_IRDA) ? "yes" : "no");
    printf("B intc: enable=0x%08X hold=0x%08X (INT_IRDA enabled=%s)\n", b->intc.enable, b->intc.hold,
        (b->intc.enable & INT_IRDA) ? "yes" : "no");
    /* Both instances are stepped with identical cycle budgets, so their IR clocks should stay locked to each
       other. Any drift here means relayed edge timestamps land in the receiver's future (or past), which
       shows up as a standing rx_queue backlog. */
    printf("A ir: clock=%llu\n", (unsigned long long)a->ir.clock_cycles);
    printf("B ir: rx_level=%d rx_queue_count=%u clock=%llu (A-B skew=%lld)\n", b->ir.rx_level, b->ir.rx_queue.count,
        (unsigned long long)b->ir.clock_cycles,
        (long long)a->ir.clock_cycles - (long long)b->ir.clock_cycles);
    /* ir.h documents that the real INT_IRDA handler measures an incoming pulse's length by reading Timer2's
       live counter. If Timer2 is not actually running on the receiving side, every measured pulse width would
       come out identical and no decode could ever succeed. */
    {
        int t;
        for (t = 0; t < 3; t++) {
            printf("A timer%d: period=%u control=0x%X | B timer%d: period=%u control=0x%X\n", t,
                a->timer.timers[t].period, a->timer.timers[t].control, t, b->timer.timers[t].period,
                b->timer.timers[t].control);
        }
    }
    psemu_ir_trace_enabled = 0;
    /* Dump the FLASH1-mapped app region containing the IR routines, so it can be disassembled offline. The
       bytes are read back through the bus exactly as the CPU sees them, which resolves the FLASH1->FLASH2
       bank mapping without having to reproduce that mapping in an external tool. */
    {
        FILE *dump = fopen("ir_code_dump.bin", "wb");
        if (dump) {
            uint32_t addr;
            for (addr = 0x02004000u; addr < 0x0200E800u; addr++) {
                uint8_t byte = psemu_bus_read8(&b->bus, addr);
                fwrite(&byte, 1, 1, dump);
            }
            fclose(dump);
            printf("wrote ir_code_dump.bin (0x02004000-0x0200E800)\n");
        }
    }
    printf("\nA clk=%u Hz  B clk=%u Hz  (PSEMU_ASSUMED_CPU_HZ reference = %u)\n", clk_current_hz(&a->clk),
        clk_current_hz(&b->clk), (unsigned)PSEMU_ASSUMED_CPU_HZ);
    {
        uint32_t unit = psemu_bus_read32(&b->bus, 0x000003C4u + 0x20u) & 0xFFFFu;
        int k;
        printf("\nTimer2 tick deltas B actually measures between line-level changes (handler wants %u +- %u):\n",
            4u * unit, unit / 2u);
        for (k = 0; k < meas_n; k++) {
            printf("  [%2d] %6u%s\n", k, meas[k],
                (meas[k] + unit / 2u >= 4u * unit && meas[k] <= 4u * unit + unit / 2u) ? "  <-- in window" : "");
        }
    }
    /* The app's IR state block, resolved from the literal its INT_IRDA handler loads into r7 (0x000003C4 in
       RAM). +0x20 is the nominal pulse-width unit the handler compares measured Timer2 deltas against
       (it accepts |measured - 4*unit| <= unit/2, a +-12.5% window), +0x26 is the expected/own line level,
       and +0x28 is the receive state machine's state. */
    {
        uint32_t sb = 0x000003C4u;
        printf("B IR state block @0x%08X: unit[+0x20]=%u expected_level[+0x26]=%u state[+0x28]=%u\n", sb,
            psemu_bus_read32(&b->bus, sb + 0x20u) & 0xFFFFu, psemu_bus_read8(&b->bus, sb + 0x26u),
            psemu_bus_read8(&b->bus, sb + 0x28u));
        printf("A IR state block: unit[+0x20]=%u expected_level[+0x26]=%u state[+0x28]=%u\n",
            psemu_bus_read32(&a->bus, sb + 0x20u) & 0xFFFFu, psemu_bus_read8(&a->bus, sb + 0x26u),
            psemu_bus_read8(&a->bus, sb + 0x28u));
    }
    /* The app's IRQ dispatcher (found by disassembling the code dump above) reads its per-source handler
       table from a RAM-resident vector table, so the INT_IRDA handler's address is only knowable at runtime.
       Print the table here; entry 12 is INT_IRDA. */
    {
        uint32_t table = psemu_bus_read32(&b->bus, 0x000003F0u);
        int e;
        printf("B handler table ptr@0x3F0 = 0x%08X\n", table);
        for (e = 11; e <= 13; e++) {
            printf("  handler[%d] (INT bit %d%s) = 0x%08X\n", e, e,
                e == 12 ? " = INT_IRDA" : (e == 13 ? " = INT_TIMER2" : ""),
                psemu_bus_read32(&b->bus, 0x000003F0u + (uint32_t)e * 4u));
        }
    }
    /* psemu_write_crash_report dumps the last PSEMU_TRACE_SIZE executed PCs. Stopping the run mid-transfer and
       dumping B's trace is what actually locates the receive-side interrupt handler, which is not reachable by
       static disassembly alone (the app registers it through a kernel callback). */
    {
        FILE *rep = fopen("ir_probe_B_trace.log", "w");
        if (rep) {
            psemu_write_crash_report(b, rep);
            fclose(rep);
            printf("wrote ir_probe_B_trace.log (B's recent PC trace)\n");
        }
    }
    print_framebuffer(a, "A (transmit) final screen");
    print_framebuffer(b, "B (receive) final screen");

    psemu_destroy(a);
    psemu_destroy(b);
    free(bios);
    free(app);
    free(save);
    return 0;
}
