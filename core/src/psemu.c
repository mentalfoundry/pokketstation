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
    io_init(&ps->io);
    flash_init(&ps->flash);
    ir_init(&ps->ir);
    psemu_bus_init(&ps->bus, &ps->lcd, &ps->io, &ps->flash, &ps->ir);
    arm7tdmi_init(&ps->cpu, &ps->bus);
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
    io_set_buttons(&ps->io, buttons);
}

uint32_t psemu_run(psemu_t *ps, uint32_t cycles) {
    if (!ps->has_bios) {
        return 0;
    }
    uint32_t ran = 0;
    while (ran < cycles) {
        ran += arm7tdmi_step(&ps->cpu);
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
    ps->bus.io = &ps->io;
    ps->bus.flash = &ps->flash;
    ps->bus.ir = &ps->ir;
    ps->cpu.bus = &ps->bus;
    return PSEMU_OK;
}
