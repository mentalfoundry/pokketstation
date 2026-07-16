#include "psemu_internal.h"

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
    return ps;
}

void psemu_destroy(psemu_t *ps) {
    free(ps);
}

void psemu_reset(psemu_t *ps) {
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

psemu_status psemu_load_flash_image(psemu_t *ps, const uint8_t *data, size_t size) {
    if (size > sizeof(ps->flash.data)) {
        return PSEMU_ERR_BAD_SIZE;
    }
    memset(ps->flash.data, 0, sizeof(ps->flash.data));
    memcpy(ps->flash.data, data, size);
    return PSEMU_OK;
}

void psemu_set_buttons(psemu_t *ps, uint32_t buttons) {
    /* Real hardware asserts a button's interrupt line on every press/release
       edge (see docs/hardware-notes.md), not as a polled level - translate
       our own PSEMU_BUTTON_* bits (an emulator-side convention, not real
       hardware's bit layout) to the real INT_BTN_* bits per source. */
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
            /* Still held, no fresh edge this call - HOLD should only
               pulse on the press edge, not stay latched as a sustained
               level for the whole physical hold duration. Confirmed via
               a real-hardware discrepancy: the generic system-tick
               callback branches on `hold & INT_BTN_ACTION` *before* its
               RTC check, so a continuously-set hold bit permanently
               skips the RTC-driven redraw path for as long as the
               button is held - but real hardware keeps redrawing/
               blinking normally while held, only acting on release.
               STATUS (unaffected here) keeps tracking the live level for
               any code that polls it directly. */
            intc_clear_hold_only(&ps->intc, button_map[i].int_bit);
        }
    }
    ps->buttons = buttons;
}

uint32_t psemu_run(psemu_t *ps, uint32_t cycles) {
    if (!ps->has_bios) {
        return 0;
    }
    /* `cycles` is a time budget expressed at the reference clock rate
       PSEMU_ASSUMED_CPU_HZ (see dac.h). This function's history is worth
       knowing (see docs/hardware-notes.md for the full story) - it has
       gone back and forth on whether Timer should track CLK_MODE or be
       pinned to real time, and landed here:

       Timer follows raw, CLK_MODE-scaled step_cycles - real timers are
       clocked by the System Clock (i.e. genuinely tied to the CPU's
       variable clock, not an independent oscillator) and via direct
       measurement: with Timer
       pinned to a fixed reference rate instead, the HELLO animation
       (driven by the same Timer1 heartbeat that drives audio - both are
       confirmed GUI-code uses of the same IRQ) ran ~4x too
       slow during CLK_MODE=7, and the date-setting screen's blink ran
       ~2x too fast during CLK_MODE=4 - both errors matching the ratio
       between CLK_MODE=7/4's real Hz and the fixed reference rate
       almost exactly (3.97x and 2.01x respectively), confirming Timer's
       rate needs to track CLK_MODE for real, not be decoupled from it.

       RTC and DAC remain pinned to real elapsed time regardless of
       CLK_MODE, for different reasons: RTC is a genuinely separate,
       CPU-clock-independent oscillator (confirmed via real hardware's
       RTC ticking at a flat real 1Hz, unrelated to its
       CPU-frequency setting), and this emulator's
       DAC resampling needs a fixed real-time OUTPUT rate to feed a
       standard audio API, regardless of how often the app actually
       writes new DACV content (which, via Timer, does still track
       CLK_MODE - that's the audio content/pitch itself, correctly
       varying with CLK_MODE same as real hardware). A fractional carry
       (ps->real_time_cycle_carry) converts each step's real elapsed time
       (via the *currently active* clk_current_hz()) back into the fixed
       PSEMU_ASSUMED_CPU_HZ reference currency RTC/DAC assume, preserving
       real time exactly across steps despite integer truncation - the
       same accumulator pattern dac_tick already uses internally. */
    double budget_seconds = (double)cycles / (double)PSEMU_ASSUMED_CPU_HZ;
    double elapsed_seconds = 0.0;
    uint32_t ran = 0;
    while (elapsed_seconds < budget_seconds) {
        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        double dt = (double)step_cycles / (double)clk_current_hz(&ps->clk);
        elapsed_seconds += dt;

        ps->real_time_cycle_carry += dt * (double)PSEMU_ASSUMED_CPU_HZ;
        uint32_t real_time_cycles = (uint32_t)ps->real_time_cycle_carry;
        ps->real_time_cycle_carry -= (double)real_time_cycles;

        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, real_time_cycles);
        dac_tick(&ps->dac, real_time_cycles);
        ran += step_cycles;
    }
    return ran;
}

const uint8_t *psemu_get_framebuffer(const psemu_t *ps) {
    return ps->lcd.presented;
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

    /* FLASH1 (0x02000000+) is a live virtual window resolved against
       F_BANK_FLG/F_BANK_VAL, not a fixed offset into FLASH2 (see
       docs/hardware-notes.md) - without this, any FLASH1 address in the
       trace/registers below can't be reliably translated back to a
       physical byte offset for static analysis after the fact. */
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
        /* r[15] has already been advanced past the faulting instruction
           (arm7tdmi_step does this before dispatch) - undo that to find
           where it was actually fetched from, then mask to the natural
           alignment for this mode (matching tools/inspect.c's own crash
           reporter, which originally got this wrong - see
           docs/hardware-notes.md). */
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
    /* bus/cpu hold self-referential pointers into this struct; the raw
       copy above carries over stale addresses from whatever psemu_t the
       state was saved from, so they must be re-linked to this instance. */
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
