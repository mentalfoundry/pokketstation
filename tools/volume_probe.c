/* Phase 1 of the system-volume investigation (see docs/hardware-notes.md).

   The PocketStation has no hardware volume register: the DAC exposes only
   an enable bit and a signed 10-bit DACV level (see core/src/dac.h), and
   the only other audio gate is IOP bit 5, which is binary. So the system
   menu's three-level sound setting (mute/low/loud) has to be a BIOS-side
   software value, applied by scaling the DACV amplitude the BIOS writes.

   Working theory: that value lives in kernel RAM (0x000-0x1FF), which is
   battery-backed on real hardware, and is already the known home of the
   date's century value (see core/src/rtc.h).

   This probe does not try to navigate to the sound-setting screen. It only
   establishes the two things phase 2 needs:
     1. A map of which kernel RAM bytes the BIOS actually touches at boot,
        with the PC that wrote each one. Route A (persisting kernel RAM as
        a battery-backed blob) needs this map regardless of the volume
        question.
     2. The boot beep's DACV envelope and the PC of the routine producing
        it. Phase 2 pokes candidate kernel RAM bytes, replays the beep from
        a save state, and looks for the byte that changes this envelope.

   Writes are found by diffing a 512-byte snapshot after every instruction
   rather than by hooking the bus, so this needs no core changes.

   Phase 2 ("sweep" mode) is the decisive experiment. It saves a state
   just before the boot beep, then for every kernel RAM byte and each of
   the three setting values, restores that state, pokes the byte, replays
   the beep, and compares the DACV envelope against the unpoked baseline.
   The byte whose value scales the beep's amplitude is the volume setting.
   Save states are a full struct copy (see psemu_save_state), so each
   replay is exact and independent.

   usage: volume_probe <bios.bin> [max_instructions]
          volume_probe <bios.bin> sweep */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

#define KERNEL_RAM_SIZE 0x200u

/* An audio "episode" is a run of DAC register activity with no more than
   this many quiet instructions inside it. The boot beep is a single
   episode; the gap to whatever plays next is far larger than this. */
#define EPISODE_GAP 200000L
#define MAX_EPISODES 32

typedef struct {
    long first_instr;
    long last_instr;
    long write_count;
    int dacv_min;
    int dacv_max;
    uint32_t first_pc;
    /* Distinct PCs seen writing DAC_DATA, capped - the beep loop is small,
       so a handful of slots covers it. */
    uint32_t writer_pcs[8];
    int writer_pc_count;
} episode_t;

typedef struct {
    long change_count;
    long first_instr;
    uint32_t first_pc;
    uint8_t first_value;
    uint8_t last_value;
    uint8_t distinct[4];
    int distinct_count;
} ram_byte_t;

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
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

/* DACV is bits 6-15 of DAC_DATA, signed 10-bit two's complement
   (see core/src/dac.h). */
static int dacv_of(uint32_t data) {
    int raw = (int)((data >> 6) & 0x3FFu);
    return (raw & 0x200) ? raw - 1024 : raw;
}

static void note_distinct(ram_byte_t *b, uint8_t value) {
    for (int i = 0; i < b->distinct_count; i++) {
        if (b->distinct[i] == value) {
            return;
        }
    }
    if (b->distinct_count < (int)(sizeof(b->distinct) / sizeof(b->distinct[0]))) {
        b->distinct[b->distinct_count++] = value;
    }
}

static void note_writer_pc(episode_t *e, uint32_t pc) {
    for (int i = 0; i < e->writer_pc_count; i++) {
        if (e->writer_pcs[i] == pc) {
            return;
        }
    }
    if (e->writer_pc_count < (int)(sizeof(e->writer_pcs) / sizeof(e->writer_pcs[0]))) {
        e->writer_pcs[e->writer_pc_count++] = pc;
    }
}

/* Phase 1 found the boot beep here: DAC_CTRL is enabled at instr #14548,
   the first DACV write lands at #15405, and the episode runs to #409365.
   The sweep snapshots just before the first write and replays past the
   end of the episode. */
#define SWEEP_SNAPSHOT_AT 15000L
#define SWEEP_REPLAY_INSTR 405000L

typedef struct {
    long write_count;
    int dacv_min;
    int dacv_max;
    long abs_sum; /* sum of |DACV| over the replay - catches amplitude scaling */
    int faulted;
} beep_t;

/* Replays the beep from the already-restored state and measures it.
   No buttons are driven: the boot beep needs no input. */
static beep_t measure_beep(psemu_t *ps) {
    beep_t r;
    r.write_count = 0;
    r.dacv_min = 0;
    r.dacv_max = 0;
    r.abs_sum = 0;
    r.faulted = 0;

    uint32_t prev_data = ps->dac.data;
    for (long i = 0; i < SWEEP_REPLAY_INSTR; i++) {
        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, step_cycles);
        dac_tick(&ps->dac, step_cycles);

        if (ps->dac.data != prev_data) {
            int dacv = dacv_of(ps->dac.data);
            if (r.write_count == 0) {
                r.dacv_min = dacv;
                r.dacv_max = dacv;
            } else if (dacv < r.dacv_min) {
                r.dacv_min = dacv;
            } else if (dacv > r.dacv_max) {
                r.dacv_max = dacv;
            }
            r.write_count++;
            r.abs_sum += dacv < 0 ? -dacv : dacv;
            prev_data = ps->dac.data;
        }
        if (psemu_cpu_faulted(ps)) {
            r.faulted = 1;
            break;
        }
    }
    return r;
}

static int beep_differs(const beep_t *a, const beep_t *b) {
    return a->write_count != b->write_count || a->dacv_min != b->dacv_min || a->dacv_max != b->dacv_max ||
           a->abs_sum != b->abs_sum;
}

static int run_sweep(psemu_t *ps) {
    printf("=== phase 2: kernel RAM poke sweep over the boot beep ===\n");

    /* Boot up to the snapshot point. No buttons: the beep happens during
       the HELLO/heart animation, before any input is needed. */
    for (long i = 0; i < SWEEP_SNAPSHOT_AT; i++) {
        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, step_cycles);
        dac_tick(&ps->dac, step_cycles);
    }

    size_t state_size = psemu_state_size(ps);
    void *state = malloc(state_size);
    if (!state || psemu_save_state(ps, state, state_size) != PSEMU_OK) {
        fprintf(stderr, "failed to save state\n");
        return 1;
    }
    printf("snapshot taken at instr #%ld (%zu bytes)\n", SWEEP_SNAPSHOT_AT, state_size);

    beep_t base = measure_beep(ps);
    printf(
        "baseline beep: %ld DACV writes, range %d..%d, abs_sum=%ld%s\n\n", base.write_count, base.dacv_min,
        base.dacv_max, base.abs_sum, base.faulted ? " (FAULTED)" : "");
    if (base.write_count == 0) {
        fprintf(stderr, "no beep in the replay window - adjust SWEEP_SNAPSHOT_AT/SWEEP_REPLAY_INSTR\n");
        free(state);
        return 1;
    }

    /* 0/1/2 covers a stored three-level setting index. 0x3F covers the
       other plausible encoding: the stored value IS an amplitude, in
       which case halving it should halve the beep. The audio code was
       seen reading several 0x7F bytes (see phase 3). */
    static const uint8_t poke_values[] = {0, 1, 2, 0x3F};
    int hits = 0;
    /* The whole 2KB, not just kernel RAM: phase 3 caught the tone routine
       reading a block at 0x280-0x2A7, above the kernel/user line. */
    for (uint32_t a = 0; a < PSEMU_RAM_SIZE; a++) {
        for (size_t v = 0; v < sizeof(poke_values) / sizeof(poke_values[0]); v++) {
            psemu_load_state(ps, state, state_size);
            if (ps->bus.ram[a] == poke_values[v]) {
                continue; /* already that value - nothing to learn */
            }
            uint8_t was = ps->bus.ram[a];
            ps->bus.ram[a] = poke_values[v];
            beep_t got = measure_beep(ps);
            if (!beep_differs(&base, &got)) {
                continue;
            }
            hits++;
            /* The signature we are hunting: the beep still plays, roughly
               as many writes as the baseline, but at a smaller amplitude.
               Anything that merely silences or derails the BIOS is a
               perturbation, not a volume control. */
            int still_playing = got.write_count > base.write_count / 2 && !got.faulted;
            int quieter = got.dacv_max < base.dacv_max || got.dacv_min > base.dacv_min;
            printf(
                "0x%03X: 0x%02X -> 0x%02X : %ld writes, range %d..%d, abs_sum=%ld%s%s\n", a, was, poke_values[v],
                got.write_count, got.dacv_min, got.dacv_max, got.abs_sum, got.faulted ? "  (FAULTED)" : "",
                (still_playing && quieter) ? "   <== AMPLITUDE SCALED" : "");
        }
    }
    printf("\n%d poke(s) changed the beep.\n", hits);
    free(state);
    return 0;
}

/* Phase 3 ("reads" mode): the sweep found no byte that scales the beep,
   so watch the other half of the access - what the tone routine reads.
   Phase 1 caught DACV writes coming from 0x0400397C and 0x04003B30, so
   the tone code lives around there; this range brackets both. */
#define AUDIO_PC_LO 0x04003400u
#define AUDIO_PC_HI 0x04003C80u

typedef struct {
    long read_count;
    uint32_t pcs[6];
    int pc_count;
    uint8_t last_value;
} ram_read_t;

static ram_read_t g_reads[PSEMU_RAM_SIZE];
static int g_read_trace_armed = 0;

static void ram_read_cb(uint32_t addr, uint8_t value, uint32_t pc) {
    /* The hook fires for every bus read, not just RAM (see memory.c), so
       anything outside RAM has to be dropped before indexing g_reads. */
    if (addr >= PSEMU_RAM_SIZE || !g_read_trace_armed || pc < AUDIO_PC_LO || pc >= AUDIO_PC_HI) {
        return;
    }
    ram_read_t *r = &g_reads[addr];
    r->read_count++;
    r->last_value = value;
    for (int i = 0; i < r->pc_count; i++) {
        if (r->pcs[i] == pc) {
            return;
        }
    }
    if (r->pc_count < (int)(sizeof(r->pcs) / sizeof(r->pcs[0]))) {
        r->pcs[r->pc_count++] = pc;
    }
}

static int run_read_trace(psemu_t *ps, long max_instr) {
    printf("=== phase 3: RAM reads issued by BIOS audio code (PC 0x%08X-0x%08X) ===\n", AUDIO_PC_LO, AUDIO_PC_HI);
    psemu_bus_read_trace_cb = ram_read_cb;
    g_read_trace_armed = 1;

    for (long i = 0; i < max_instr; i++) {
        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, step_cycles);
        dac_tick(&ps->dac, step_cycles);
        if (psemu_cpu_faulted(ps)) {
            printf("!! CPU faulted at instr #%ld - stopping\n", i);
            break;
        }
    }
    g_read_trace_armed = 0;
    psemu_bus_read_trace_cb = NULL;

    printf("%-6s %-9s %-6s %s\n", "addr", "reads", "last", "reading PCs");
    for (uint32_t a = 0; a < PSEMU_RAM_SIZE; a++) {
        ram_read_t *r = &g_reads[a];
        if (r->read_count == 0) {
            continue;
        }
        printf("0x%03X  %-9ld 0x%02X  ", a, r->read_count, r->last_value);
        for (int k = 0; k < r->pc_count; k++) {
            printf("%s0x%08X", k ? " " : "", r->pcs[k]);
        }
        if (r->pc_count == (int)(sizeof(r->pcs) / sizeof(r->pcs[0]))) {
            printf(" ...");
        }
        printf("\n");
    }
    return 0;
}

/* The byte the sweep identified: poking it left the beep's timing and
   write count untouched while halving DACV per increment. */
#define VOLUME_ADDR 0x290u

/* Maps that byte's full response curve, to see how many distinct levels
   it really has and whether one of them is the menu's "mute". */
static int run_levels(psemu_t *ps) {
    for (long i = 0; i < SWEEP_SNAPSHOT_AT; i++) {
        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, step_cycles);
        dac_tick(&ps->dac, step_cycles);
    }
    size_t state_size = psemu_state_size(ps);
    void *state = malloc(state_size);
    if (!state || psemu_save_state(ps, state, state_size) != PSEMU_OK) {
        fprintf(stderr, "failed to save state\n");
        return 1;
    }

    printf("=== response curve of RAM 0x%03X over the boot beep ===\n", VOLUME_ADDR);
    printf("%-6s %-8s %-14s %s\n", "value", "writes", "DACV range", "abs_sum");
    static const uint8_t values[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x0F, 0x10, 0x1F, 0x7F, 0x80, 0xFF};
    for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
        psemu_load_state(ps, state, state_size);
        ps->bus.ram[VOLUME_ADDR] = values[v];
        beep_t got = measure_beep(ps);
        char range[32];
        snprintf(range, sizeof(range), "%d..%d", got.dacv_min, got.dacv_max);
        printf("0x%02X   %-8ld %-14s %ld%s\n", values[v], got.write_count, range, got.abs_sum,
               got.faulted ? "  (FAULTED)" : "");
    }
    free(state);
    return 0;
}

/* Logs every read and write of one address, with the PC responsible, so
   the persistent source of the volume value can be traced back. Writes
   are caught by watching the byte after each step; reads come through the
   memory.c trace hook. */
static uint32_t g_watch_addr = 0;
static long g_watch_instr = 0;
static long g_watch_reads = 0;
static uint32_t g_watch_read_pcs[16];
static int g_watch_read_pc_count = 0;

static void watch_read_cb(uint32_t addr, uint8_t value, uint32_t pc) {
    if (addr != g_watch_addr) {
        return;
    }
    g_watch_reads++;
    for (int i = 0; i < g_watch_read_pc_count; i++) {
        if (g_watch_read_pcs[i] == pc) {
            return;
        }
    }
    if (g_watch_read_pc_count < (int)(sizeof(g_watch_read_pcs) / sizeof(g_watch_read_pcs[0]))) {
        g_watch_read_pcs[g_watch_read_pc_count++] = pc;
        printf("  read  instr #%-9ld pc=0x%08X value=0x%02X  (first read from this PC)\n", g_watch_instr, pc, value);
    }
}

static int run_watch(psemu_t *ps, uint32_t addr, long max_instr) {
    g_watch_addr = addr;
    printf("=== watching RAM 0x%03X for %ld instructions ===\n", addr, max_instr);
    psemu_bus_read_trace_cb = watch_read_cb;

    uint8_t prev = ps->bus.ram[addr];
    for (long i = 0; i < max_instr; i++) {
        g_watch_instr = i;
        uint32_t pc_before = ps->cpu.r[15];

        /* Same confirmed power-on sequence as phase 1, so the run gets
           past the time-setting screen and into the BIOS menu proper. */
        {
            long phase = i % 2500000;
            uint32_t buttons = 0;
            if (phase >= 200000 && phase < 350000) {
                buttons = PSEMU_BUTTON_DOWN;
            } else if (phase >= 500000 && phase < 650000) {
                buttons = PSEMU_BUTTON_FIRE;
            }
            psemu_set_buttons(ps, buttons);
        }

        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, step_cycles);
        dac_tick(&ps->dac, step_cycles);

        if (ps->bus.ram[addr] != prev) {
            printf("  WRITE instr #%-9ld pc=0x%08X  0x%02X -> 0x%02X\n", i, pc_before, prev, ps->bus.ram[addr]);
            prev = ps->bus.ram[addr];
        }
        if (psemu_cpu_faulted(ps)) {
            printf("!! CPU faulted at instr #%ld\n", i);
            break;
        }
    }
    psemu_bus_read_trace_cb = NULL;
    printf("total reads: %ld, from %d distinct PCs\n", g_watch_reads, g_watch_read_pc_count);
    return 0;
}

/* Phase 4 ("menu" mode): the decisive read of the real setting values.
   Takes a desktop-frontend quicksave parked on the BIOS sound-setting
   screen, where Up cycles mute/quiet/loud, and taps Up repeatedly,
   reporting RAM 0x290 and the confirmation beep after each press.
   This is the only step that observes the actual menu rather than
   inferring from the boot beep.

   The quicksave file is a 16-byte frontend header (see
   quicksave_header_t in frontends/desktop/main.c) followed by the raw
   psemu_save_state blob. */
#define QUICKSAVE_HEADER_SIZE 16u

/* Real taps are ~40ms; tools/inspect.c settled on ~150000 instructions
   held with a generous gap for the screen to settle. */
#define TAP_HOLD 150000L
#define TAP_GAP 250000L

static int run_menu(psemu_t *ps, const char *sav_path, int presses) {
    size_t sav_size = 0;
    uint8_t *sav = read_file(sav_path, &sav_size);
    if (!sav) {
        fprintf(stderr, "failed to read quicksave %s\n", sav_path);
        return 1;
    }
    size_t state_size = psemu_state_size(ps);
    if (sav_size != QUICKSAVE_HEADER_SIZE + state_size) {
        fprintf(
            stderr, "quicksave is %zu bytes, expected %zu (%u header + %zu state)\n", sav_size,
            QUICKSAVE_HEADER_SIZE + state_size, QUICKSAVE_HEADER_SIZE, state_size);
        free(sav);
        return 1;
    }
    if (psemu_load_state(ps, sav + QUICKSAVE_HEADER_SIZE, state_size) != PSEMU_OK) {
        fprintf(stderr, "failed to load state\n");
        free(sav);
        return 1;
    }
    free(sav);

    printf("=== phase 4: Up-key walk of the BIOS sound-setting screen ===\n");
    printf("loaded %s: pc=0x%08X, RAM 0x%03X = 0x%02X\n\n", sav_path, ps->cpu.r[15], VOLUME_ADDR,
           ps->bus.ram[VOLUME_ADDR]);

    /* Whether the BIOS also commits the setting to the card decides how
       persistence has to work: if flash never changes, the value lives
       only in battery-backed RAM and a frontend must persist it itself. */
    static uint8_t flash_before[PSEMU_FLASH_SIZE];
    memcpy(flash_before, ps->flash.data, PSEMU_FLASH_SIZE);

    uint8_t prev = ps->bus.ram[VOLUME_ADDR];
    for (int p = 1; p <= presses; p++) {
        uint32_t prev_data = ps->dac.data;
        long writes = 0;
        int dacv_min = 0, dacv_max = 0;

        for (long i = 0; i < TAP_HOLD + TAP_GAP; i++) {
            uint32_t pc_before = ps->cpu.r[15];
            psemu_set_buttons(ps, i < TAP_HOLD ? PSEMU_BUTTON_UP : 0u);

            uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
            timer_tick(&ps->timer, &ps->intc, step_cycles);
            rtc_tick(&ps->rtc, &ps->intc, step_cycles);
            dac_tick(&ps->dac, step_cycles);

            if (ps->bus.ram[VOLUME_ADDR] != prev) {
                printf(
                    "  press %d: RAM 0x%03X 0x%02X -> 0x%02X   (written by pc=0x%08X)\n", p, VOLUME_ADDR, prev,
                    ps->bus.ram[VOLUME_ADDR], pc_before);
                prev = ps->bus.ram[VOLUME_ADDR];
            }
            if (ps->dac.data != prev_data) {
                int dacv = dacv_of(ps->dac.data);
                if (writes == 0) {
                    dacv_min = dacv;
                    dacv_max = dacv;
                } else if (dacv < dacv_min) {
                    dacv_min = dacv;
                } else if (dacv > dacv_max) {
                    dacv_max = dacv;
                }
                writes++;
                prev_data = ps->dac.data;
            }
            if (psemu_cpu_faulted(ps)) {
                printf("!! CPU faulted during press %d\n", p);
                return 1;
            }
        }
        printf(
            "  press %d done: RAM 0x%03X = 0x%02X, beep = %ld DACV writes, range %d..%d\n\n", p, VOLUME_ADDR, prev,
            writes, dacv_min, dacv_max);
    }

    long flash_changed = 0;
    long first_change = -1;
    for (long a = 0; a < PSEMU_FLASH_SIZE; a++) {
        if (ps->flash.data[a] != flash_before[a]) {
            if (first_change < 0) {
                first_change = a;
            }
            flash_changed++;
        }
    }
    if (flash_changed == 0) {
        printf("flash: unchanged across every press - the setting is RAM-only\n");
    } else {
        printf("flash: %ld byte(s) changed, first at 0x%05lX\n", flash_changed, first_change);
    }
    return 0;
}

/* Phase 5 ("boot" mode): does a frontend-applied volume survive the boot?
   The desktop frontend writes 0x290 before every frame it emulates (see
   the Tools > Volume Override block in frontends/desktop/main.c), which
   should mean the value is in place well before sound init reads it at
   ~instr #14548. The reported symptom is that the boot chime plays at
   full volume anyway.

   The earlier watch mode cannot see the cause: it starts from a
   psemu_reset, where RAM is already all zeroes, so a BIOS-side clear of
   0x290 back to 0x00 is a no-op change that its prev-vs-now diff never
   reports. That is why "the BIOS never writes this byte" is recorded in
   docs/hardware-notes.md. This mode pre-loads a non-zero value first, so
   any such clear becomes visible.

   reapply=0 writes the byte once before instruction 0; reapply=1 rewrites
   it every FRAME_CYCLES, exactly as the frontend's per-frame loop did
   before this was understood; reapply=2 uses psemu_set_volume_override,
   the fix, which needs no re-applying at all. */
#define FRAME_CYCLES 33000u

static int run_boot(psemu_t *ps, uint8_t level, int reapply, long max_instr) {
    static const char *const mode_names[] = {"once before boot", "every frame", "psemu_set_volume_override"};
    printf(
        "=== phase 5: boot with RAM 0x%03X pre-set to 0x%02X, mode=%s ===\n", VOLUME_ADDR, level,
        mode_names[reapply < 0 || reapply > 2 ? 0 : reapply]);

    if (reapply == 2) {
        psemu_set_volume_override(ps, level);
    } else {
        ps->bus.ram[VOLUME_ADDR] = level;
    }
    uint8_t prev = level;
    uint32_t prev_data = ps->dac.data;
    uint32_t prev_ctrl = ps->dac.ctrl;
    uint32_t frame_cycles = 0;
    long frame = 0;
    long writes = 0;
    int dacv_min = 0, dacv_max = 0;

    for (long i = 0; i < max_instr; i++) {
        uint32_t pc_before = ps->cpu.r[15];

        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, step_cycles);
        dac_tick(&ps->dac, step_cycles);

        if (ps->bus.ram[VOLUME_ADDR] != prev) {
            printf(
                "  WRITE instr #%-9ld frame %-5ld pc=0x%08X  0x%02X -> 0x%02X\n", i, frame, pc_before, prev,
                ps->bus.ram[VOLUME_ADDR]);
            prev = ps->bus.ram[VOLUME_ADDR];
        }
        if (ps->dac.ctrl != prev_ctrl) {
            printf(
                "  instr #%-9ld frame %-5ld DAC_CTRL enable=%u (pc=0x%08X), 0x%03X reads 0x%02X\n", i, frame,
                ps->dac.ctrl & 1u, pc_before, VOLUME_ADDR, ps->bus.ram[VOLUME_ADDR]);
            prev_ctrl = ps->dac.ctrl;
        }
        if (ps->dac.data != prev_data) {
            int dacv = dacv_of(ps->dac.data);
            if (writes == 0) {
                dacv_min = dacv;
                dacv_max = dacv;
                printf("  instr #%-9ld frame %-5ld first DACV write, 0x%03X reads 0x%02X\n", i, frame, VOLUME_ADDR,
                       ps->bus.ram[VOLUME_ADDR]);
            } else if (dacv < dacv_min) {
                dacv_min = dacv;
            } else if (dacv > dacv_max) {
                dacv_max = dacv;
            }
            writes++;
            prev_data = ps->dac.data;
        }

        /* The frontend re-applies between frames, never mid-frame. */
        frame_cycles += step_cycles;
        if (frame_cycles >= FRAME_CYCLES) {
            frame_cycles -= FRAME_CYCLES;
            frame++;
            if (reapply == 1) {
                ps->bus.ram[VOLUME_ADDR] = level;
                prev = level;
            }
        }
        if (psemu_cpu_faulted(ps)) {
            printf("!! CPU faulted at instr #%ld\n", i);
            break;
        }
    }

    printf(
        "\nboot beep: %ld DACV writes, range %d..%d   (0x%03X ends at 0x%02X)\n", writes, dacv_min, dacv_max,
        VOLUME_ADDR, ps->bus.ram[VOLUME_ADDR]);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(
            stderr,
            "usage: %s <bios.bin> [max_instructions|sweep|reads|levels|watch <hexaddr>|menu <file.sav>|"
            "boot <hexlevel> [reapply]]\n",
            argv[0]);
        return 1;
    }

    size_t bios_size = 0;
    uint8_t *bios = read_file(argv[1], &bios_size);
    if (!bios) {
        fprintf(stderr, "failed to read bios %s\n", argv[1]);
        return 1;
    }

    psemu_t *ps = psemu_create();
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        fprintf(stderr, "bad bios size: %zu (need %d)\n", bios_size, PSEMU_BIOS_SIZE);
        return 1;
    }
    free(bios);
    psemu_reset(ps);

    if (argc >= 3 && strcmp(argv[2], "sweep") == 0) {
        int rc = run_sweep(ps);
        psemu_destroy(ps);
        return rc;
    }
    if (argc >= 4 && strcmp(argv[2], "menu") == 0) {
        int rc = run_menu(ps, argv[3], argc >= 5 ? atoi(argv[4]) : 5);
        psemu_destroy(ps);
        return rc;
    }
    if (argc >= 4 && strcmp(argv[2], "boot") == 0) {
        uint8_t level = (uint8_t)strtoul(argv[3], NULL, 16);
        int reapply = argc >= 5 ? atoi(argv[4]) : 0;
        int rc = run_boot(ps, level, reapply, argc >= 6 ? atol(argv[5]) : 500000);
        psemu_destroy(ps);
        return rc;
    }
    if (argc >= 3 && strcmp(argv[2], "levels") == 0) {
        int rc = run_levels(ps);
        psemu_destroy(ps);
        return rc;
    }
    if (argc >= 3 && strcmp(argv[2], "watch") == 0) {
        uint32_t addr = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 16) : VOLUME_ADDR;
        int rc = run_watch(ps, addr, argc >= 5 ? atol(argv[4]) : 3000000);
        psemu_destroy(ps);
        return rc;
    }
    if (argc >= 3 && strcmp(argv[2], "reads") == 0) {
        int rc = run_read_trace(ps, argc >= 4 ? atol(argv[3]) : 500000);
        psemu_destroy(ps);
        return rc;
    }

    long max_instr = argc >= 3 ? atol(argv[2]) : 3000000;

    static ram_byte_t ram_map[KERNEL_RAM_SIZE];
    static uint8_t prev_ram[KERNEL_RAM_SIZE];
    memcpy(prev_ram, ps->bus.ram, KERNEL_RAM_SIZE);
    for (size_t i = 0; i < KERNEL_RAM_SIZE; i++) {
        ram_map[i].first_instr = -1;
    }

    static episode_t episodes[MAX_EPISODES];
    int episode_count = 0;

    uint32_t prev_dac_ctrl = ps->dac.ctrl;
    uint32_t prev_dac_data = ps->dac.data;
    int prev_sound_gate = iop_sound_enabled(&ps->iop);

    printf("=== phase 1: kernel RAM + DAC activity map, %ld instructions ===\n", max_instr);

    for (long i = 0; i < max_instr; i++) {
        uint32_t pc_before = ps->cpu.r[15];

        /* The real-hardware-confirmed power-on sequence (see
           tools/inspect.c, button_sim=3): Down, then Action, to get past
           the time-setting screen. Repeating, because a single attempt can
           land at the wrong moment in the boot animation. */
        {
            long phase = i % 2500000;
            uint32_t buttons = 0;
            if (phase >= 200000 && phase < 350000) {
                buttons = PSEMU_BUTTON_DOWN;
            } else if (phase >= 500000 && phase < 650000) {
                buttons = PSEMU_BUTTON_FIRE;
            }
            psemu_set_buttons(ps, buttons);
        }

        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, step_cycles);
        dac_tick(&ps->dac, step_cycles);

        for (uint32_t a = 0; a < KERNEL_RAM_SIZE; a++) {
            uint8_t now = ps->bus.ram[a];
            if (now == prev_ram[a]) {
                continue;
            }
            ram_byte_t *b = &ram_map[a];
            if (b->first_instr < 0) {
                b->first_instr = i;
                b->first_pc = pc_before;
                b->first_value = now;
                note_distinct(b, prev_ram[a]);
            }
            b->change_count++;
            b->last_value = now;
            note_distinct(b, now);
            prev_ram[a] = now;
        }

        int sound_gate = iop_sound_enabled(&ps->iop);
        if (sound_gate != prev_sound_gate) {
            printf(
                "instr #%ld: IOP sound gate -> %s (pc=0x%08X)\n", i, sound_gate ? "ENABLED" : "disabled", pc_before);
            prev_sound_gate = sound_gate;
        }
        if (ps->dac.ctrl != prev_dac_ctrl) {
            printf(
                "instr #%ld: DAC_CTRL 0x%08X -> 0x%08X (enable=%u, pc=0x%08X)\n", i, prev_dac_ctrl, ps->dac.ctrl,
                ps->dac.ctrl & 1u, pc_before);
            prev_dac_ctrl = ps->dac.ctrl;
        }
        if (ps->dac.data != prev_dac_data) {
            int dacv = dacv_of(ps->dac.data);
            episode_t *e = episode_count > 0 ? &episodes[episode_count - 1] : NULL;
            if (!e || i - e->last_instr > EPISODE_GAP) {
                if (episode_count < MAX_EPISODES) {
                    e = &episodes[episode_count++];
                    e->first_instr = i;
                    e->first_pc = pc_before;
                    e->dacv_min = dacv;
                    e->dacv_max = dacv;
                    e->write_count = 0;
                    e->writer_pc_count = 0;
                } else {
                    e = NULL;
                }
            }
            if (e) {
                e->last_instr = i;
                e->write_count++;
                if (dacv < e->dacv_min) {
                    e->dacv_min = dacv;
                }
                if (dacv > e->dacv_max) {
                    e->dacv_max = dacv;
                }
                note_writer_pc(e, pc_before);
            }
            prev_dac_data = ps->dac.data;
        }

        if (psemu_cpu_faulted(ps)) {
            printf("!! CPU faulted at instr #%ld, pc=0x%08X - stopping\n", i, pc_before);
            break;
        }
    }

    printf("\n=== kernel RAM (0x000-0x1FF) bytes touched during boot ===\n");
    printf("%-6s %-8s %-10s %-6s %-6s %s\n", "addr", "changes", "first@", "firstPC", "last", "distinct values");
    long touched = 0;
    for (uint32_t a = 0; a < KERNEL_RAM_SIZE; a++) {
        ram_byte_t *b = &ram_map[a];
        if (b->first_instr < 0) {
            continue;
        }
        touched++;
        printf("0x%03X  %-8ld %-10ld 0x%06X 0x%02X   ", a, b->change_count, b->first_instr, b->first_pc, b->last_value);
        for (int k = 0; k < b->distinct_count; k++) {
            printf("%s0x%02X", k ? "," : "", b->distinct[k]);
        }
        if (b->distinct_count == (int)(sizeof(b->distinct) / sizeof(b->distinct[0]))) {
            printf(",...");
        }
        printf("\n");
    }
    printf("(%ld of %u kernel RAM bytes were written at least once)\n", touched, KERNEL_RAM_SIZE);

    printf("\n=== DAC activity episodes ===\n");
    if (episode_count == 0) {
        printf("(none - no DAC_DATA writes at all in this run)\n");
    }
    for (int e = 0; e < episode_count; e++) {
        episode_t *ep = &episodes[e];
        printf(
            "episode %d: instr #%ld-#%ld (%ld DACV writes), DACV range %d..%d, first pc=0x%08X\n", e, ep->first_instr,
            ep->last_instr, ep->write_count, ep->dacv_min, ep->dacv_max, ep->first_pc);
        printf("  writer PCs:");
        for (int k = 0; k < ep->writer_pc_count; k++) {
            printf(" 0x%08X", ep->writer_pcs[k]);
        }
        if (ep->writer_pc_count == (int)(sizeof(ep->writer_pcs) / sizeof(ep->writer_pcs[0]))) {
            printf(" ...");
        }
        printf("\n");
    }

    psemu_destroy(ps);
    return 0;
}
