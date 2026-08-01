/* Where the BIOS keeps the date/time settings, and what decides whether
   it resets them to Jan 1 1999 (see docs/hardware-notes.md, "RTC").

   This is the companion investigation to tools/volume_probe.c. The volume
   setting turned out to be a byte of RAM the BIOS reads but never writes,
   so it survives on real hardware purely because the battery holds SRAM
   up. Date/time looks different: phase 1 of that investigation found a
   block at RAM 0x120-0x123 holding 01 01 99 19 (day, month, year, century
   in BCD - 1999-01-01), and unlike the volume byte it IS written during
   boot, from BIOS 0x040003D4.

   That matters beyond the date itself. The RTC's own date register has no
   century field (core/src/rtc.h), so the century has to live in RAM; and
   whatever the BIOS checks before deciding to overwrite that RAM with
   1999-01-01 is the same cold-vs-warm-boot discriminator that any
   battery-backed-SRAM persistence feature would have to satisfy.

   Modes:
     boot  - timeline of RAM writes and RTC register activity across the
             boot window, with the PC responsible for each.
     reads - every RAM read issued from a given PC range, to find what the
             reset decision actually consults.
     dump  - final contents of the candidate date/time RAM blocks.

   usage: datetime_probe <bios.bin> boot [max_instr]
          datetime_probe <bios.bin> reads <pc_lo_hex> <pc_hi_hex> [max_instr]
          datetime_probe <bios.bin> dump [max_instr] */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

/* Candidate date/time blocks, from the volume investigation's kernel RAM
   map. 0x120-0x123 read as 01 01 99 19; 0x128-0x12A as 01 01 99; 0x0CD
   and 0x0CF held 0x99 and 0x19 on their own. */
#define DATE_BLOCK_LO 0x0C0u
#define DATE_BLOCK_HI 0x130u

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

/* Drives the real-hardware-confirmed power-on sequence: Down, then
   Action, to get past the time-setting screen (see tools/inspect.c,
   button_sim=3). */
static void drive_boot_buttons(psemu_t *ps, long i) {
    long phase = i % 2500000;
    uint32_t buttons = 0;
    if (phase >= 200000 && phase < 350000) {
        buttons = PSEMU_BUTTON_DOWN;
    } else if (phase >= 500000 && phase < 650000) {
        buttons = PSEMU_BUTTON_FIRE;
    }
    psemu_set_buttons(ps, buttons);
}

static void step(psemu_t *ps) {
    uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
    timer_tick(&ps->timer, &ps->intc, step_cycles);
    rtc_tick(&ps->rtc, &ps->intc, step_cycles);
    dac_tick(&ps->dac, step_cycles);
}

static void print_block(const psemu_t *ps, uint32_t lo, uint32_t hi, const char *label) {
    printf("%s (0x%03X-0x%03X):\n", label, lo, hi - 1);
    for (uint32_t a = lo; a < hi; a += 16) {
        printf("  0x%03X: ", a);
        for (uint32_t k = 0; k < 16 && a + k < hi; k++) {
            printf("%02X ", ps->bus.ram[a + k]);
        }
        printf("\n");
    }
}

static int run_boot(psemu_t *ps, long max_instr) {
    printf("=== RAM writes and RTC activity across boot ===\n");
    static uint8_t prev[PSEMU_RAM_SIZE];
    memcpy(prev, ps->bus.ram, PSEMU_RAM_SIZE);

    uint32_t prev_rtc_mode = ps->rtc.mode;
    uint32_t prev_rtc_ctrl = ps->rtc.control;
    uint32_t prev_rtc_time = ps->rtc.time;
    uint32_t prev_rtc_date = ps->rtc.date;
    long adjusts = 0;

    for (long i = 0; i < max_instr; i++) {
        uint32_t pc_before = ps->cpu.r[15];
        drive_boot_buttons(ps, i);
        step(ps);

        /* Only the candidate date/time region: the rest of RAM churns
           constantly with stack and GUI state (see the volume probe's
           phase 1 map) and would bury the signal. */
        for (uint32_t a = DATE_BLOCK_LO; a < DATE_BLOCK_HI; a++) {
            if (ps->bus.ram[a] != prev[a]) {
                printf(
                    "instr #%-9ld RAM 0x%03X 0x%02X -> 0x%02X  pc=0x%08X\n", i, a, prev[a], ps->bus.ram[a],
                    pc_before);
                prev[a] = ps->bus.ram[a];
            }
        }

        if (ps->rtc.mode != prev_rtc_mode) {
            printf(
                "instr #%-9ld RTC_MODE 0x%08X -> 0x%08X (PRGSEL=%u CNTSEL=%u) pc=0x%08X\n", i, prev_rtc_mode,
                ps->rtc.mode, ps->rtc.mode & 1u, (ps->rtc.mode >> 1) & 7u, pc_before);
            prev_rtc_mode = ps->rtc.mode;
        }
        if (ps->rtc.control != prev_rtc_ctrl) {
            /* A write of 1 while control already holds 1 is the real
               "increment the CNTSEL-selected field" idiom (see rtc.h). */
            if (ps->rtc.control == 0 && prev_rtc_ctrl == 1) {
                adjusts++;
            }
            prev_rtc_ctrl = ps->rtc.control;
        }
        if (ps->rtc.date != prev_rtc_date) {
            printf(
                "instr #%-9ld RTC_DATE 0x%08X -> 0x%08X  pc=0x%08X\n", i, prev_rtc_date, ps->rtc.date, pc_before);
            prev_rtc_date = ps->rtc.date;
        }
        if (ps->rtc.time != prev_rtc_time) {
            /* Time advances once per second on its own; only report the
               first few so the log stays readable. */
            static long time_reports = 0;
            if (time_reports < 4) {
                printf(
                    "instr #%-9ld RTC_TIME 0x%08X -> 0x%08X  pc=0x%08X\n", i, prev_rtc_time, ps->rtc.time, pc_before);
                time_reports++;
            }
            prev_rtc_time = ps->rtc.time;
        }
        if (psemu_cpu_faulted(ps)) {
            printf("!! CPU faulted at instr #%ld\n", i);
            break;
        }
    }
    printf("\nRTC_ADJUST increments issued: %ld\n\n", adjusts);
    print_block(ps, DATE_BLOCK_LO, DATE_BLOCK_HI, "final date/time region");
    return 0;
}

static uint32_t g_pc_lo = 0, g_pc_hi = 0;
static long g_instr = 0;

typedef struct {
    long reads;
    uint32_t pcs[8];
    int pc_count;
    uint8_t last_value;
    long first_instr;
} read_rec_t;

static read_rec_t g_recs[PSEMU_RAM_SIZE];

static void read_cb(uint32_t addr, uint8_t value, uint32_t pc) {
    /* The hook fires for every bus read, not just RAM (see memory.c), so
       anything outside RAM has to be dropped before indexing g_recs. */
    if (addr >= PSEMU_RAM_SIZE || pc < g_pc_lo || pc >= g_pc_hi) {
        return;
    }
    read_rec_t *r = &g_recs[addr];
    if (r->reads == 0) {
        r->first_instr = g_instr;
    }
    r->reads++;
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

static int run_reads(psemu_t *ps, uint32_t pc_lo, uint32_t pc_hi, long max_instr) {
    g_pc_lo = pc_lo;
    g_pc_hi = pc_hi;
    printf("=== RAM reads issued from PC 0x%08X-0x%08X ===\n", pc_lo, pc_hi);
    psemu_bus_read_trace_cb = read_cb;
    for (long i = 0; i < max_instr; i++) {
        g_instr = i;
        drive_boot_buttons(ps, i);
        step(ps);
        if (psemu_cpu_faulted(ps)) {
            break;
        }
    }
    psemu_bus_read_trace_cb = NULL;

    printf("%-6s %-9s %-10s %-6s %s\n", "addr", "reads", "first@", "last", "reading PCs");
    for (uint32_t a = 0; a < PSEMU_RAM_SIZE; a++) {
        read_rec_t *r = &g_recs[a];
        if (r->reads == 0) {
            continue;
        }
        printf("0x%03X  %-9ld %-10ld 0x%02X  ", a, r->reads, r->first_instr, r->last_value);
        for (int k = 0; k < r->pc_count; k++) {
            printf("%s0x%08X", k ? " " : "", r->pcs[k]);
        }
        printf("\n");
    }
    return 0;
}

static int run_dump(psemu_t *ps, long max_instr) {
    for (long i = 0; i < max_instr; i++) {
        drive_boot_buttons(ps, i);
        step(ps);
        if (psemu_cpu_faulted(ps)) {
            break;
        }
    }
    printf("after %ld instructions:\n", max_instr);
    printf(
        "RTC: mode=0x%08X control=0x%08X time=0x%08X date=0x%08X\n", ps->rtc.mode, ps->rtc.control, ps->rtc.time,
        ps->rtc.date);
    print_block(ps, DATE_BLOCK_LO, DATE_BLOCK_HI, "date/time region");
    return 0;
}

/* Boots to a date-displaying screen, optionally poking a distinctive test
   date in first, and dumps the LCD so the on-screen digits can be read
   directly. This is what verifies two things the frontend plan rests on:
   that RAM 0x0CF really is the century the BIOS displays (it was only
   ever inferred from holding 0x19), and which of day/month comes first in
   the RAM shadows (the boot value 01 01 99 19 has both equal, so the
   order was never actually established).

   The test date is deliberately asymmetric: day 01, month 08, year 26,
   century 20. Jan-8-2026 and Aug-1-2026 are distinguishable on screen.

   `what` is a bitmask: 1 = RTC registers, 2 = century/year bytes at
   0x0CF/0x0CD, 4 = both RAM shadows at 0x120 and 0x128. */
#define TEST_DAY 0x01u
#define TEST_MONTH 0x08u
#define TEST_YEAR 0x26u
#define TEST_CENTURY 0x20u

#define POKE_RTC 1
#define POKE_CENTURY 2
#define POKE_SHADOWS 4
#define POKE_2B7 8
#define POKE_426 16

static void print_framebuffer(const psemu_t *ps) {
    const uint8_t *fb = psemu_get_framebuffer(ps);
    for (int row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
            int on = (fb[byte_index] >> (col % 8)) & 1;
            putchar(on ? '#' : '.');
        }
        putchar('\n');
    }
}

static int run_screen(psemu_t *ps, int what, long poke_at, long max_instr) {
    printf("=== screen dump: poke mask %d at instr #%ld ===\n", what, poke_at);
    for (long i = 0; i < max_instr; i++) {
        drive_boot_buttons(ps, i);
        step(ps);

        /* Poked after the BIOS's own boot-time RTC_ADJUST walk has
           finished, since anything written before it is overwritten. */
        if (i == poke_at) {
            if (what & POKE_RTC) {
                ps->rtc.date = ((uint32_t)TEST_YEAR << 16) | ((uint32_t)TEST_MONTH << 8) | TEST_DAY;
            }
            if (what & POKE_CENTURY) {
                ps->bus.ram[0x0CF] = TEST_CENTURY;
                ps->bus.ram[0x0CD] = TEST_YEAR;
            }
            if (what & POKE_SHADOWS) {
                ps->bus.ram[0x120] = TEST_DAY;
                ps->bus.ram[0x121] = TEST_MONTH;
                ps->bus.ram[0x122] = TEST_YEAR;
                ps->bus.ram[0x123] = TEST_CENTURY;
                ps->bus.ram[0x128] = TEST_DAY;
                ps->bus.ram[0x129] = TEST_MONTH;
                ps->bus.ram[0x12A] = TEST_YEAR;
                ps->bus.ram[0x12B] = TEST_CENTURY;
            }
            /* The only two RAM bytes still holding 0x19 once the RTC and
               every known date field have been moved off 1999. If the
               displayed century comes from RAM at all, it is one of
               these. */
            if (what & POKE_2B7) {
                ps->bus.ram[0x2B7] = TEST_CENTURY;
            }
            if (what & POKE_426) {
                ps->bus.ram[0x426] = TEST_CENTURY;
            }
            printf("poked at instr #%ld\n", i);
        }
        if (psemu_cpu_faulted(ps)) {
            printf("!! CPU faulted at instr #%ld\n", i);
            break;
        }
    }
    printf(
        "final RTC: date=0x%08X time=0x%08X, RAM 0x0CF=0x%02X 0x0CD=0x%02X\n", ps->rtc.date, ps->rtc.time,
        ps->bus.ram[0x0CF], ps->bus.ram[0x0CD]);
    print_block(ps, 0x120, 0x130, "shadows");
    /* If the screen still renders a "19" century while no RAM byte holds
       0x19, the displayed century cannot be coming from RAM at all. */
    printf("RAM bytes holding 0x19:");
    int found = 0;
    for (uint32_t a = 0; a < PSEMU_RAM_SIZE; a++) {
        if (ps->bus.ram[a] == 0x19) {
            printf(" 0x%03X", a);
            found++;
        }
    }
    printf("%s (%d total)\n", found ? "" : " none", found);
    printf("LCD:\n");
    print_framebuffer(ps);
    return 0;
}

/* Identifies which SWI reads the century byte, by calling every entry in
   the kernel's SWI dispatch table in isolation and watching what each one
   touches.

   The open question this closes: the clock screen's century comes from
   RAM 0x426, but apps read the date through GetBcdDate, and rtc.h says
   that SWI is the only thing exposing a century. If GetBcdDate reads
   0x0CF (the byte the BIOS derives from the RTC year) rather than 0x426,
   then apps and the BIOS clock screen can disagree about the year, and a
   frontend override that writes only 0x426 would leave apps out of sync.

   The dispatch table's base address lives at RAM 0x000000E0 (see
   docs/hardware-notes.md, "SWI (syscall) mechanism"). Each entry is
   called with a scratch output buffer in r0 and a sentinel return address
   in lr, from a state snapshot taken after boot, and restored afterwards
   so one entry cannot affect the next. */
#define SWI_TABLE_PTR 0x000000E0u
#define SWI_TABLE_ENTRIES 24
#define SWI_SENTINEL_LR 0x04FFFFF0u
#define SWI_SCRATCH_BUF 0x00000700u
#define SWI_MAX_INSTR 20000

static int g_saw_cf = 0;
static int g_saw_426 = 0;
static int g_saw_rtc = 0;

static void swi_watch_cb(uint32_t addr, uint8_t value, uint32_t pc) {
    (void)value;
    (void)pc;
    if (addr == 0x0CFu) {
        g_saw_cf = 1;
    } else if (addr == 0x426u) {
        g_saw_426 = 1;
    } else if (addr >= PSEMU_RTC_BASE && addr < PSEMU_RTC_BASE + RTC_REG_SPAN) {
        g_saw_rtc = 1;
    }
}

static int run_swi(psemu_t *ps, long boot_instr, int poke) {
    for (long i = 0; i < boot_instr; i++) {
        drive_boot_buttons(ps, i);
        step(ps);
    }

    uint32_t table_base = psemu_bus_read32(&ps->bus, SWI_TABLE_PTR);
    printf("=== SWI dispatch table at 0x%08X (from RAM 0x%03X) ===\n", table_base, SWI_TABLE_PTR);

    size_t state_size = psemu_state_size(ps);
    void *state = malloc(state_size);
    if (!state || psemu_save_state(ps, state, state_size) != PSEMU_OK) {
        fprintf(stderr, "failed to save state\n");
        return 1;
    }

    printf("%-5s %-12s %-8s %-9s %-7s %-7s %s\n", "swi", "entry", "mode", "ran", "0x0CF", "0x426", "RTC regs");
    for (int n = 0; n < SWI_TABLE_ENTRIES; n++) {
        psemu_load_state(ps, state, state_size);
        uint32_t entry = psemu_bus_read32(&ps->bus, table_base + (uint32_t)n * 4u);
        if (entry < PSEMU_BIOS_BASE || entry >= PSEMU_BIOS_BASE + PSEMU_BIOS_SIZE) {
            printf("%-5d 0x%08X   (not a BIOS address - skipped)\n", n, entry);
            continue;
        }

        /* With a distinctive date in the RTC and a distinctive century in
           0x0CF, whatever the routine writes into the scratch buffer says
           plainly which source it used. */
        if (poke) {
            ps->rtc.date = ((uint32_t)TEST_YEAR << 16) | ((uint32_t)TEST_MONTH << 8) | TEST_DAY;
            ps->bus.ram[0x0CF] = TEST_CENTURY;
            ps->bus.ram[0x426] = 0x19; /* left at the boot value, to tell the two apart */
        }
        for (uint32_t k = 0; k < 8; k++) {
            ps->bus.ram[SWI_SCRATCH_BUF + k] = 0xEE; /* poison, so writes are obvious */
        }

        int thumb = entry & 1u;
        arm_set_mode(&ps->cpu, ARM_MODE_SVC);
        ps->cpu.r[15] = entry & ~1u;
        ps->cpu.r[14] = SWI_SENTINEL_LR;
        ps->cpu.r[0] = SWI_SCRATCH_BUF;
        ps->cpu.r[1] = SWI_SCRATCH_BUF;
        ps->cpu.r[2] = 0;
        ps->cpu.r[3] = 0;
        if (thumb) {
            ps->cpu.cpsr |= CPSR_T;
        } else {
            ps->cpu.cpsr &= ~CPSR_T;
        }

        g_saw_cf = 0;
        g_saw_426 = 0;
        g_saw_rtc = 0;
        psemu_bus_read_trace_cb = swi_watch_cb;

        long ran = 0;
        int returned = 0;
        for (; ran < SWI_MAX_INSTR; ran++) {
            if ((ps->cpu.r[15] & ~1u) == SWI_SENTINEL_LR) {
                returned = 1;
                break;
            }
            step(ps);
            if (psemu_cpu_faulted(ps)) {
                break;
            }
        }
        psemu_bus_read_trace_cb = NULL;

        printf(
            "%-5d 0x%08X   %-8s %-9ld %-7s %-7s %s\n", n, entry, thumb ? "thumb" : "arm", ran,
            g_saw_cf ? "READ" : "-", g_saw_426 ? "READ" : "-", g_saw_rtc ? "read" : "-");
        if (returned) {
            printf("      r0=0x%08X  scratch:", ps->cpu.r[0]);
            for (uint32_t k = 0; k < 8; k++) {
                printf(" %02X", ps->bus.ram[SWI_SCRATCH_BUF + k]);
            }
            printf("\n");
        }
        if (!returned) {
            printf("      (did not reach the sentinel - result for this entry is unreliable)\n");
        }
    }
    free(state);
    return 0;
}

/* Every non-RAM, non-ROM read the BIOS issues before it starts stepping
   the clock, in order. This is what identifies the source the reset
   decision is actually gated on: the warm-boot test showed the reset
   fires even with a valid date already in the RTC, so the discriminator
   is neither RAM (nothing is read before it is written) nor the RTC's
   own date register. */
typedef struct {
    uint32_t addr;
    uint8_t value;
    uint32_t pc;
    long instr;
} io_read_t;

#define MAX_IO_READS 4096
static io_read_t g_io[MAX_IO_READS];
static int g_io_count = 0;
static long g_io_limit_instr = 0;

static void io_read_cb(uint32_t addr, uint8_t value, uint32_t pc) {
    /* RAM and BIOS ROM are the bulk of all reads and are not candidates:
       ROM is constant, and RAM was already ruled out. */
    if (addr < PSEMU_RAM_SIZE) {
        return;
    }
    if (addr >= PSEMU_BIOS_BASE && addr < PSEMU_BIOS_BASE + PSEMU_BIOS_SIZE) {
        return;
    }
    if (g_instr > g_io_limit_instr || g_io_count >= MAX_IO_READS) {
        return;
    }
    g_io[g_io_count].addr = addr;
    g_io[g_io_count].value = value;
    g_io[g_io_count].pc = pc;
    g_io[g_io_count].instr = g_instr;
    g_io_count++;
}

static const char *region_name(uint32_t addr) {
    if (addr >= PSEMU_FLASH1_BASE && addr < PSEMU_FLASH1_BASE + PSEMU_FLASH_SIZE) return "FLASH1";
    if (addr >= PSEMU_FLASH2_BASE && addr < PSEMU_FLASH2_BASE + PSEMU_FLASH_SIZE) return "FLASH2";
    if (addr >= PSEMU_FLASH_CTRL_BASE && addr < PSEMU_FLASH_CTRL_BASE + FLASH_CTRL_SPAN) return "FLASH_CTRL";
    if (addr >= PSEMU_INTC_BASE && addr < PSEMU_INTC_BASE + INTC_REG_SPAN) return "INTC";
    if (addr >= PSEMU_TIMER_BASE && addr < PSEMU_TIMER_BASE + TIMER_REG_SPAN) return "TIMER";
    if (addr >= PSEMU_CLK_BASE && addr < PSEMU_CLK_BASE + CLK_REG_SPAN) return "CLK";
    if (addr >= PSEMU_RTC_BASE && addr < PSEMU_RTC_BASE + RTC_REG_SPAN) return "RTC";
    if (addr >= PSEMU_IR_BASE && addr < PSEMU_IR_BASE + IR_REG_SPAN) return "IR";
    if (addr >= PSEMU_LCD_VRAM_BASE && addr < PSEMU_LCD_VRAM_BASE + LCD_VRAM_SIZE) return "LCD_VRAM";
    if (addr >= PSEMU_LCD_MODE_BASE && addr < PSEMU_LCD_MODE_BASE + LCD_MODE_REG_SPAN) return "LCD_MODE";
    if (addr >= PSEMU_DAC_BASE && addr < PSEMU_DAC_BASE + DAC_REG_SPAN) return "DAC";
    if (addr >= PSEMU_IOP_BASE && addr < PSEMU_IOP_BASE + IOP_REG_SPAN) return "IOP";
    return "UNMAPPED";
}

static int run_io(psemu_t *ps, long until_instr) {
    g_io_limit_instr = until_instr;
    printf("=== I/O reads up to instr #%ld (RAM and BIOS ROM excluded) ===\n", until_instr);
    psemu_bus_read_trace_cb = io_read_cb;
    for (long i = 0; i <= until_instr; i++) {
        g_instr = i;
        drive_boot_buttons(ps, i);
        step(ps);
        if (psemu_cpu_faulted(ps)) {
            break;
        }
    }
    psemu_bus_read_trace_cb = NULL;

    printf("%-10s %-12s %-12s %-6s %s\n", "instr", "addr", "region", "value", "pc");
    uint32_t last_addr = 0xFFFFFFFFu;
    long run_len = 0;
    for (int k = 0; k < g_io_count; k++) {
        /* Poll loops read the same address thousands of times; collapse
           consecutive repeats so the interesting one-shot reads show. */
        if (g_io[k].addr == last_addr) {
            run_len++;
            continue;
        }
        if (run_len > 1) {
            printf("           (previous address repeated %ld more times)\n", run_len - 1);
        }
        printf(
            "%-10ld 0x%08X   %-12s 0x%02X   0x%08X\n", g_io[k].instr, g_io[k].addr, region_name(g_io[k].addr),
            g_io[k].value, g_io[k].pc);
        last_addr = g_io[k].addr;
        run_len = 1;
    }
    if (run_len > 1) {
        printf("           (previous address repeated %ld more times)\n", run_len - 1);
    }
    printf("\n%d I/O reads recorded%s\n", g_io_count, g_io_count >= MAX_IO_READS ? " (buffer full)" : "");
    return 0;
}

/* Simulates a warm boot: a device whose battery kept the RTC running, so
   the clock already holds a real date rather than the power-on-reset
   1998-01-01. If the BIOS's Jan-1999 reset is gated on the RTC's own
   contents, this date should survive boot untouched.

   RTC_DATE packs day, month, year (BCD) from the LSB up; RTC_TIME packs
   seconds, minutes, hours, day-of-week (see core/src/rtc.h). */
#define WARM_DATE 0x00070615u /* 2007-06-15 */
#define WARM_TIME 0x06124530u /* Fri 12:45:30 */
#define WARM_CENTURY 0x20u

static int run_warm(psemu_t *ps, int poke_ram, long max_instr, const char *card_path) {
    if (card_path) {
        /* The BIOS reads FLASH2 offset 0 shortly before it commits the
           date, and with no card loaded that read returns 0. A real card
           image is the one remaining untested input to the decision. */
        size_t card_size = 0;
        uint8_t *card = read_file(card_path, &card_size);
        if (!card) {
            fprintf(stderr, "failed to read card %s\n", card_path);
            return 1;
        }
        psemu_status st = psemu_load_flash_image(ps, card, card_size);
        free(card);
        printf("card %s: %s (%zu bytes)\n", card_path, st == PSEMU_OK ? "loaded" : "load FAILED", card_size);
    }
    ps->rtc.date = WARM_DATE;
    ps->rtc.time = WARM_TIME;
    printf("=== warm boot: RTC preloaded to date=0x%08X time=0x%08X ===\n", ps->rtc.date, ps->rtc.time);

    if (poke_ram) {
        /* The two RAM shadow copies of {day, month, year, century}, plus
           the standalone year/century bytes at 0x0CD/0x0CF. */
        ps->bus.ram[0x120] = 0x15;
        ps->bus.ram[0x121] = 0x06;
        ps->bus.ram[0x122] = 0x07;
        ps->bus.ram[0x123] = WARM_CENTURY;
        ps->bus.ram[0x128] = 0x15;
        ps->bus.ram[0x129] = 0x06;
        ps->bus.ram[0x12A] = 0x07;
        ps->bus.ram[0x12B] = WARM_CENTURY;
        ps->bus.ram[0x0CD] = 0x07;
        ps->bus.ram[0x0CF] = WARM_CENTURY;
        printf("    RAM date shadows and century preloaded too\n");
    }
    printf("\n");

    uint32_t prev_date = ps->rtc.date;
    uint32_t prev_time = ps->rtc.time;
    long time_reports = 0;
    for (long i = 0; i < max_instr; i++) {
        uint32_t pc_before = ps->cpu.r[15];
        drive_boot_buttons(ps, i);
        step(ps);
        if (ps->rtc.date != prev_date) {
            printf("instr #%-9ld RTC_DATE 0x%08X -> 0x%08X  pc=0x%08X\n", i, prev_date, ps->rtc.date, pc_before);
            prev_date = ps->rtc.date;
        }
        if (ps->rtc.time != prev_time) {
            if (time_reports < 3) {
                printf("instr #%-9ld RTC_TIME 0x%08X -> 0x%08X  pc=0x%08X\n", i, prev_time, ps->rtc.time, pc_before);
                time_reports++;
            }
            prev_time = ps->rtc.time;
        }
        if (psemu_cpu_faulted(ps)) {
            printf("!! CPU faulted at instr #%ld\n", i);
            break;
        }
    }

    printf(
        "\nfinal RTC: date=0x%08X time=0x%08X%s\n", ps->rtc.date, ps->rtc.time,
        (ps->rtc.date & 0x00FFFFFFu) == (WARM_DATE & 0x00FFFFFFu) ? "   <== DATE PRESERVED" : "   <== DATE OVERWRITTEN");
    print_block(ps, DATE_BLOCK_LO, DATE_BLOCK_HI, "date/time region");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(
            stderr,
            "usage: %s <bios.bin> boot [max_instr]\n"
            "       %s <bios.bin> reads <pc_lo_hex> <pc_hi_hex> [max_instr]\n"
            "       %s <bios.bin> dump [max_instr]\n",
            argv[0], argv[0], argv[0]);
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

    int rc = 1;
    if (strcmp(argv[2], "boot") == 0) {
        rc = run_boot(ps, argc >= 4 ? atol(argv[3]) : 60000);
    } else if (strcmp(argv[2], "reads") == 0 && argc >= 5) {
        rc = run_reads(
            ps, (uint32_t)strtoul(argv[3], NULL, 16), (uint32_t)strtoul(argv[4], NULL, 16),
            argc >= 6 ? atol(argv[5]) : 60000);
    } else if (strcmp(argv[2], "swi") == 0) {
        rc = run_swi(ps, argc >= 4 ? atol(argv[3]) : 3000000, argc >= 5 && strcmp(argv[4], "poke") == 0);
    } else if (strcmp(argv[2], "screen") == 0) {
        rc = run_screen(
            ps, argc >= 4 ? atoi(argv[3]) : 0, argc >= 5 ? atol(argv[4]) : 100000,
            argc >= 6 ? atol(argv[5]) : 3000000);
    } else if (strcmp(argv[2], "io") == 0) {
        rc = run_io(ps, argc >= 4 ? atol(argv[3]) : 14700);
    } else if (strcmp(argv[2], "warm") == 0) {
        rc = run_warm(
            ps, argc >= 4 && strcmp(argv[3], "ram") == 0, argc >= 5 ? atol(argv[4]) : 60000,
            argc >= 6 ? argv[5] : NULL);
    } else if (strcmp(argv[2], "dump") == 0) {
        rc = run_dump(ps, argc >= 4 ? atol(argv[3]) : 3000000);
    } else {
        fprintf(stderr, "unknown mode %s\n", argv[2]);
    }
    psemu_destroy(ps);
    return rc;
}
