#ifndef PSEMU_FLASH_H
#define PSEMU_FLASH_H

#include <stddef.h>
#include <stdint.h>

#include "psemu/psemu.h"

typedef struct flash {
    uint8_t data[PSEMU_FLASH_SIZE];
} flash_t;

void flash_init(flash_t *flash);

/* Validates and loads a PSX Title Sector app image (see docs/hardware-notes.md). */
psemu_status flash_load_app(flash_t *flash, const uint8_t *data, size_t size);

uint8_t flash_read8(flash_t *flash, uint32_t addr);

#endif
