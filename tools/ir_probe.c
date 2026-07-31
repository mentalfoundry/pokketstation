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

   Verdict, printed near the end of a run: whether each side ends up holding the other's hardware id.
   The two instances are given distinct ids on purpose. A real IR message carries the sender's id, and
   neither instance can learn the other's by any route except the link, so this cannot pass by coincidence.

   Byte-for-byte buffer comparison was tried first and misled three separate times, which is worth recording
   so it is not tried again: both instances run the same app from the same save file, so their message
   buffers start identical and "match" before a single edge has been relayed; the buffers are cleared and
   reused immediately after a transfer, so an end-of-run read finds only zeroes; and the field once assumed
   to be the buffer pointer (state block +0x14) is really a pointer to a table of buffer pointers, so
   comparing it compared pointers and neighbouring state. The id check has none of those failure modes.

   Slice size trades relay precision against timing accuracy, and both directions have bitten this tool
   before. psemu_run's budget is in reference-rate cycles converted to real seconds, but its loop always runs
   at least one instruction. When one instruction's real duration exceeds the whole slice budget, that call
   overshoots. A slow app-selected CLK_MODE makes an instruction expensive in real time (at ~254KHz one
   instruction is ~11.8us, against a 3.79us budget at slice_cycles=4), so a finely sliced instance running
   slowly over-advances its own clock badly, while a fast one does not. Two instances at different CLK_MODEs
   therefore appear to drift apart. Measured: ~957000 reference cycles of apparent skew at slice 4, ~78600 at
   64, ~20700 at 256, ~8200 at 1024, with total elapsed time exactly correct only at the coarse end. Use a
   coarse slice when absolute timing matters, and a fine one only when relay precision matters more.

   usage: ir_probe <bios.bin> <app.mcs> <quicksave.dat> [slice_cycles] [frames] [scriptA] [scriptB] [trace]
     slice_cycles  emulated cycles to run each instance before exchanging edges. 33000 = one frontend frame
                   (the frontend's real behavior). Smaller = finer interleaving, but see the note above on
                   what a small value costs in timing accuracy. Default 33000.
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
            fprintf(stderr, "%s: quicksave state is %zu bytes, this build expects %zu. Rebuild mismatch.\n", label,
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
   Both instances step in lockstep here, with identical cycle budgets, so their IR clocks track each other
   almost exactly and no wall-clock conversion is needed.

   This used to relay timestamps as-is and call that "the most favorable case possible". It is the opposite.
   Relaying with no playout delay hands the receiver a batch of edges whose timestamps are already in its
   past, so it releases the whole batch at once and every interval it measures collapses. Measured at
   frontend frame granularity: with no delay the transfer fails outright, and with one frame of delay it
   completes in both directions. The delay defaults to the desktop frontend's own IR_LINK_PLAYOUT_DELAY_US
   so this probe models the real transport rather than a strictly worse one; IR_PROBE_PLAYOUT_DELAY overrides
   it for experiments. */
static int relay_edges(psemu_t *from, psemu_t *to, const char *direction, int verbose, uint64_t playout_delay) {
    ir_edge_t edge;
    int relayed = 0;
    while (ir_pop_tx_edge(&from->ir, &edge)) {
        /* A relay that runs every N cycles hands over edges up to N cycles after they were produced, with
           timestamps already in the receiver's past. The receiver then releases the whole batch at once and
           every interval it measures collapses, which is why a transfer that works at fine granularity fails
           at frame granularity. Adding a fixed playout delay to every edge restores the spacing: the receiver
           holds each edge until delay cycles after it was produced, so relative timing survives any relay
           latency up to that delay. This is the ordinary jitter-buffer trade of latency for correctness. */
        ir_push_rx_edge(&to->ir, edge.timestamp_cycles + playout_delay, edge.level);
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
    /* Transmit-side analysis. Each time A's IRDA_DATA changes, this records the state of all three of A's
       timers, plus the emulated real time since the previous change. That identifies which timer actually
       paces the transmit callback, and what its effective period is, rather than assuming it. */
#define TX_MAX 700
    struct {
        uint64_t at_ref;
        long frame;
        uint32_t period[3], count[3], control[3];
    } tx_ev[TX_MAX];
    int tx_n = 0;
    uint32_t last_a_data = 0xFFFFFFFFu;
#define MEAS_MAX 700
    uint32_t meas[MEAS_MAX];
    long meas_frame[MEAS_MAX];
    int meas_n = 0;
    /* The same measurement, filtered the way the app's own handler filters it (see the rx_level block). */
    uint32_t qual[MEAS_MAX], qual_state[MEAS_MAX];
    long qual_frame[MEAS_MAX];
    int qual_n = 0;
    uint32_t prev_qual_t2 = 0;
    int have_prev_qual_t2 = 0;
    /* One-off instrumentation: neither INT_IRDA nor INT_TIMER2 are enabled for B in this scenario, so B is
       not interrupt-driven here at all - it must be polling directly, and no single PC address is a
       reliable trap point. This instead watches the state (+0x28) and bit-index (+0xC) fields themselves
       for any change, whatever code path produces it. */
    uint32_t last_b_state = 0xFFFFFFFFu, last_b_bitindex = 0xFFFFFFFFu;
    uint32_t max_b_state = 0;
    int state_changes = 0;
    /* bit_index (+0xC) never moved past 0. Confirmed why below: B's buffer is never written at all in this
       scenario, so there is no "real data filling in" for any field to track. Watching the buffer bytes
       directly is what proved that, and mirroring the same watch onto A's own buffer (below) is what showed
       the previously-reported "37 of 41 bytes decode" was really A constructing its own message, compared
       against B's untouched, stale, pre-loaded copy. See docs/hardware-notes.md's "IR / IR Link" notes. */
#define SNAP_MAX 64
    uint8_t a_snap[SNAP_MAX], b_snap[SNAP_MAX];
    uint32_t a_peak_bits = 0, b_peak_bits = 0;
    int a_snap_len = 0, b_snap_len = 0;
    long a_snap_frame = -1, b_snap_frame = -1;
    uint32_t a_snap_addr = 0, b_snap_addr = 0, a_snap_idx = 0, b_snap_idx = 0;
    uint8_t a_first[SNAP_MAX], b_first[SNAP_MAX];
    int a_have_first = 0, b_have_first = 0;
    int a_first_len = 0, b_first_len = 0;
    long a_first_frame = -1, b_first_frame = -1;

    /* The desktop frontend's IR_LINK_PLAYOUT_DELAY_US (100000us) expressed in the reference-rate cycle units
       these timestamps use, so the default models the real transport. */
    uint64_t playout_delay = (100000ull * PSEMU_ASSUMED_CPU_HZ) / 1000000ull;
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

    {
        const char *pd = getenv("IR_PROBE_PLAYOUT_DELAY");
        if (pd) {
            playout_delay = (uint64_t)strtoull(pd, NULL, 10);
            printf("playout delay: %llu reference cycles\n", (unsigned long long)playout_delay);
        }
    }
    a = make_instance(bios, bios_size, app, app_size, state, state_size, "A");
    /* Both instances otherwise run the same app from the same save with the same default hardware ID, and
       a real IR message carries that ID. Identical IDs make "the receiver decoded the sender's message"
       and "the receiver composed its own identical message" indistinguishable. Give each side a distinct
       ID so the receiver's buffer proves which one it holds. Set after psemu_load_state, which would
       otherwise overwrite it. */
    b = make_instance(bios, bios_size, app, app_size, state, state_size, "B");
    psemu_set_hardware_id(a, 0xAA1111AAu);
    psemu_set_hardware_id(b, 0xBB2222BBu);
    printf("hardware ids: A=0x%08X B=0x%08X\n", psemu_get_hardware_id(a), psemu_get_hardware_id(b));

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
            /* The trace flag is global. This toggles it around each instance's own psemu_run.
               That is what attributes each logged register access to the instance that made it. */
            psemu_ir_trace_enabled = (trace & 1) != 0;
            psemu_run(a, slice);
            psemu_ir_trace_enabled = (trace & 2) != 0;
            psemu_run(b, slice);
            psemu_ir_trace_enabled = 0;
            if (a->ir.data != last_a_data) {
                if (tx_n < TX_MAX) {
                    int t;
                    tx_ev[tx_n].at_ref = a->ir.clock_cycles;
                    tx_ev[tx_n].frame = f;
                    for (t = 0; t < 3; t++) {
                        tx_ev[tx_n].period[t] = a->timer.timers[t].period;
                        tx_ev[tx_n].count[t] = a->timer.timers[t].count;
                        tx_ev[tx_n].control[t] = a->timer.timers[t].control;
                    }
                    tx_n++;
                    /* Dump A's recent PC trace right after a mid-burst transmit write. Counting the
                       instructions back to the exception vector gives the instruction count for the
                       expiry-to-LED-write span, which is what turns a cycle shortfall into a
                       cycles-per-instruction figure. */
                    if (tx_n == 8) {
                        FILE *tr = fopen("ir_probe_A_tx_trace.log", "w");
                        if (tr) {
                            psemu_write_crash_report(a, tr);
                            fclose(tr);
                        }
                    }
                }
                last_a_data = a->ir.data;
            }
            /* The app's own handler ignores any edge whose live level does not match its expected_level
               (+0x26): it reads the level back out of INTC STATUS bit 12 and returns immediately on a
               mismatch (see the handler entry at 0x02005244). Its measured interval is therefore between
               consecutive *qualifying* edges, which is not the same thing as between consecutive level
               changes. Replicating that filter here reproduces the number the app actually tests, instead
               of a reconstruction that happens to be measuring something else. */
            if (b->ir.rx_level != last_rx_level) {
                uint32_t expected = psemu_bus_read8(&b->bus, 0x000003C4u + 0x26u);
                uint32_t st = psemu_bus_read8(&b->bus, 0x000003C4u + 0x28u);
                static int edge_log = 0;
                if (edge_log < 30) {
                    printf("  [edge %2d] f=%ld level=%d expected=%u state=%u t2=%u\n", edge_log, f,
                        b->ir.rx_level, expected, st, b->timer.timers[2].count);
                    edge_log++;
                }
                if ((uint32_t)b->ir.rx_level == expected) {
                    uint32_t t2q = b->timer.timers[2].count;
                    if (have_prev_qual_t2 && qual_n < MEAS_MAX) {
                        qual_frame[qual_n] = f;
                        qual_state[qual_n] = st;
                        qual[qual_n++] = (prev_qual_t2 - t2q) & 0xFFFFu;
                    }
                    prev_qual_t2 = t2q;
                    have_prev_qual_t2 = 1;
                }
            }
            if (b->ir.rx_level != last_rx_level) {
                uint32_t t2 = b->timer.timers[2].count;
                if (have_prev_t2 && meas_n < MEAS_MAX) {
                    /* Timer2 counts down and reloads at 0xFFFF, so an unsigned 16-bit difference handles the
                       wrap the same way the handler's own `lsl/lsr #16` truncation does. */
                    meas_frame[meas_n] = f;
                    meas[meas_n++] = (prev_t2 - t2) & 0xFFFFu;
                    /* Dump B's PC trace a few edges after sync, so it lands inside the receive-side
                       decode logic actually processing that edge, not idling afterward. */
                    if (meas_n == 5) {
                        FILE *tr = fopen("ir_probe_B_decode_trace.log", "w");
                        if (tr) {
                            psemu_write_crash_report(b, tr);
                            fclose(tr);
                        }
                    }
                }
                prev_t2 = t2;
                have_prev_t2 = 1;
                last_rx_level = b->ir.rx_level;
            }
            {
                uint32_t state = psemu_bus_read8(&b->bus, 0x000003C4u + 0x28u);
                uint32_t bit_index = psemu_bus_read32(&b->bus, 0x000003C4u + 0xCu);
                if (state > max_b_state) {
                    max_b_state = state;
                }
                if (state != last_b_state || bit_index != last_b_bitindex) {
                    state_changes++;
                    if (state_changes <= 500) {
                        printf("  [state #%d] f=%ld pc=0x%08X state=%u bit_index=%u\n", state_changes, f,
                            b->cpu.r[15], state, bit_index);
                    }
                    last_b_state = state;
                    last_b_bitindex = bit_index;
                }
            }
            /* Snapshot each side's live data buffer at the moment its own bit_index peaks. That is the only
               instant a fully-assembled message exists: the app reuses and clears these buffers afterwards,
               so an end-of-run read finds zeroes and proves nothing either way. The buffer address must be
               resolved through the pointer table (field+0x14 is the table, field+0x27 the index). */
            {
                int side;
                for (side = 0; side < 2; side++) {
                    psemu_t *ps = side ? b : a;
                    uint32_t sb = 0x000003C4u;
                    uint32_t table = psemu_bus_read32(&ps->bus, sb + 0x14u);
                    uint32_t idx = psemu_bus_read8(&ps->bus, sb + 0x27u);
                    uint32_t bidx = psemu_bus_read32(&ps->bus, sb + 0xCu);
                    uint32_t bcount = psemu_bus_read32(&ps->bus, sb + 0x8u);
                    uint32_t buf;
                    uint32_t nbytes;
                    uint32_t k;
                    if (table == 0u || idx == 0u || bcount == 0u || bcount > 8u * SNAP_MAX) {
                        continue;
                    }
                    buf = psemu_bus_read32(&ps->bus, table + (idx - 1u) * 4u);
                    nbytes = (bcount + 7u) / 8u;
                    /* These fields are updated non-atomically by the app, so a poll can land mid-update and
                       read a half-written table entry. Sampling A once produced a "buffer" at 0x000000FF,
                       inside the BIOS callback slots. Every real buffer seen sits in low WRAM, so reject
                       anything that could not be one rather than snapshotting nonsense. */
                    if (buf < 0x100u || buf + nbytes > 0x800u) {
                        continue;
                    }
                    /* The sender holds its message from before the first bit goes out until it clears the
                       buffer afterwards; the receiver's exists only once the last bit lands. Snapshotting
                       both sides at "peak bit_index" therefore caught the sender just after it had zeroed
                       the buffer. Capture the first-bit state and the last-bit state separately, and
                       compare the sender's first against the receiver's last. */
                    if (bidx >= 1u && !(side ? b_have_first : a_have_first)) {
                        for (k = 0; k < nbytes; k++) {
                            (side ? b_first : a_first)[k] = psemu_bus_read8(&ps->bus, buf + k);
                        }
                        if (side) {
                            b_have_first = 1;
                            b_first_len = (int)nbytes;
                            b_first_frame = f;
                        } else {
                            a_have_first = 1;
                            a_first_len = (int)nbytes;
                            a_first_frame = f;
                        }
                    }
                    if (bidx <= (side ? b_peak_bits : a_peak_bits)) {
                        continue;
                    }
                    for (k = 0; k < nbytes; k++) {
                        (side ? b_snap : a_snap)[k] = psemu_bus_read8(&ps->bus, buf + k);
                    }
                    if (side) {
                        b_peak_bits = bidx;
                        b_snap_len = (int)nbytes;
                        b_snap_frame = f;
                        b_snap_addr = buf;
                        b_snap_idx = idx;
                    } else {
                        a_peak_bits = bidx;
                        a_snap_len = (int)nbytes;
                        a_snap_frame = f;
                        a_snap_addr = buf;
                        a_snap_idx = idx;
                    }
                }
            }
            total_a_to_b += relay_edges(a, b, "A->B", trace, playout_delay);
            total_b_to_a += relay_edges(b, a, "B->A", trace, playout_delay);
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
        /* The end-of-run intc.enable is read after the receiver has already given up and gone to standby,
           which says nothing about what it was listening to while a transfer was actually in flight. */
        {
            static uint32_t last_a_en = 0xFFFFFFFFu, last_b_en = 0xFFFFFFFFu;
            if (a->intc.enable != last_a_en || b->intc.enable != last_b_en) {
                printf("frame %ld: intc.enable A=0x%08X (IRDA=%s T2=%s) B=0x%08X (IRDA=%s T2=%s)\n", f,
                    a->intc.enable, (a->intc.enable & INT_IRDA) ? "on" : "off",
                    (a->intc.enable & INT_TIMER2) ? "on" : "off", b->intc.enable,
                    (b->intc.enable & INT_IRDA) ? "on" : "off", (b->intc.enable & INT_TIMER2) ? "on" : "off");
                last_a_en = a->intc.enable;
                last_b_en = b->intc.enable;
            }
        }
        /* Both instances are meant to be stepped with identical cycle budgets so their IR clocks track each
           other, but this is the first place that checks it frame-by-frame instead of only at the very end. */
        {
            long long skew = (long long)a->ir.clock_cycles - (long long)b->ir.clock_cycles;
            static long long last_skew = 0;
            static uint32_t last_a_hz = 0, last_b_hz = 0;
            uint32_t a_hz = clk_current_hz(&a->clk), b_hz = clk_current_hz(&b->clk);
            if (f == 0 || (skew > last_skew + 5000) || (skew < last_skew - 5000) || a_hz != last_a_hz ||
                b_hz != last_b_hz) {
                printf("frame %ld: ir clock skew (A-B) = %lld (A=%llu B=%llu) A_hz=%u B_hz=%u\n", f, skew,
                    (unsigned long long)a->ir.clock_cycles, (unsigned long long)b->ir.clock_cycles, a_hz, b_hz);
                last_skew = skew;
                last_a_hz = a_hz;
                last_b_hz = b_hz;
            }
        }
    }

    printf("\nedges relayed: A->B %ld, B->A %ld\n", total_a_to_b, total_b_to_a);
    printf("B receive state machine: max state reached = %u (state changes: %d)\n", max_b_state, state_changes);
    /* The decisive check, and the only one here that cannot be faked by coincidence.

       Comparing the two sides' message buffers byte-for-byte proved unreliable three separate times: both
       instances run the same app from the same save file, so their buffers start identical, and two all-zero
       or two identically-stale buffers "match" without a single edge having been relayed. The buffers are
       also cleared and reused right after a transfer, so when they are sampled matters as much as what they
       hold.

       Giving each instance a distinct hardware id removes the ambiguity. A real IR message carries the
       sender's id, so finding one side's id in the other side's RAM can only happen if data actually
       crossed the link. Neither instance can derive the other's id by any other route. */
    {
        uint32_t id_a = psemu_get_hardware_id(a);
        uint32_t id_b = psemu_get_hardware_id(b);
        uint32_t addr;
        int b_has_a = 0, a_has_b = 0;
        for (addr = 0x300u; addr + 4u <= 0x800u; addr++) {
            uint32_t at_b = (uint32_t)psemu_bus_read8(&b->bus, addr) |
                            ((uint32_t)psemu_bus_read8(&b->bus, addr + 1u) << 8) |
                            ((uint32_t)psemu_bus_read8(&b->bus, addr + 2u) << 16) |
                            ((uint32_t)psemu_bus_read8(&b->bus, addr + 3u) << 24);
            uint32_t at_a = (uint32_t)psemu_bus_read8(&a->bus, addr) |
                            ((uint32_t)psemu_bus_read8(&a->bus, addr + 1u) << 8) |
                            ((uint32_t)psemu_bus_read8(&a->bus, addr + 2u) << 16) |
                            ((uint32_t)psemu_bus_read8(&a->bus, addr + 3u) << 24);
            if (at_b == id_a && !b_has_a) {
                printf("B holds A's id 0x%08X at 0x%08X\n", id_a, addr);
                b_has_a = 1;
            }
            if (at_a == id_b && !a_has_b) {
                printf("A holds B's id 0x%08X at 0x%08X\n", id_b, addr);
                a_has_b = 1;
            }
        }
        printf("\nIR TRANSFER RESULT: A->B %s, B->A %s\n", b_has_a ? "VERIFIED" : "not seen",
            a_has_b ? "VERIFIED" : "not seen");
        if (b_has_a && a_has_b) {
            printf("  Both sides decoded the other's hardware id: a full bidirectional exchange completed.\n");
        } else if (!b_has_a && !a_has_b) {
            printf("  Neither side holds the other's id: no message content crossed the link.\n");
        }
    }
    /* The window both message buffers live in. Printed unconditionally because it is the quickest way to
       see whether anything was written at all: with no transfer this range stays entirely zero. */
    {
        int j;
        printf("\nlow-RAM window 0x340-0x36F at end of run:\n  A:");
        for (j = 0; j < 48; j++) {
            printf(" %02X", psemu_bus_read8(&a->bus, 0x340u + (uint32_t)j));
        }
        printf("\n  B:");
        for (j = 0; j < 48; j++) {
            printf(" %02X", psemu_bus_read8(&b->bus, 0x340u + (uint32_t)j));
        }
        printf("\n");
    }
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
            for (addr = 0x02000000u; addr < 0x0200E800u; addr++) {
                uint8_t byte = psemu_bus_read8(&b->bus, addr);
                fwrite(&byte, 1, 1, dump);
            }
            fclose(dump);
            printf("wrote ir_code_dump.bin (0x02004000-0x0200E800)\n");
        }
    }
    {
        int k, t;
        printf("\nA transmit-side timer state at each IRDA_DATA change (%d total, first/last 10 shown):\n", tx_n);
        printf("  %-6s %-10s %-8s   %-22s %-22s %-22s\n", "frame", "t(ref)", "d(ref)", "timer0 per/cnt/ctl",
            "timer1 per/cnt/ctl", "timer2 per/cnt/ctl");
        for (k = 0; k < tx_n; k++) {
            if (k == 10 && tx_n > 20) {
                printf("  ...\n");
                k = tx_n - 10;
            }
            printf("  %-6ld %-10llu %-8lld", tx_ev[k].frame, (unsigned long long)tx_ev[k].at_ref,
                k ? (long long)(tx_ev[k].at_ref - tx_ev[k - 1].at_ref) : 0);
            for (t = 0; t < 3; t++) {
                printf("   %6u/%6u/%X", tx_ev[k].period[t], tx_ev[k].count[t], tx_ev[k].control[t]);
            }
            printf("\n");
        }
    }

    printf("\nA clk=%u Hz  B clk=%u Hz  (PSEMU_ASSUMED_CPU_HZ reference = %u)\n", clk_current_hz(&a->clk),
        clk_current_hz(&b->clk), (unsigned)PSEMU_ASSUMED_CPU_HZ);
    {
        uint32_t unit = psemu_bus_read32(&b->bus, 0x000003C4u + 0x20u) & 0xFFFFu;
        int k;
        printf("\nTimer2 tick deltas B actually measures between line-level changes (handler wants %u +- %u):\n",
            4u * unit, unit / 2u);
        for (k = 0; k < meas_n && k < 8; k++) {
            printf("  [%3d] f=%-4ld %6u%s\n", k, meas_frame[k], meas[k],
                (meas[k] + unit / 2u >= 4u * unit && meas[k] <= 4u * unit + unit / 2u) ? "  <-- in window" : "");
        }
        /* This second list is the one that matters: it is what the app's own handler measures, after its
           expected-level filter. The sync test it applies at state 2 is |delta - 4*unit| <= unit/2. */
        printf("\nSame, filtered by the app's own expected-level rule (state 2 wants %u +- %u):\n", 4u * unit,
            unit / 2u);
        for (k = 0; k < qual_n && k < 24; k++) {
            printf("  [%3d] f=%-4ld state=%u %6u%s\n", k, qual_frame[k], qual_state[k], qual[k],
                (qual[k] + unit / 2u >= 4u * unit && qual[k] <= 4u * unit + unit / 2u) ? "  <-- in sync window"
                                                                                      : "");
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
    /* Direct proof of transfer success or failure, independent of interpreting the state machine's own
       state numbers: field+0x14 is the data-buffer pointer both the transmit and receive state machines
       use (see docs/hardware-notes.md's disassembly notes). If a transfer actually decoded correctly, B's
       buffer should now hold the same bytes A's own buffer does. */
    {
        uint32_t sb = 0x000003C4u;
        /* field+0x14 is NOT the data buffer. It points at an array of buffer pointers, indexed by the
           counter at field+0x27: the receive bit-store reads its target as table[(field+0x27) - 1] (see
           0x020054BC in the disassembly). Comparing the table bytes directly, as this once did, compares
           pointers and unrelated neighbouring state rather than any message content. */
        uint32_t a_table = psemu_bus_read32(&a->bus, sb + 0x14u);
        uint32_t b_table = psemu_bus_read32(&b->bus, sb + 0x14u);
        uint32_t a_idx = psemu_bus_read8(&a->bus, sb + 0x27u);
        uint32_t b_idx = psemu_bus_read8(&b->bus, sb + 0x27u);
        uint32_t a_buf = psemu_bus_read32(&a->bus, a_table);
        uint32_t b_buf = psemu_bus_read32(&b->bus, b_table);
        printf("A buffer table @0x%08X idx=%u -> first buffer 0x%08X\n", a_table, a_idx, a_buf);
        printf("B buffer table @0x%08X idx=%u -> first buffer 0x%08X\n", b_table, b_idx, b_buf);
        uint32_t bit_index_a = psemu_bus_read32(&a->bus, sb + 0xCu);
        uint32_t bit_index_b = psemu_bus_read32(&b->bus, sb + 0xCu);
        /* field+0x8: the total-bit-count limit the transmit handler compares its own bit-index against
           (see 0x2005168-0x2005170 in the disassembled handler). Bytes at or past bit_count/8 are outside
           the real message entirely - never written by any repeat, so a mismatch there is stale memory,
           not a decode error. */
        uint32_t bit_count_a = psemu_bus_read32(&a->bus, sb + 0x8u);
        uint32_t bit_count_b = psemu_bus_read32(&b->bus, sb + 0x8u);
        int i, mismatches = 0, matches = 0;
        printf("A data buffer @0x%08X (bit_index=%u, bit_count=%u -> %u bytes), B data buffer @0x%08X (bit_index=%u, "
               "bit_count=%u -> %u bytes):\n",
            a_buf, bit_index_a, bit_count_a, (bit_count_a + 7u) / 8u, b_buf, bit_index_b, bit_count_b,
            (bit_count_b + 7u) / 8u);
        for (i = 0; i < (int)((bit_count_a + 7u) / 8u); i++) {
            uint8_t av = psemu_bus_read8(&a->bus, a_buf + (uint32_t)i);
            uint8_t bv = psemu_bus_read8(&b->bus, b_buf + (uint32_t)i);
            if (av != bv) {
                mismatches++;
            } else {
                matches++;
            }
            printf("  [%2d] A=0x%02X B=0x%02X%s\n", i, av, bv, av != bv ? "  <-- MISMATCH" : "");
        }
        /* Byte-exact equality is a coincidence past the real message: B's buffer is not necessarily
           zeroed the same way A's trailing memory happens to be. Report counts, not a blunt pass/fail -
           the mismatch position within the message matters more than the total. */
        printf("%d of %d bytes matched, %d differed (position matters more than the count here)\n", matches, matches + mismatches,
            mismatches);
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
