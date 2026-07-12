#ifndef PSEMU_FLASH_H
#define PSEMU_FLASH_H

#include <stddef.h>
#include <stdint.h>

#include "psemu/psemu.h"

#define FLASH_BLOCK_SIZE 8192u
/* +0 command/status, +4 unused/mirror, +8 bank bitmask, +0xC F_WAIT1,
   +0x10 F_WAIT2 ("waitstates, and FLASH-Write-Control-and-
   Status") - a real, confirmed bug: a real BIOS flash-write routine
   polls +0x10 waiting for bit 2 to read back set, and this register
   wasn't modeled at all (span stopped at 0xC), so a default/unmapped
   read of 0 left that loop spinning forever - the second of two
   busy-wait bugs blocking every real app launch this session traced
   far enough to reach (the first being flash_ctrl_read8's +0 command
   readback, see there). Since this emulator doesn't model real flash
   write timing at all (writes complete instantly), +0x10 always reads
   back "not busy". */
#define FLASH_CTRL_SPAN 0x14u

typedef struct flash {
    uint8_t data[PSEMU_FLASH_SIZE];
    uint32_t bank_mask;    /* last value written to FLASH_CTRL+8 */
    uint32_t last_command; /* last value written to FLASH_CTRL+0 */
    uint32_t bank_offset;  /* FLASH1 window offset into data[], recomputed on commit */
} flash_t;

void flash_init(flash_t *flash);

/* Validates and loads a PSX Title Sector app image (see docs/hardware-notes.md). */
psemu_status flash_load_app(flash_t *flash, const uint8_t *data, size_t size);

/* FLASH2: physical flash, unwindowed. */
uint8_t flash_read8(flash_t *flash, uint32_t addr);
void flash_write8(flash_t *flash, uint32_t addr, uint8_t value);

/* FLASH1: virtual window onto FLASH2, offset by the active bank selection
   (see docs/hardware-notes.md, "App-selection and dispatch"). */
uint8_t flash1_read8(flash_t *flash, uint32_t addr);
void flash1_write8(flash_t *flash, uint32_t addr, uint8_t value);

/* FLASH_CTRL: bank-select registers, reverse-engineered from a real BIOS -
   +8 is a bitmask of the app's blocks, +0 is a commit/activate trigger. */
uint8_t flash_ctrl_read8(flash_t *flash, uint32_t offset);
void flash_ctrl_write8(flash_t *flash, uint32_t offset, uint8_t value);

#endif
