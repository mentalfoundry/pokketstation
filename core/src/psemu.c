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
    psemu_bus_init(&ps->bus, &ps->lcd, &ps->intc, &ps->flash, &ps->ir, &ps->timer, &ps->rtc);
    arm7tdmi_init(&ps->cpu, &ps->bus);
    ps->buttons = 0;
    ps->has_bios = 0;
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
        }
    }
    ps->buttons = buttons;
}

uint32_t psemu_run(psemu_t *ps, uint32_t cycles) {
    if (!ps->has_bios) {
        return 0;
    }
    uint32_t ran = 0;
    while (ran < cycles) {
        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, step_cycles);
        ran += step_cycles;
    }
    return ran;
}

const uint8_t *psemu_get_framebuffer(const psemu_t *ps) {
    return ps->lcd.vram;
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
    ps->cpu.bus = &ps->bus;
    return PSEMU_OK;
}
