#include "psemu_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIOS_RESET_VECTOR PSEMU_BIOS_BASE

psemu_t *psemu_create(void) {
    psemu_t *ps = (psemu_t *)malloc(sizeof(psemu_t));
    if (!ps) {
        return NULL;
    }
    lcd_init(&ps->lcd);
    intc_init(&ps->intc);
    flash_init(&ps->flash);
    ir_init(&ps->ir);
    timer_init(&ps->timer);
    rtc_init(&ps->rtc);
    dac_init(&ps->dac);
    clk_init(&ps->clk);
    iop_init(&ps->iop);
    psemu_bus_init(
        &ps->bus, &ps->lcd, &ps->intc, &ps->flash, &ps->ir, &ps->timer, &ps->rtc, &ps->dac, &ps->clk, &ps->iop);
    arm7tdmi_init(&ps->cpu, &ps->bus);
    ps->buttons = 0;
    ps->has_bios = 0;
    ps->real_time_cycle_carry = 0.0;
    ps->app_running = 0;
    ps->app_exec_idle_cycles = 0;
    return ps;
}

void psemu_destroy(psemu_t *ps) {
    free(ps);
}

void psemu_reset(psemu_t *ps) {
    /* A true hardware-level reset: every peripheral returns to its
       power-on default, the same way psemu_create sets each one up.
       Loaded content survives, since a reset does not erase ROM/flash
       content: ps->bus.bios[], ps->flash.data[], ps->flash.f_sn_lo/
       f_sn_hi/f_cal (the hardware ID and LCD calibration), and
       ps->has_bios are all left untouched.

       Without this, reloading a different BIOS or app/card mid-session
       left the previous session's peripheral register state (FLASH1 bank
       mapping, INTC enable/mask, CLK_MODE, DAC buffer state, stale RAM)
       sitting underneath the newly-loaded content, which is what caused
       visible glitches on reload. */
    memset(ps->bus.ram, 0, sizeof(ps->bus.ram));
    /* A frontend-held setting is deliberately not part of the power-on
       state this function restores: it stands in for battery-backed SRAM,
       which a reset is exactly what does NOT clear. Re-seed it after the
       wipe so it is already in place for the BIOS's first read, rather
       than only from the frontend's next frame. See
       psemu_set_volume_override. */
    if (ps->bus.ram_lock_addr < PSEMU_RAM_SIZE) {
        ps->bus.ram[ps->bus.ram_lock_addr] = ps->bus.ram_lock_value;
    }
    lcd_init(&ps->lcd);
    /* lcd_init clears dirty, correct for a fresh psemu_create where
       there is nothing on screen yet. A mid-session reset instead needs
       the frontend to redraw immediately, replacing whatever frame was
       already shown. */
    ps->lcd.dirty = 1;
    intc_init(&ps->intc);
    ir_init(&ps->ir);
    timer_init(&ps->timer);
    rtc_init(&ps->rtc);
    dac_init(&ps->dac);
    clk_init(&ps->clk);
    iop_init(&ps->iop);
    flash_reset_registers(&ps->flash);
    ps->buttons = 0;
    ps->real_time_cycle_carry = 0.0;
    /* A reset returns to the BIOS, so whatever app was dispatched is gone. */
    ps->app_running = 0;
    ps->app_exec_idle_cycles = 0;
    arm7tdmi_reset(&ps->cpu, BIOS_RESET_VECTOR);
}

psemu_status psemu_load_bios(psemu_t *ps, const uint8_t *data, size_t size) {
    if (size != PSEMU_BIOS_SIZE) {
        return PSEMU_ERR_BAD_SIZE;
    }
    memcpy(ps->bus.bios, data, size);
    ps->has_bios = 1;
    return PSEMU_OK;
}

psemu_status psemu_load_app(psemu_t *ps, const uint8_t *data, size_t size) {
    return flash_load_app(&ps->flash, data, size);
}

/* A PS1 directory frame layout:
   - Byte 0 is the in-use marker.
   - Bytes 4-7 hold the file's total data size, little-endian, a multiple of FLASH_BLOCK_SIZE.
   - The rest holds link/filename bookkeeping that this emulator does not need. */
#define MCS_HEADER_SIZE 0x80u
#define MCS_DATASIZE_OFFSET 0x04u

/* The directory-frame half of a .mcs's validation: everything that can be decided before handing the
   body to flash_load_app. Returns nonzero and sets *out_payload_size when `data` carries a frame whose
   recorded size agrees with the bytes that follow it.
   Shared by psemu_load_mcs and psemu_identify_content so the two cannot disagree about what a .mcs is. */
static int mcs_payload_size(const uint8_t *data, size_t size, size_t *out_payload_size) {
    size_t payload_size;
    uint32_t datasize;
    if (!data || size <= MCS_HEADER_SIZE) {
        return 0;
    }
    payload_size = size - MCS_HEADER_SIZE;
    if (payload_size % FLASH_BLOCK_SIZE != 0) {
        return 0;
    }
    datasize = (uint32_t)data[MCS_DATASIZE_OFFSET] | ((uint32_t)data[MCS_DATASIZE_OFFSET + 1] << 8) |
               ((uint32_t)data[MCS_DATASIZE_OFFSET + 2] << 16) | ((uint32_t)data[MCS_DATASIZE_OFFSET + 3] << 24);
    if (datasize != payload_size) {
        return 0;
    }
    *out_payload_size = payload_size;
    return 1;
}

psemu_status psemu_load_mcs(psemu_t *ps, const uint8_t *data, size_t size) {
    size_t payload_size;
    if (!mcs_payload_size(data, size, &payload_size)) {
        /* Preserved from when this was written inline: a short or misaligned file is a size problem, and
           a frame that disagrees with what follows it is a format problem. */
        if (!data || size <= MCS_HEADER_SIZE || (size - MCS_HEADER_SIZE) % FLASH_BLOCK_SIZE != 0) {
            return PSEMU_ERR_BAD_SIZE;
        }
        return PSEMU_ERR_BAD_FORMAT;
    }
    return flash_load_app(&ps->flash, data + MCS_HEADER_SIZE, payload_size);
}

psemu_status psemu_load_flash_image(psemu_t *ps, const uint8_t *data, size_t size) {
    if (size > sizeof(ps->flash.data)) {
        return PSEMU_ERR_BAD_SIZE;
    }
    memset(ps->flash.data, 0, sizeof(ps->flash.data));
    memcpy(ps->flash.data, data, size);
    return PSEMU_OK;
}

psemu_status psemu_save_flash_image(const psemu_t *ps, uint8_t *buf, size_t size) {
    if (size < sizeof(ps->flash.data)) {
        return PSEMU_ERR_BAD_SIZE;
    }
    memcpy(buf, ps->flash.data, sizeof(ps->flash.data));
    return PSEMU_OK;
}

/* See the header for why the layout these two expose is a contract rather than an implementation
   detail. Both are plain arrays inside psemu_t, so their addresses hold for the instance's lifetime. */
uint8_t *psemu_flash_data(psemu_t *ps) {
    return ps->flash.data;
}

uint8_t *psemu_ram_data(psemu_t *ps) {
    return ps->bus.ram;
}

psemu_status psemu_save_app_image(const psemu_t *ps, uint8_t *buf, size_t size) {
    return flash_save_app(&ps->flash, buf, size);
}

psemu_content_kind psemu_identify_content(const uint8_t *data, size_t size) {
    size_t payload_size;
    if (!data) {
        return PSEMU_CONTENT_UNKNOWN;
    }
    if (size == PSEMU_FLASH_SIZE) {
        return PSEMU_CONTENT_CARD;
    }
    /* A .mcs is checked before a bare body, because single-save exports are far more common than bare
       Title Sector dumps - and because the two are distinguishable: only the .mcs carries a directory
       frame whose recorded size matches what follows it. */
    if (mcs_payload_size(data, size, &payload_size) && flash_app_body_is_valid(data + MCS_HEADER_SIZE, payload_size)) {
        return PSEMU_CONTENT_MCS;
    }
    if (flash_app_body_is_valid(data, size)) {
        return PSEMU_CONTENT_APP;
    }
    return PSEMU_CONTENT_UNKNOWN;
}

/* FNV-1a, the same construction the desktop frontend used when it hashed whole files. Kept identical so
   the change is what gets hashed, not how. */
#define FNV1A_OFFSET_BASIS 2166136261u
#define FNV1A_PRIME 16777619u

static void identity_hash_update(uint32_t *hash, const uint8_t *data, size_t size) {
    size_t i;
    for (i = 0; i < size; i++) {
        *hash ^= data[i];
        *hash *= FNV1A_PRIME;
    }
}

/* A PS1 directory frame's identity: whether it is in use, and what the file is called. Deliberately not
   the size or the block link - those describe where the file sits, and a card rewritten by a save
   manager can move a file without it becoming a different file. */
#define DIRECTORY_FRAME_SIZE 128u
#define DIRECTORY_NAME_OFFSET 0x0Au
#define DIRECTORY_NAME_SIZE 21u

static void identity_hash_directory_frame(uint32_t *hash, const uint8_t *frame) {
    identity_hash_update(hash, frame, 1); /* allocation state */
    if (frame[0] != 0xA0u) {
        identity_hash_update(hash, frame + DIRECTORY_NAME_OFFSET, DIRECTORY_NAME_SIZE);
    }
}

/* Everything up to the end of the standard PS1 icon: header, title, CLUT, icon frames. See the header
   comment on psemu_content_identity_hash for why this region and not more. */
#define TITLE_SECTOR_ICON_FLAG_OFFSET 0x02u
#define TITLE_SECTOR_ICON_DATA_OFFSET 0x80u
#define TITLE_SECTOR_ICON_FRAME_SIZE 128u

static void identity_hash_title_sector(uint32_t *hash, const uint8_t *body, size_t size) {
    /* The standard PS1 icon-flag byte's low bits give the frame count, 1 to 3. Clamped rather than
       trusted: this runs on a file that has not been validated beyond its magic. */
    size_t frames = (size_t)(body[TITLE_SECTOR_ICON_FLAG_OFFSET] & 0x03u);
    size_t end;
    if (frames < 1u) {
        frames = 1u;
    }
    end = TITLE_SECTOR_ICON_DATA_OFFSET + frames * TITLE_SECTOR_ICON_FRAME_SIZE;
    if (end > size) {
        end = size;
    }
    identity_hash_update(hash, body, end);
}

uint32_t psemu_content_identity_hash(const uint8_t *data, size_t size) {
    uint32_t hash = FNV1A_OFFSET_BASIS;
    /* Initialized because nothing here enforces the invariant that makes the PSEMU_CONTENT_MCS branch
       below safe. psemu_identify_content only returns MCS when mcs_payload_size succeeded on these same
       arguments, and it is deterministic, so the second call cannot fail - but that reasoning spans two
       functions, which is why -Wmaybe-uninitialized flags it on GCC. If it ever stopped holding, the
       consequence would be identity_hash_title_sector reading past the buffer on a garbage length. Zero
       degrades that to hashing nothing extra. No reachable behaviour changes, so hashes already written
       to disk stay valid. */
    size_t payload_size = 0;
    uint32_t frame;
    if (!data) {
        return hash;
    }
    switch (psemu_identify_content(data, size)) {
    case PSEMU_CONTENT_CARD:
        for (frame = 1; frame < 16u; frame++) {
            identity_hash_directory_frame(&hash, data + frame * DIRECTORY_FRAME_SIZE);
        }
        return hash;
    case PSEMU_CONTENT_MCS:
        (void)mcs_payload_size(data, size, &payload_size);
        identity_hash_directory_frame(&hash, data);
        identity_hash_title_sector(&hash, data + MCS_HEADER_SIZE, payload_size);
        return hash;
    case PSEMU_CONTENT_APP:
        identity_hash_title_sector(&hash, data, size);
        return hash;
    default:
        identity_hash_update(&hash, data, size);
        return hash;
    }
}

psemu_status psemu_load_content(psemu_t *ps, const uint8_t *data, size_t size) {
    switch (psemu_identify_content(data, size)) {
    case PSEMU_CONTENT_CARD:
        return psemu_load_flash_image(ps, data, size);
    case PSEMU_CONTENT_MCS:
        return psemu_load_mcs(ps, data, size);
    case PSEMU_CONTENT_APP:
        return psemu_load_app(ps, data, size);
    default:
        /* Unchanged from when the dispatch was a fallback chain: nothing matched, so report the
           last-attempted loader's own status rather than inventing one. */
        return psemu_load_app(ps, data, size);
    }
}

uint32_t psemu_get_hardware_id(const psemu_t *ps) {
    return flash_get_serial(&ps->flash);
}

void psemu_set_hardware_id(psemu_t *ps, uint32_t id) {
    flash_set_serial(&ps->flash, id);
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

int psemu_parse_hardware_id(const char *str, uint32_t *out_id) {
    /* This function accepts only one form: exactly 8 plain hex digits (0-9, A-F/a-f).
       Real hardware has no "first digit must be a letter" restriction.
       Real-hardware testing confirmed this: a real ID-editing homebrew writes and
       persists a value like "EEEEEEEE".
       This form is also exactly what that homebrew displays and edits: 8 raw hex
       nibbles, nothing more structured.
       This function deliberately does not accept the letter+8-decimal-digit "sticker"
       form that real units print under their front cover.
       This keeps a persisted hardware-ID string what-you-see-is-what-you-get, instead
       of hiding a second, less-general encoding inside it.
       A frontend that wants to accept sticker-format input directly can convert it
       before calling this function.
       That conversion is a frontend-level convenience, not part of the canonical format. */
    uint32_t value;
    int i;

    if (!str) {
        return 0;
    }
    value = 0;
    for (i = 0; i < 8; i++) {
        int d = hex_digit_value(str[i]);
        if (d < 0) {
            return 0;
        }
        value = (value << 4) | (uint32_t)d;
    }
    if (str[8] != '\0') {
        return 0;
    }
    *out_id = value;
    return 1;
}

void psemu_format_hardware_id(uint32_t id, char *buf, size_t buf_size) {
    /* The canonical form (see psemu_parse_hardware_id) is 8 plain hex digits.
       This matches a real ID-editing homebrew's own on-screen representation exactly.
       This form can round-trip every value the real hardware allows.
       The letter+8-decimal "sticker" form cannot do this: it cannot represent a high
       byte outside A-Z/a-z. */
    snprintf(buf, buf_size, "%08X", (unsigned)id);
}

void psemu_set_buttons(psemu_t *ps, uint32_t buttons) {
    /* Real hardware asserts a button's interrupt line on every press/release edge
       (see docs/hardware-notes.md). Real hardware does not use a polled level for this.
       This code translates this emulator's own PSEMU_BUTTON_* bits to the real
       INT_BTN_* bits.
       PSEMU_BUTTON_* is an emulator-side convention, not real hardware's bit layout. */
    static const struct {
        uint32_t psemu_bit;
        uint32_t int_bit;
    } button_map[] = {
        {PSEMU_BUTTON_UP, INT_BTN_UP},
        {PSEMU_BUTTON_RIGHT, INT_BTN_RIGHT},
        {PSEMU_BUTTON_DOWN, INT_BTN_DOWN},
        {PSEMU_BUTTON_LEFT, INT_BTN_LEFT},
        {PSEMU_BUTTON_FIRE, INT_BTN_ACTION},
    };
    uint32_t changed = buttons ^ ps->buttons;
    size_t i;

    for (i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
        if (changed & button_map[i].psemu_bit) {
            intc_set_line(&ps->intc, button_map[i].int_bit, (buttons & button_map[i].psemu_bit) != 0);
        } else if (buttons & button_map[i].psemu_bit) {
            /* The button is still held; there is no fresh edge this call.
               HOLD should only pulse on the press edge.
               HOLD should not stay latched as a sustained level for the whole physical
               hold duration.

               A real-hardware discrepancy confirmed this. The generic system-tick
               callback branches on `hold & INT_BTN_ACTION` before its RTC check. A
               continuously-set hold bit permanently skips the RTC-driven redraw path
               for as long as the button is held. Real hardware keeps redrawing and
               blinking normally while the button is held; it only acts on release.

               STATUS is unaffected here: it keeps tracking the live level, for any
               code that polls it directly. */
            intc_clear_hold_only(&ps->intc, button_map[i].int_bit);
        }
    }
    ps->buttons = buttons;
}

/* How long the CPU must go without executing a FLASH1 instruction before a
   dispatched app counts as gone. See psemu_app_running in psemu/psemu.h.

   A running app spends real stretches inside the BIOS: every SWI it issues
   executes from the BIOS window until it returns. A single "PC is not in
   FLASH1" sample therefore means nothing, and this has to be a timeout rather
   than an instantaneous test.

   The unit is real elapsed time, expressed at the PSEMU_ASSUMED_CPU_HZ
   reference rate - the same accumulator psemu_run already feeds to RTC and
   DAC, and the same unit as a frontend's per-frame cycle budget. A raw
   step_cycles count would not do: those scale with CLK_MODE, whose table spans
   over two orders of magnitude, so any fixed value would mean seconds at the
   idle clock and milliseconds at the fastest one.

   One second is deliberately far longer than it needs to be. Getting this
   wrong in the "app already exited" direction costs a frontend nothing it can
   see - an override resumes a few frames later than it could have. Getting it
   wrong the other way puts back exactly the bug psemu_app_running exists to
   prevent. So it is sized to outlast any plausible BIOS call rather than to be
   responsive. */
#define APP_EXEC_GRACE_CYCLES ((uint32_t)PSEMU_ASSUMED_CPU_HZ)

/* FLASH1 is the windowed view of card flash that a dispatched app executes
   from (see flash.h). The BIOS itself never runs from it. */
static void psemu_track_app_execution(psemu_t *ps, uint32_t pc, uint32_t real_time_cycles) {
    if (pc >= PSEMU_FLASH1_BASE && pc < PSEMU_FLASH1_BASE + PSEMU_FLASH_SIZE) {
        ps->app_running = 1;
        ps->app_exec_idle_cycles = 0;
        return;
    }
    if (!ps->app_running) {
        return;
    }
    ps->app_exec_idle_cycles += real_time_cycles;
    if (ps->app_exec_idle_cycles >= APP_EXEC_GRACE_CYCLES) {
        ps->app_running = 0;
        ps->app_exec_idle_cycles = 0;
    }
}

int psemu_app_running(const psemu_t *ps) {
    return ps->app_running;
}

uint32_t psemu_run(psemu_t *ps, uint32_t cycles) {
    if (!ps->has_bios) {
        return 0;
    }
    /* `cycles` is a time budget expressed at the reference clock rate PSEMU_ASSUMED_CPU_HZ
       (see dac.h). See docs/hardware-notes.md, "CLK_MODE", for a summary. This comment
       gives the full reasoning.

       Timer follows raw, CLK_MODE-scaled step_cycles.
       Real timers are clocked by the System Clock, so they are tied directly to the
       CPU's variable clock, not to an independent oscillator.
       Direct measurement confirmed this. Pinning Timer to a fixed reference rate
       instead produced two errors:
       - The HELLO animation ran about 4x too slow during CLK_MODE=7. The HELLO
         animation is driven by the same Timer1 heartbeat that drives audio; both are
         confirmed GUI-code uses of the same IRQ.
       - The date-setting screen's blink ran about 2x too fast during CLK_MODE=4.
       Both errors matched the ratio between CLK_MODE=7/4's real Hz and the fixed
       reference rate almost exactly: 3.97x and 2.01x respectively.
       This confirms that Timer's rate must track CLK_MODE, not be decoupled from it.

       RTC and DAC remain pinned to real elapsed time regardless of CLK_MODE, for
       different reasons.
       RTC is a separate, CPU-clock-independent oscillator. Real hardware
       confirms this: its RTC ticks at a flat real 1Hz, unrelated to its CPU-frequency
       setting.
       This emulator's DAC resampling needs a fixed real-time output rate, to feed a
       standard audio API. This holds regardless of how often the app actually writes
       new DACV content.
       DACV content still tracks CLK_MODE via Timer: the audio content and pitch
       correctly vary with CLK_MODE, the same as on real hardware.

       A fractional carry (ps->real_time_cycle_carry) converts each step's real elapsed
       time, via the currently active clk_current_hz(), back into the fixed
       PSEMU_ASSUMED_CPU_HZ reference rate that RTC and DAC assume. This preserves real
       time exactly across steps, despite integer truncation. dac_tick already uses this
       same accumulator pattern internally. */
    double budget_seconds = (double)cycles / (double)PSEMU_ASSUMED_CPU_HZ;
    double elapsed_seconds = 0.0;
    uint32_t ran = 0;
    while (elapsed_seconds < budget_seconds) {
        uint32_t pc = ps->cpu.r[15];
        uint32_t step_cycles;
        /* Software asked the clock to stop (see clk.h). Execute nothing, and freeze the peripherals that
           run off the same oscillator: Timer is clocked by the System Clock, so it stops here too. That
           part matters. Waking on any asserted interrupt is not a stop at all, because a running Timer
           re-asserts within microseconds and the CPU never actually pauses.

           RTC and DAC keep their own real-time rate: the RTC is a separate oscillator (see rtc.h), and DAC
           resampling is this emulator's own fixed output rate rather than anything the hardware clocks.

           A button is an external signal rather than something this clock drives, so it can still wake the
           CPU, and that is what a user pressing a button on a sleeping PocketStation does. The wake clears
           the bit, so software does not have to. Whether real hardware auto-clears it, and whether any
           source other than a button can wake it, are both unconfirmed - no app this project can drive
           reaches either case. */
        if (clk_stop_requested(&ps->clk)) {
            static const uint32_t WAKE_SOURCES =
                INT_BTN_ACTION | INT_BTN_RIGHT | INT_BTN_LEFT | INT_BTN_DOWN | INT_BTN_UP;
            if (ps->intc.hold & ps->intc.enable & WAKE_SOURCES) {
                clk_clear_stop(&ps->clk);
            } else {
                /* One reference-rate cycle per iteration, deliberately, rather than one CPU cycle at the
                   current CLK_MODE. Nothing executes here, so the only thing this granularity controls is
                   how finely RTC and DAC get ticked - and picking the reference rate keeps `ran` (which this
                   function returns in reference-rate cycles) consistent with the real time actually
                   consumed. Counting CPU cycles instead over-reported `ran` by the ratio between the two
                   rates, up to about 4x at CLK_MODE 7, so a caller that paces itself by the return value
                   saw a sleeping device's clock run fast. */
                double stopped_dt = 1.0 / (double)PSEMU_ASSUMED_CPU_HZ;
                uint32_t real_time_cycles;
                elapsed_seconds += stopped_dt;
                ps->real_time_cycle_carry += stopped_dt * (double)PSEMU_ASSUMED_CPU_HZ;
                real_time_cycles = (uint32_t)ps->real_time_cycle_carry;
                ps->real_time_cycle_carry -= (double)real_time_cycles;
                rtc_tick(&ps->rtc, &ps->intc, real_time_cycles);
                dac_tick(&ps->dac, real_time_cycles);
                ran += 1u;
                continue;
            }
        }
        step_cycles = arm7tdmi_step(&ps->cpu);
        double dt = (double)step_cycles / (double)clk_current_hz(&ps->clk);
        elapsed_seconds += dt;

        ps->real_time_cycle_carry += dt * (double)PSEMU_ASSUMED_CPU_HZ;
        uint32_t real_time_cycles = (uint32_t)ps->real_time_cycle_carry;
        ps->real_time_cycle_carry -= (double)real_time_cycles;

        psemu_track_app_execution(ps, pc, real_time_cycles);

        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, real_time_cycles);
        dac_tick(&ps->dac, real_time_cycles);
        ir_tick(&ps->ir, &ps->intc, real_time_cycles);
        ran += step_cycles;
    }
    return ran;
}

const uint8_t *psemu_get_framebuffer(const psemu_t *ps) {
    return ps->lcd.presented;
}

/* Converts between ir_t's internal reference-rate cycle clock and real microseconds.
   It uses the same PSEMU_ASSUMED_CPU_HZ reference rate psemu_run already uses for RTC and DAC.
   This conversion happens only at this public-API boundary.
   Core's own ir_t stays in cycle units throughout, like every other peripheral. */
int psemu_ir_pop_tx_edge(psemu_t *ps, psemu_ir_edge_t *out_edge) {
    ir_edge_t edge;
    if (!ir_pop_tx_edge(&ps->ir, &edge)) {
        return 0;
    }
    out_edge->timestamp_us = (edge.timestamp_cycles * 1000000ull) / PSEMU_ASSUMED_CPU_HZ;
    out_edge->level = edge.level;
    return 1;
}

void psemu_ir_push_rx_edge(psemu_t *ps, uint64_t local_timestamp_us, int level) {
    uint64_t cycles = (local_timestamp_us * (uint64_t)PSEMU_ASSUMED_CPU_HZ) / 1000000ull;
    ir_push_rx_edge(&ps->ir, cycles, level);
}

uint64_t psemu_ir_get_clock_us(const psemu_t *ps) {
    return (ir_get_clock_cycles(&ps->ir) * 1000000ull) / PSEMU_ASSUMED_CPU_HZ;
}

int psemu_cpu_faulted(const psemu_t *ps) {
    return ps->cpu.unimplemented;
}

void psemu_write_crash_report(const psemu_t *ps, FILE *f) {
    const arm7tdmi_t *cpu = &ps->cpu;
    int thumb = (cpu->cpsr & CPSR_T) != 0;
    uint32_t i;

    fprintf(f, "psemu diagnostic report\n");
    fprintf(f, "total instructions executed: %llu\n", (unsigned long long)cpu->total_steps);
    fprintf(f, "buttons held: 0x%08X\n", ps->buttons);
    fprintf(f, "clk mode: 0x%08X\n", ps->clk.mode);
    fprintf(f, "cpu faulted (unrecognized opcode): %s\n", cpu->unimplemented ? "YES" : "no");

    /* FLASH1 (0x02000000+) is a live virtual window, resolved against F_BANK_FLG/F_BANK_VAL.
       FLASH1 is not a fixed offset into FLASH2 (see docs/hardware-notes.md).
       Without this bank information, a FLASH1 address in the trace or registers below
       cannot be reliably translated back to a physical byte offset for static analysis
       after the fact. */
    fprintf(f, "flash F_BANK_FLG (bank_mask): 0x%08X\n", ps->flash.bank_mask);
    fprintf(f, "flash F_BANK_VAL (bank_val[physical] = virtual):\n");
    for (i = 0; i < FLASH_BANK_VAL_COUNT; i++) {
        fprintf(f, "  [%u] = 0x%08X\n", i, ps->flash.bank_val[i]);
    }

    fprintf(f, "\nregisters:\n");
    for (i = 0; i < 15; i++) {
        fprintf(f, "  r%-2u = 0x%08X\n", i, cpu->r[i]);
    }
    fprintf(f, "  pc  = 0x%08X\n", cpu->r[15]);
    fprintf(f, "  cpsr = 0x%08X (mode=0x%02X, %s)\n", cpu->cpsr, cpu->cpsr & CPSR_MODE_MASK,
        thumb ? "thumb" : "arm");

    if (cpu->unimplemented) {
        /* r[15] has already been advanced past the faulting instruction; arm7tdmi_step
           does this before dispatch. This code undoes that advance, to find where the
           instruction was actually fetched from. This code then masks the result to the
           natural alignment for this mode. This matches tools/inspect.c's own crash
           reporter, which originally got this wrong (see docs/hardware-notes.md). */
        uint32_t fault_pc = thumb ? cpu->r[15] - 2u : cpu->r[15] - 4u;
        uint32_t fetch_pc = thumb ? (fault_pc & ~1u) : (fault_pc & ~3u);
        uint32_t raw = thumb ? psemu_bus_read16((psemu_bus_t *)&ps->bus, fetch_pc)
                              : psemu_bus_read32((psemu_bus_t *)&ps->bus, fetch_pc);
        fprintf(
            f, "\nfault: unrecognized %s opcode 0x%0*X, fetched from 0x%08X (pc was 0x%08X before advancing)\n",
            thumb ? "thumb" : "arm", thumb ? 4 : 8, raw, fetch_pc, fault_pc);
    }

    {
        uint32_t count = cpu->total_steps < PSEMU_TRACE_SIZE ? (uint32_t)cpu->total_steps : PSEMU_TRACE_SIZE;
        uint32_t start = (cpu->trace_pos - count + PSEMU_TRACE_SIZE) % PSEMU_TRACE_SIZE;
        fprintf(f, "\nlast %u executed PCs (oldest first):\n", count);
        for (i = 0; i < count; i++) {
            uint32_t idx = (start + i) % PSEMU_TRACE_SIZE;
            fprintf(
                f, "  pc=0x%08X %s\n", cpu->trace[idx].pc, (cpu->trace[idx].cpsr & CPSR_T) ? "(thumb)" : "(arm)");
        }
    }
}

int psemu_framebuffer_dirty(psemu_t *ps) {
    int was_dirty = ps->lcd.dirty;
    ps->lcd.dirty = 0;
    return was_dirty;
}

/* BIOS-owned settings that live in RAM. See docs/hardware-notes.md for how
   each address was found; none of this comes from a published register map.

   PSEMU_RAM_VOLUME: the sound setting. The BIOS reads it but never writes it
   at boot, so on real hardware it survives purely because the battery holds
   SRAM up.
   PSEMU_RAM_CENTURY_SCREEN: the century the BIOS clock screen renders.
   PSEMU_RAM_CENTURY_SWI: the century GetBcdDate (SWI 0Dh) returns to apps.
   These two really are separate bytes, and a test confirms they can disagree:
   the SWI reported 2026 while the screen rendered 1926. */
#define PSEMU_RAM_VOLUME 0x290u
#define PSEMU_RAM_CENTURY_SCREEN 0x426u
#define PSEMU_RAM_CENTURY_SWI 0x0CFu

void psemu_set_volume(psemu_t *ps, uint8_t level) {
    ps->bus.ram[PSEMU_RAM_VOLUME] = level;
}

uint8_t psemu_get_volume(const psemu_t *ps) {
    return ps->bus.ram[PSEMU_RAM_VOLUME];
}

void psemu_set_volume_override(psemu_t *ps, uint8_t level) {
    ps->bus.ram[PSEMU_RAM_VOLUME] = level;
    ps->bus.ram_lock_addr = PSEMU_RAM_VOLUME;
    ps->bus.ram_lock_value = level;
}

void psemu_clear_volume_override(psemu_t *ps) {
    ps->bus.ram_lock_addr = PSEMU_RAM_SIZE;
    ps->bus.ram_lock_value = 0;
}

static uint8_t to_bcd(int value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

int psemu_set_datetime(psemu_t *ps, int year, int month, int day, int hour, int minute, int second, int dow) {
    if (year < 0 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59 || dow < 1 || dow > 7) {
        return 0;
    }
    /* PRGSEL: the BIOS is mid-way through stepping the clock with
       RTC_ADJUST increments. Writing the registers now can stop its loop
       converging. See this function's comment in psemu/psemu.h. */
    if (ps->rtc.mode & 1u) {
        return 0;
    }

    /* RTC_DATE packs day, month, year from the LSB up; RTC_TIME packs
       seconds, minutes, hours, day-of-week. See core/src/rtc.h. */
    ps->rtc.date = ((uint32_t)to_bcd(year % 100) << 16) | ((uint32_t)to_bcd(month) << 8) | to_bcd(day);
    ps->rtc.time = ((uint32_t)to_bcd(dow) << 24) | ((uint32_t)to_bcd(hour) << 16) | ((uint32_t)to_bcd(minute) << 8) |
                   to_bcd(second);

    uint8_t century = to_bcd(year / 100);
    ps->bus.ram[PSEMU_RAM_CENTURY_SCREEN] = century;
    ps->bus.ram[PSEMU_RAM_CENTURY_SWI] = century;
    return 1;
}

/* FNV-1a over the whole 16KB BIOS image. Any revision whose RAM layout has
   actually been traced goes in this table. A hash rather than a signature
   scan because it fails closed: an unknown or modified image simply turns
   the feature off, instead of writing to addresses that mean something else
   there. Add entries only after re-verifying the addresses above against
   that revision. */
static const uint32_t known_bios_hashes[] = {
    0xB2E46838u, /* J110 (retail, docs/hardware-notes.md "BIOS/kernel revisions") */
};

int psemu_settings_offsets_known(const psemu_t *ps) {
    if (!ps->has_bios) {
        return 0;
    }
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < PSEMU_BIOS_SIZE; i++) {
        hash ^= ps->bus.bios[i];
        hash *= 16777619u;
    }
    for (size_t i = 0; i < sizeof(known_bios_hashes) / sizeof(known_bios_hashes[0]); i++) {
        if (hash == known_bios_hashes[i]) {
            return 1;
        }
    }
    return 0;
}

size_t psemu_state_size(const psemu_t *ps) {
    (void)ps;
    return sizeof(psemu_t);
}

psemu_status psemu_save_state(const psemu_t *ps, void *buf, size_t size) {
    if (size < sizeof(psemu_t)) {
        return PSEMU_ERR_BAD_SIZE;
    }
    memcpy(buf, ps, sizeof(psemu_t));
    return PSEMU_OK;
}

psemu_status psemu_load_state(psemu_t *ps, const void *buf, size_t size) {
    if (size < sizeof(psemu_t)) {
        return PSEMU_ERR_BAD_SIZE;
    }
    memcpy(ps, buf, sizeof(psemu_t));
    /* bus and cpu hold self-referential pointers into this struct.
       The raw copy above carries over stale addresses from whatever psemu_t the state
       was saved from.
       These pointers must be re-linked to this instance. */
    ps->bus.lcd = &ps->lcd;
    ps->bus.intc = &ps->intc;
    ps->bus.flash = &ps->flash;
    ps->bus.ir = &ps->ir;
    ps->bus.timer = &ps->timer;
    ps->bus.rtc = &ps->rtc;
    ps->bus.dac = &ps->dac;
    ps->bus.clk = &ps->clk;
    ps->bus.iop = &ps->iop;
    ps->cpu.bus = &ps->bus;
    return PSEMU_OK;
}

uint32_t psemu_get_audio_samples(psemu_t *ps, int16_t *buf, uint32_t max_samples) {
    return dac_read_samples(&ps->dac, buf, max_samples);
}
