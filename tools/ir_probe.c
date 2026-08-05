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

   Verdict, printed near the end of a run: the app-independent link analysis (see analyze_direction). For
   each direction it recovers the pulse encoding from the relayed edges alone, reports how cleanly the
   waveform quantizes into two symbols, decodes the bits, and looks for the result verbatim in the
   receiver's RAM. It needs no knowledge of the app on either side, and it distinguishes the two failures
   that matter: a distorted waveform shows up as a collapse in clean-quantization, and a clean waveform the
   receiver never decoded shows up as no decoded content.

   Two earlier verdicts are kept, demoted, because each is right only in a narrower case than it looked:

   - The F_SN cross-check (each instance gets a distinct hardware id, and finding one side's id in the
     other's RAM proves data crossed) is decisive when the protocol carries F_SN, and silent otherwise.
     The serial-carrying app carries it; the card-data app sends card data with no serial number, and this
     check reported "not seen" for a transfer that had demonstrably succeeded. It is supplementary now.
   - Everything read out of a fixed RAM address - the receive state machine, the sync-window pulse table,
     the message-buffer window - belongs to one app's build and reports unrelated bytes for any other. All
     of it is off unless IR_PROBE_STATE_BLOCK names an address. See app_state_block.

   Byte-for-byte buffer comparison was tried first and misled three separate times, which is worth recording
   so it is not tried again: both instances run the same app from the same save file, so their message
   buffers start identical and "match" before a single edge has been relayed; the buffers are cleared and
   reused immediately after a transfer, so an end-of-run read finds only zeroes; and the field once assumed
   to be the buffer pointer (state block +0x14) is really a pointer to a table of buffer pointers, so
   comparing it compared pointers and neighbouring state. The RAM search in analyze_direction avoids the
   first two by searching for content the link itself carried, and its threshold is calibrated against a
   measured control run rather than assumed - see DECODE_VERIFY_RUN.

   Slice size trades relay precision against timing accuracy, and both directions have bitten this tool
   before. psemu_run's budget is in reference-rate cycles converted to real seconds, but its loop always runs
   at least one instruction. When one instruction's real duration exceeds the whole slice budget, that call
   overshoots. A slow app-selected CLK_MODE makes an instruction expensive in real time (at ~254KHz one
   instruction is ~11.8us, against a 3.79us budget at slice_cycles=4), so a finely sliced instance running
   slowly over-advances its own clock badly, while a fast one does not. Two instances at different CLK_MODEs
   therefore appear to drift apart. Measured: ~957000 reference cycles of apparent skew at slice 4, ~78600 at
   64, ~20700 at 256, ~8200 at 1024, with total elapsed time exactly correct only at the coarse end. Use a
   coarse slice when absolute timing matters, and a fine one only when relay precision matters more.

   usage: ir_probe <bios.bin> <app.mcs> <quicksaveA[,quicksaveB]> [slice_cycles] [frames] [scriptA] [scriptB]
                   [trace]
     quicksave     one state, loaded into both instances, when the app reaches its send/receive choice from a
                   single shared screen (the serial-carrying app). Two comma-separated states instead, when each role
                   has to be armed separately and no button script can reach both from one state (the card-data app
                   Forbidden Memories parks the sender and the receiver on different screens).
     slice_cycles  emulated cycles to run each instance before exchanging edges. 33000 = one frontend frame
                   (the frontend's real behavior). Smaller = finer interleaving, but see the note above on
                   what a small value costs in timing accuracy. Default 33000.
     frames        total emulated frames to run. Default 400 (~12s of emulated time).
     scriptA/B     per-instance button script: comma-separated "button@start-end" entries, frame-numbered.
                   Buttons: up, down, left, right, fire. Example: "up@20-30,fire@40-50".
                   "-" means no input. Default "up@20-30" for A and "down@20-30" for B.
     trace         "trace" logs every IR register access on both instances with its PC; "traceA"/"traceB"
                   restrict that log to one instance, which is what makes the two sides' roles separable.
                   "flashwatch" instead reports every attempted flash write, which is how this tool answers
                   whether an app commits received data to the PS1 save on its card (see FLASH_WATCH_SAMPLES).

   Environment:
     IR_PROBE_PLAYOUT_DELAY  override the relay's playout delay, in reference cycles.
     IR_PROBE_STATE_BLOCK    RAM address of the running app's IR state block, enabling the app-specific
                             readouts that depend on one. Off by default; see app_state_block.
     IR_PROBE_WATCH_PC       comma-separated app addresses; reports whether either side's execution ever
                             reaches each one over the whole run. See watch_pcs.

   Output: both sides' final screens, the per-block card diff against flash as it stood at setup, and -
   for any side whose card changed - that side's whole card written to ir_probe_A_card.mcr /
   ir_probe_B_card.mcr, in the same raw layout a .mcr already uses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

#define QUICKSAVE_HEADER_SIZE 16 /* magic[4] + version + app_size + app_hash; see frontends/desktop/main.c */
#define FRAME_CYCLES 33000u

/* "flashwatch" mode: every attempted write to FLASH1 or FLASH2, with the PC that issued it.
   This exists because the end-of-run card-block diff answers a narrower question than it appears to. A diff
   reports nothing in three different situations: the app never wrote, the app wrote a value identical to
   what was already stored, or something between the app and storage discarded the write. Only the third is
   a bug, and the hook separates them by reporting the attempt itself.
   Deliberately counts rather than logs by default: a real app rewriting a save block issues thousands of
   byte writes, and a per-write log buries the one fact worth having. The first few are printed with their
   PC, which is enough to find the routine responsible in a disassembly. */
#define FLASH_WATCH_SAMPLES 8u

/* Base address of the running app's own IR state block in RAM, or 0 to skip every readout that depends on
   one. Zero by default, on purpose: this address is a property of one specific app's build, recovered from
   the literal its INT_IRDA handler loads, and pointing it at a different app reports whatever unrelated
   bytes happen to live there. That is not a harmless nuisance - a card-data-app run once printed "max state
   reached = 17" from an address of the serial-carrying app, which reads as a working receive state machine and is pure
   noise. The default verdict is analyze_direction, which needs no such address.
   Set IR_PROBE_STATE_BLOCK=0x3C4 to re-enable the readouts of the serial-carrying app (its state block, the sync-window
   pulse-quality table, and the low-RAM message window), which is where this project's transmit-timing
   measurements came from. */
static uint32_t app_state_block = 0;

static struct {
    int enabled;
    unsigned long flash1_writes;
    unsigned long flash2_writes;
    unsigned long ctrl_writes; /* FLASH_CTRL: bank select, and any command-style programming interface */
    unsigned long samples_printed;
    const char *label;
} flash_watch;

static void flash_write_watch(uint32_t addr, uint8_t value, uint32_t pc) {
    int is_flash1 = addr >= PSEMU_FLASH1_BASE && addr < PSEMU_FLASH1_BASE + PSEMU_FLASH_SIZE;
    int is_flash2 = addr >= PSEMU_FLASH2_BASE && addr < PSEMU_FLASH2_BASE + PSEMU_FLASH_SIZE;
    /* FLASH_CTRL is watched alongside the two data windows on purpose. Storage-class flash is usually
       programmed through a command interface rather than by storing to the data address directly, so an app
       that commits data could in principle show up here and nowhere else. Counting it separately keeps
       "the app is programming flash" distinct from the bank-select writes the kernel makes at dispatch. */
    int is_ctrl = addr >= PSEMU_FLASH_CTRL_BASE && addr < PSEMU_FLASH_CTRL_BASE + FLASH_CTRL_SPAN;
    const char *region;
    uint32_t offset;
    if (!is_flash1 && !is_flash2 && !is_ctrl) {
        return;
    }
    if (is_flash1) {
        flash_watch.flash1_writes++;
        region = "FLASH1";
        offset = addr - PSEMU_FLASH1_BASE;
    } else if (is_flash2) {
        flash_watch.flash2_writes++;
        region = "FLASH2";
        offset = addr - PSEMU_FLASH2_BASE;
    } else {
        flash_watch.ctrl_writes++;
        region = "FLASH_CTRL";
        offset = addr - PSEMU_FLASH_CTRL_BASE;
    }
    if (flash_watch.samples_printed < FLASH_WATCH_SAMPLES) {
        printf("  [flashwatch %s] %s +0x%05X", flash_watch.label, region, (unsigned)offset);
        if (!is_ctrl) {
            printf(" (block %u)", (unsigned)(offset / 0x2000u));
        }
        printf(" = 0x%02X from pc=0x%08X\n", value, pc);
        flash_watch.samples_printed++;
    }
}

/* "watch" list: does execution ever reach a given app address, on either side, over the whole run?
   The flash-write hook above answers what an app wrote. This answers the question that comes first, and
   that a write hook cannot: whether the app even called the routine that would have written. The card-data app's
   PS1-save writer is a syscall wrapper behind a gate (docs/app-notes.md), so "no write attempt" has two
   very different causes - the gate refused, or nothing ever called the wrapper - and only one of them is
   about this emulator.
   Set IR_PROBE_WATCH_PC to a comma-separated list of addresses (0x... accepted). Every hit is counted per
   instance, with the frame of the first one. */
#define WATCH_MAX 24
static struct {
    uint32_t pc;
    unsigned long hits[2]; /* [0] = A, [1] = B */
    long first_frame[2];
} watch_pcs[WATCH_MAX];
static int watch_n;
static int watch_side;    /* which instance psemu_run is currently inside: 0 = A, 1 = B */
static long watch_frame;

static void exec_watch(uint32_t pc, uint32_t cpsr) {
    int i;
    (void)cpsr;
    for (i = 0; i < watch_n; i++) {
        if (watch_pcs[i].pc == pc) {
            if (watch_pcs[i].hits[watch_side]++ == 0) {
                watch_pcs[i].first_frame[watch_side] = watch_frame;
                printf("  [watch %s] pc=0x%08X first reached at frame %ld\n", watch_side ? "B" : "A", pc,
                    watch_frame);
            }
            return;
        }
    }
}

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

/* Every edge that actually crossed the link, in the receiver's own timeline, kept so the run can be judged
   without knowing anything about the app that produced it. See analyze_direction. */
#define LINK_EDGE_MAX 32768u
typedef struct link_capture {
    uint64_t t[LINK_EDGE_MAX];
    uint8_t level[LINK_EDGE_MAX];
    uint32_t n;
    uint32_t dropped; /* edges past LINK_EDGE_MAX: capture limit only, the link still relayed them */
} link_capture_t;

static link_capture_t cap_a_to_b, cap_b_to_a;

static void capture_edge(link_capture_t *cap, uint64_t t, int level) {
    if (cap->n >= LINK_EDGE_MAX) {
        cap->dropped++;
        return;
    }
    cap->t[cap->n] = t;
    cap->level[cap->n] = (uint8_t)level;
    cap->n++;
}

/* Judges one direction of the link without knowing anything about the app running on either side.

   This replaces a verdict that only ever worked for one app. The old check scanned the receiver's RAM for
   the sender's F_SN, which is decisive when the protocol carries F_SN (the serial-carrying app) and silent when it
   does not (the card-data app carries card data and no serial number, so the check reported "not
   seen" for a transfer that had in fact succeeded). Reading a fixed RAM address for a "receive state
   machine" was worse: that address belongs to one app's state block, and for any other app it reports
   whatever unrelated bytes happen to live there.

   Two things here are app-independent:

   1. Signal quality. Whatever an app's encoding, a bit-banged IR protocol built on pulse timing spaces its
      pulses at small integer multiples of one unit. Taking the modal rise-to-rise interval as that unit and
      measuring how cleanly every other interval lands on a multiple of it says whether the waveform
      survived the relay, with no protocol knowledge at all. This is the metric that exposes distortion: the
      falling-edge stretch bug turned a clean 205/406 two-symbol code into gaps of 5 and 206 with 272 of
      them collapsed to zero, and it shows up here as a collapse in the clean-quantization rate.

   2. Content delivery. If the interval multiples are dominated by two symbols, the stream is almost
      certainly a two-symbol code, and the bits can be recovered without knowing which symbol means which -
      so try both polarities and both bit orders, and look for the resulting bytes verbatim in the
      receiver's RAM. A receiver that decoded the message has to have put the bytes somewhere, and no
      instance can produce the other's payload by any route except the link. That is the same argument the
      F_SN check rested on, generalized off any particular field. */
/* How long a verbatim run has to be before it counts as proof rather than coincidence.

   Chance matches are real and were measured, not guessed at. Both instances run the same app from the same
   card image, so the receiver's RAM already holds data shaped like the sender's, and a repetitive payload
   matches some of it for free. A control run - sender transmitting normally, receiver never armed, so every
   relayed edge is dropped and nothing is decoded - still found an 11-byte run. The same scenario with the
   receiver armed found 172 bytes. The threshold sits well clear of the measured noise floor, and the best
   run is always printed so a borderline result can be judged rather than trusted. */
#define DECODE_MIN_RUN 8u  /* worth printing */
#define DECODE_VERIFY_RUN 32u /* worth believing */

static uint32_t longest_run_in_ram(const psemu_t *rx, const uint8_t *bytes, uint32_t len) {
    uint8_t ram[PSEMU_RAM_SIZE];
    uint32_t i, best = 0;
    for (i = 0; i < PSEMU_RAM_SIZE; i++) {
        ram[i] = psemu_bus_read8(&((psemu_t *)rx)->bus, i);
    }
    for (i = 0; i < PSEMU_RAM_SIZE; i++) {
        uint32_t j;
        for (j = 0; j < len; j++) {
            uint32_t k = 0;
            while (i + k < PSEMU_RAM_SIZE && j + k < len && ram[i + k] == bytes[j + k]) {
                k++;
            }
            if (k > best) {
                best = k;
            }
            if (best >= len) {
                return best;
            }
        }
    }
    return best;
}

/* Splits `durations` into the two populations a two-symbol code produces, and reports how much of the data
   that explains. Used for both encoding families this project has seen (see analyze_direction).

   Deliberately clusters rather than quantizing against integer multiples of a unit, because real apps do not
   oblige. The two gap lengths of the card-data app two gap lengths are 205 and 406, near enough 1:2 that a
   multiple-of-a-unit test works. The two pulse widths of the serial-carrying app are 804 and 1407 - a ratio of 1.75, on a
   constant 404 gap - and a multiple test reads its short symbol as 0.5 units, rounds it to 1, and reports a
   single-symbol stream with nothing to decode. An external register reference describing these pulses as
   "long is usually twice as long as short" is approximate, and building the detector on that number made it
   work for one app and silently fail on the other. Two clusters and their measured separation carry no such
   assumption. */
typedef struct symbol_fit {
    uint32_t lo;   /* centre of the shorter symbol's cluster */
    uint32_t hi;   /* centre of the longer one, 0 if the data is single-symbol */
    uint32_t clean; /* durations belonging to either cluster */
    uint32_t total;
    uint32_t sym1; /* population of lo */
    uint32_t sym2; /* population of hi */
} symbol_fit_t;

/* Within an eighth counts as the same symbol: wide enough for the cycle-level jitter an emulated clock and
   a relay introduce, far narrower than the separation between two real symbols. */
#define SYMBOL_TOLERANCE(v) ((v) / 8u)

static uint32_t population_near(const uint64_t *d, uint32_t count, uint64_t centre) {
    uint32_t i, hits = 0;
    for (i = 0; i < count; i++) {
        if (d[i] >= centre - SYMBOL_TOLERANCE(centre) && d[i] <= centre + SYMBOL_TOLERANCE(centre)) {
            hits++;
        }
    }
    return hits;
}

static void fit_symbols(const uint64_t *durations, uint32_t count, uint8_t *class_out, symbol_fit_t *out) {
    uint32_t i, best = 0, second = 0, best_hits = 0, second_hits = 0;
    memset(out, 0, sizeof(*out));
    out->total = count;
    if (count == 0) {
        return;
    }
    for (i = 0; i < count; i++) {
        uint32_t hits;
        if (durations[i] == 0) {
            continue;
        }
        hits = population_near(durations, count, durations[i]);
        if (hits > best_hits) {
            best_hits = hits;
            best = (uint32_t)durations[i];
        }
    }
    /* The second cluster is the most populated value that is not part of the first. */
    for (i = 0; i < count; i++) {
        uint32_t hits;
        if (durations[i] == 0 || (durations[i] >= best - SYMBOL_TOLERANCE(best) &&
                                     durations[i] <= best + SYMBOL_TOLERANCE(best))) {
            continue;
        }
        hits = population_near(durations, count, durations[i]);
        if (hits > second_hits) {
            second_hits = hits;
            second = (uint32_t)durations[i];
        }
    }
    /* A lone outlier is not a symbol. Sync pulses and inter-burst gaps are each one or two samples. */
    if (second_hits * 16u < count) {
        second = 0;
        second_hits = 0;
    }
    out->lo = (second && second < best) ? second : best;
    out->hi = (second && second < best) ? best : second;
    out->sym1 = (second && second < best) ? second_hits : best_hits;
    out->sym2 = (second && second < best) ? best_hits : second_hits;
    out->clean = out->sym1 + out->sym2;
    for (i = 0; i < count; i++) {
        if (out->lo && durations[i] >= out->lo - SYMBOL_TOLERANCE(out->lo) &&
            durations[i] <= out->lo + SYMBOL_TOLERANCE(out->lo)) {
            class_out[i] = 1;
        } else if (out->hi && durations[i] >= out->hi - SYMBOL_TOLERANCE(out->hi) &&
                   durations[i] <= out->hi + SYMBOL_TOLERANCE(out->hi)) {
            class_out[i] = 2;
        } else {
            class_out[i] = 0; /* neither: an outlier, a sync pulse, or a gap between bursts */
        }
    }
}

static void analyze_direction(const char *name, const link_capture_t *cap, const psemu_t *rx) {
    static uint64_t gap[LINK_EDGE_MAX], width[LINK_EDGE_MAX];
    static uint8_t gap_class[LINK_EDGE_MAX], width_class[LINK_EDGE_MAX];
    static uint8_t decoded[LINK_EDGE_MAX / 8 + 4];
    symbol_fit_t gap_fit, width_fit, fit;
    const uint8_t *symbol;
    const char *scheme;
    uint32_t ngap = 0, nwidth = 0, i;
    uint64_t last_rise = 0;
    int have_rise = 0;

    if (cap->n == 0) {
        printf("  %s: no edges\n", name);
        return;
    }
    /* Two measurements, because the two real apps this project has traced encode along different axes.
       The card-data app varies the gap between fixed-width pulses (pulse-distance); the serial-carrying app
       varies the width of the pulses themselves (pulse-width, "long is about twice as long as short").
       Measuring only rise-to-rise intervals reads the first correctly and the second as a single-symbol
       stream with nothing to decode. Both are collected and whichever separates into two symbols more
       cleanly is the one reported, so neither encoding needs to be known in advance. */
    for (i = 0; i < cap->n; i++) {
        if (cap->level[i]) {
            if (have_rise) {
                gap[ngap++] = cap->t[i] - last_rise;
            }
            last_rise = cap->t[i];
            have_rise = 1;
        } else if (have_rise) {
            width[nwidth++] = cap->t[i] - last_rise;
        }
    }
    if (ngap < 2 || nwidth < 2) {
        printf("  %s: %u edges, too few pulses to analyze\n", name, cap->n);
        return;
    }
    fit_symbols(gap, ngap, gap_class, &gap_fit);
    fit_symbols(width, nwidth, width_class, &width_fit);
    /* The axis the app actually modulates splits into two populated clusters; the axis it holds constant
       yields one. Whichever recovers more two-symbol data is the real one. */
    if (width_fit.sym2 > gap_fit.sym2) {
        fit = width_fit;
        symbol = width_class;
        scheme = "pulse-width";
    } else {
        fit = gap_fit;
        symbol = gap_class;
        scheme = "pulse-distance";
    }
    if (fit.lo == 0) {
        printf("  %s: could not establish a pulse unit\n", name);
        return;
    }
    printf("  %s: %u edges, %u pulses, %s symbols %u/%u cycles (%.1f/%.1fus), %u/%u clean (%.1f%%)\n", name,
        cap->n, nwidth, scheme, fit.lo, fit.hi, (double)fit.lo / (PSEMU_ASSUMED_CPU_HZ / 1000000.0),
        (double)fit.hi / (PSEMU_ASSUMED_CPU_HZ / 1000000.0), fit.clean, fit.total,
        100.0 * (double)fit.clean / (double)fit.total);

    if (fit.clean < 16u || fit.sym2 == 0u) {
        printf("    not a two-symbol code on either axis (short %u, long %u); no bit decode attempted\n",
            fit.sym1, fit.sym2);
        return;
    }
    printf("    two-symbol code: %u short, %u long -> %u bits\n", fit.sym1, fit.sym2, fit.clean);
    {
        uint32_t intervals = fit.total;
        /* Neither the symbol-to-bit mapping nor the bit order is known without the app, so try all four and
           report the best. A wrong combination simply finds nothing. */
        int polarity, msb_first, best_len = 0;
        const char *best_desc = "";
        for (polarity = 0; polarity < 2; polarity++) {
            for (msb_first = 0; msb_first < 2; msb_first++) {
                uint32_t nbits = 0, nbytes = 0, run;
                uint8_t acc = 0;
                for (i = 0; i < intervals; i++) {
                    int bit;
                    if (symbol[i] == 0u) {
                        continue; /* outlier, sync pulse, or inter-burst gap: carries no bit */
                    }
                    bit = (symbol[i] == 2u) ? 1 : 0;
                    if (polarity) {
                        bit = !bit;
                    }
                    acc = msb_first ? (uint8_t)((acc << 1) | bit) : (uint8_t)((acc >> 1) | (bit << 7));
                    if (++nbits % 8u == 0u && nbytes < sizeof(decoded)) {
                        decoded[nbytes++] = acc;
                        acc = 0;
                    }
                }
                if (nbytes < DECODE_MIN_RUN) {
                    continue;
                }
                run = longest_run_in_ram(rx, decoded, nbytes);
                if ((int)run > best_len) {
                    best_len = (int)run;
                    best_desc = polarity ? (msb_first ? "inverted/MSB-first" : "inverted/LSB-first")
                                         : (msb_first ? "MSB-first" : "LSB-first");
                }
            }
        }
        if (best_len >= (int)DECODE_VERIFY_RUN) {
            printf("    CONTENT VERIFIED: %d decoded bytes (%s) found verbatim in the receiver's RAM\n", best_len,
                best_desc);
        } else if (best_len >= (int)DECODE_MIN_RUN) {
            printf("    inconclusive: longest decoded run in the receiver's RAM is %d bytes (%s), under the %u"
                   " needed to rule out coincidence\n",
                best_len, best_desc, DECODE_VERIFY_RUN);
        } else {
            printf("    no decoded content found in the receiver's RAM (best run %d bytes)\n", best_len);
        }
    }
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
/* `to_minus_from` converts a timestamp on `from`'s IR timeline onto `to`'s. It is zero when both instances
   start from the same save state, which is the single-screen case and what this tool did for its whole life
   before two-state runs existed.
   It is not zero when each role is armed from its own separately-captured state: those two states were saved
   at different points in emulated time, so their IR clocks are offset by however far apart the two capture
   moments were. Measured at 9306433 reference cycles (~8.8s) for the sender and receiver pair of the card-data app. Relaying
   raw across that offset puts every edge ~8.8s into the receiver's past, so ir_tick releases the entire
   message in one shot, the spacing that encodes each bit collapses, and the RX edge queue overruns.
   The offset is latched once at startup and held for the whole run, never resampled. Both instances get
   identical per-frame cycle budgets here, so the offset stays constant, and holding it keeps every edge's
   relative spacing exact. This mirrors ir_link.h's wall_minus_core_us and the reasoning recorded there. */
static int relay_edges(psemu_t *from, psemu_t *to, const char *direction, int verbose, uint64_t playout_delay,
    int64_t to_minus_from, link_capture_t *cap) {
    ir_edge_t edge;
    int relayed = 0;
    while (ir_pop_tx_edge(&from->ir, &edge)) {
        capture_edge(cap, (uint64_t)((int64_t)edge.timestamp_cycles + to_minus_from) + playout_delay, edge.level);
        /* A relay that runs every N cycles hands over edges up to N cycles after they were produced, with
           timestamps already in the receiver's past. The receiver then releases the whole batch at once and
           every interval it measures collapses, which is why a transfer that works at fine granularity fails
           at frame granularity. Adding a fixed playout delay to every edge restores the spacing: the receiver
           holds each edge until delay cycles after it was produced, so relative timing survives any relay
           latency up to that delay. This is the ordinary jitter-buffer trade of latency for correctness. */
        ir_push_rx_edge(
            &to->ir, (uint64_t)((int64_t)edge.timestamp_cycles + to_minus_from) + playout_delay, edge.level);
        relayed++;
        if (verbose) {
            printf("  [relay %s] t=%llu level=%d\n", direction, (unsigned long long)edge.timestamp_cycles,
                edge.level);
        }
    }
    return relayed;
}

int main(int argc, char **argv) {
    size_t bios_size = 0, app_size = 0, save_size = 0, save_b_size = 0;
    uint8_t *bios, *app, *save, *save_b = NULL;
    const uint8_t *state = NULL, *state_b = NULL;
    size_t state_size = 0, state_b_size = 0;
    psemu_t *a, *b;
    uint32_t slice_cycles = FRAME_CYCLES;
    long frames = 400;
    int trace = 0;
    long f;
    long total_a_to_b = 0, total_b_to_a = 0;
    int64_t b_minus_a = 0; /* see relay_edges */
    uint8_t *flash_baseline_a, *flash_baseline_b; /* see the card-block diff at the end of the run */
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
            "usage: %s <bios.bin> <app.mcs> <quicksaveA[,quicksaveB]> [slice_cycles] [frames] [scriptA] "
            "[scriptB] [trace]\n",
            argv[0]);
        return 1;
    }
    bios = read_file(argv[1], &bios_size);
    app = read_file(argv[2], &app_size);
    /* argv[3] is either one state for both instances, or "stateA,stateB" when each role has to be armed from
       its own screen. See the usage note at the top of this file. */
    {
        const char *comma = strchr(argv[3], ',');
        if (comma) {
            size_t a_len = (size_t)(comma - argv[3]);
            char *a_path = (char *)malloc(a_len + 1);
            if (!a_path) {
                fprintf(stderr, "out of memory\n");
                return 1;
            }
            memcpy(a_path, argv[3], a_len);
            a_path[a_len] = '\0';
            save = read_file(a_path, &save_size);
            save_b = read_file(comma + 1, &save_b_size);
            if (!save_b) {
                fprintf(stderr, "failed to read B's quicksave %s\n", comma + 1);
                return 1;
            }
            free(a_path);
        } else {
            save = read_file(argv[3], &save_size);
        }
    }
    if (!bios || !app || !save) {
        fprintf(stderr, "failed to read one of the input files\n");
        return 1;
    }
    if (save_size > QUICKSAVE_HEADER_SIZE) {
        state = save + QUICKSAVE_HEADER_SIZE;
        state_size = save_size - QUICKSAVE_HEADER_SIZE;
    }
    /* B falls back to A's state when only one was given, which is the single-screen case. */
    state_b = state;
    state_b_size = state_size;
    if (save_b && save_b_size > QUICKSAVE_HEADER_SIZE) {
        state_b = save_b + QUICKSAVE_HEADER_SIZE;
        state_b_size = save_b_size - QUICKSAVE_HEADER_SIZE;
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
        } else if (strcmp(argv[8], "flashwatch") == 0) {
            flash_watch.enabled = 1;
            psemu_bus_write_trace_cb = flash_write_watch;
        }
    }

    {
        const char *pd = getenv("IR_PROBE_PLAYOUT_DELAY");
        if (pd) {
            playout_delay = (uint64_t)strtoull(pd, NULL, 10);
            printf("playout delay: %llu reference cycles\n", (unsigned long long)playout_delay);
        }
    }
    {
        const char *w = getenv("IR_PROBE_WATCH_PC");
        if (w) {
            while (*w && watch_n < WATCH_MAX) {
                char *end;
                unsigned long v = strtoul(w, &end, 0);
                if (end == w) {
                    break;
                }
                watch_pcs[watch_n].pc = (uint32_t)v;
                watch_pcs[watch_n].first_frame[0] = -1;
                watch_pcs[watch_n].first_frame[1] = -1;
                watch_n++;
                w = (*end == ',') ? end + 1 : end;
            }
            if (watch_n > 0) {
                int i;
                psemu_exec_trace_cb = exec_watch;
                printf("watching %d pc(s):", watch_n);
                for (i = 0; i < watch_n; i++) {
                    printf(" 0x%08X", watch_pcs[i].pc);
                }
                printf("\n");
            }
        }
    }
    {
        /* See app_state_block: off unless a caller names an address, because the address belongs to one
           app's build and reports noise for any other. */
        const char *sb = getenv("IR_PROBE_STATE_BLOCK");
        if (sb) {
            app_state_block = (uint32_t)strtoul(sb, NULL, 0);
            printf("app IR state block: 0x%08X (app-specific readouts enabled)\n", app_state_block);
        }
    }
    a = make_instance(bios, bios_size, app, app_size, state, state_size, "A");
    /* Both instances otherwise run the same app from the same save with the same default hardware ID, and
       a real IR message carries that ID. Identical IDs make "the receiver decoded the sender's message"
       and "the receiver composed its own identical message" indistinguishable. Give each side a distinct
       ID so the receiver's buffer proves which one it holds. Set after psemu_load_state, which would
       otherwise overwrite it. */
    b = make_instance(bios, bios_size, app, app_size, state_b, state_b_size, "B");
    psemu_set_hardware_id(a, 0xAA1111AAu);
    psemu_set_hardware_id(b, 0xBB2222BBu);
    printf("hardware ids: A=0x%08X B=0x%08X\n", psemu_get_hardware_id(a), psemu_get_hardware_id(b));

    /* See relay_edges. Latched here, before a single cycle runs, so it measures only the gap between the two
       save states' capture moments and nothing either instance does afterwards. */
    b_minus_a = (int64_t)ir_get_clock_cycles(&b->ir) - (int64_t)ir_get_clock_cycles(&a->ir);
    if (b_minus_a != 0) {
        printf("relay clock offset (B-A): %lld reference cycles, latched from the two save states\n",
            (long long)b_minus_a);
    }

    /* Captured after psemu_load_state and psemu_set_hardware_id, so it is flash exactly as the first frame
       will see it. See the block-diff report at the end of the run for why the input file cannot serve. */
    flash_baseline_a = (uint8_t *)malloc(PSEMU_FLASH_SIZE);
    flash_baseline_b = (uint8_t *)malloc(PSEMU_FLASH_SIZE);
    if (!flash_baseline_a || !flash_baseline_b) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    memcpy(flash_baseline_a, a->flash.data, PSEMU_FLASH_SIZE);
    memcpy(flash_baseline_b, b->flash.data, PSEMU_FLASH_SIZE);

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
            flash_watch.label = "A";
            watch_side = 0;
            watch_frame = f;
            psemu_run(a, slice);
            psemu_ir_trace_enabled = (trace & 2) != 0;
            flash_watch.label = "B";
            watch_side = 1;
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
            if (app_state_block && b->ir.rx_level != last_rx_level) {
                uint32_t expected = psemu_bus_read8(&b->bus, app_state_block + 0x26u);
                uint32_t st = psemu_bus_read8(&b->bus, app_state_block + 0x28u);
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
            if (app_state_block) {
                uint32_t state = psemu_bus_read8(&b->bus, app_state_block + 0x28u);
                uint32_t bit_index = psemu_bus_read32(&b->bus, app_state_block + 0xCu);
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
            if (app_state_block) {
                int side;
                for (side = 0; side < 2; side++) {
                    psemu_t *ps = side ? b : a;
                    uint32_t sb = app_state_block;
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
            total_a_to_b += relay_edges(a, b, "A->B", trace, playout_delay, b_minus_a, &cap_a_to_b);
            total_b_to_a += relay_edges(b, a, "B->A", trace, playout_delay, -b_minus_a, &cap_b_to_a);
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
    printf("link analysis (app-independent):\n");
    analyze_direction("A->B", &cap_a_to_b, b);
    analyze_direction("B->A", &cap_b_to_a, a);
    if (app_state_block) {
        printf("B receive state machine: max state reached = %u (state changes: %d)\n", max_b_state, state_changes);
    }
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
        /* This test is supplementary. It is not the result. It gives positive evidence when it
           succeeds, and it gives no data when it fails: only a protocol that puts F_SN in its message
           can satisfy this test. The serial-carrying app does this. The card-data app sends card data
           with no serial number, and this test reported "not seen" for a transfer that was clearly
           successful. Read the app-independent link analysis above for the true result. */
        printf("\nF_SN cross-check (only meaningful if the app's protocol carries F_SN): A->B %s, B->A %s\n",
            b_has_a ? "id found" : "not seen", a_has_b ? "id found" : "not seen");
        if (b_has_a && a_has_b) {
            printf("  Both sides hold the other's hardware id: a full bidirectional exchange completed.\n");
        }
    }
    /* The window one specific app's message buffers live in, so it is gated with the rest of the
       app-specific readouts. For that app it is the quickest look at whether anything was written at all:
       with no transfer the range stays entirely zero. For any other app it is an arbitrary slice of RAM. */
    if (app_state_block) {
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
            printf("wrote ir_code_dump.bin (0x02000000-0x0200E800, FLASH1 as the CPU sees it)\n");
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

    if (watch_n > 0) {
        int i, side;
        printf("\nwatched pcs:\n");
        for (i = 0; i < watch_n; i++) {
            for (side = 0; side < 2; side++) {
                printf("  0x%08X %s: %lu hit(s)", watch_pcs[i].pc, side ? "B" : "A", watch_pcs[i].hits[side]);
                if (watch_pcs[i].hits[side]) {
                    printf(", first at frame %ld", watch_pcs[i].first_frame[side]);
                }
                printf("\n");
            }
        }
    }
    printf("\n");
    print_framebuffer(a, "A");
    print_framebuffer(b, "B");
    if (app_state_block) {
        uint32_t unit = psemu_bus_read32(&b->bus, app_state_block + 0x20u) & 0xFFFFu;
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
    if (app_state_block) {
        uint32_t sb = app_state_block;
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
    if (app_state_block) {
        uint32_t sb = app_state_block;
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

    /* Which physical card blocks each side actually rewrote during the run.
       A PocketStation app runs out of FLASH1, a banked window holding only its own blocks, but the whole
       card stays visible unwindowed through FLASH2 - including the PS1 save belonging to the console game.
       That is the only route between the two, so a transfer that is supposed to land in the game's save has
       to show up here as a write to a block the app does not own. Reported per block, so "the app rewrote
       its own state" and "the app rewrote the game's save" stay distinguishable.
       See docs/app-notes.md, "How an app reaches the PS1 save on the same card".

       The baseline is flash as it stood once the run was fully set up, captured in flash_baseline_a/b, not
       the input file. Those two are not the same thing and using the file gives nonsense for anything but a
       whole-card .mcr: psemu_load_mcs synthesizes a 16-frame directory and relocates the save's data to
       block 1, so a file-vs-flash diff reports that relocation as thousands of "written" bytes in every
       block before a single instruction runs. psemu_load_state then overwrites flash again with whatever
       the save state captured. Both happen before the first frame, and neither is a write by the app. */
    {
        int side;
        for (side = 0; side < 2; side++) {
            const psemu_t *ps = side ? b : a;
            const uint8_t *now = ps->flash.data;
            const uint8_t *was = side ? flash_baseline_b : flash_baseline_a;
            uint32_t block;
            int any = 0;
            printf("%s card blocks written: ", side ? "B" : "A");
            for (block = 0; block < PSEMU_FLASH_SIZE / 0x2000u; block++) {
                size_t base = (size_t)block * 0x2000u;
                size_t i;
                uint32_t changed = 0;
                for (i = 0; i < 0x2000u; i++) {
                    if (now[base + i] != was[base + i]) {
                        changed++;
                    }
                }
                if (changed) {
                    printf("%s%u (%u bytes)", any ? ", " : "", block, changed);
                    any = 1;
                }
            }
            printf("%s\n", any ? "" : "none");
            /* The run's real output: each side's card exactly as it stands afterwards, in the same raw
               128KB layout a .mcr already uses. A block count says a write landed; only the card itself
               shows what landed, and it can be diffed against the input card or loaded straight back into
               either frontend. */
            if (any) {
                char path[64];
                FILE *out;
                sprintf(path, "ir_probe_%s_card.mcr", side ? "B" : "A");
                out = fopen(path, "wb");
                if (out) {
                    fwrite(now, 1, PSEMU_FLASH_SIZE, out);
                    fclose(out);
                    printf("  wrote %s\n", path);
                }
            }
        }
        free(flash_baseline_a);
        free(flash_baseline_b);
    }
    if (flash_watch.enabled) {
        /* Attempted writes, whether or not they changed a stored byte. Compare against the block diff
           above: writes here with no blocks reported there means the app wrote values identical to what was
           already stored, or something dropped them. No writes here means the app never tried at all, and
           the write path is gated on something this scenario did not reach. */
        printf("flash writes attempted: FLASH1 %lu, FLASH2 %lu, FLASH_CTRL %lu\n", flash_watch.flash1_writes,
            flash_watch.flash2_writes, flash_watch.ctrl_writes);
    }

    psemu_destroy(a);
    psemu_destroy(b);
    free(bios);
    free(app);
    free(save);
    free(save_b);
    return 0;
}
