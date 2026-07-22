#ifndef PSEMU_FLASH_H
#define PSEMU_FLASH_H

#include <stddef.h>
#include <stdint.h>

#include "psemu/psemu.h"

#define FLASH_BLOCK_SIZE 8192u
/* +0 command/status, +4 unused/mirror, +8 bank bitmask (F_BANK_FLG),
   +0xC F_WAIT1, +0x10 F_WAIT2 (waitstates, and FLASH-Write-
   Control-and-Status) - a real, confirmed bug: a real BIOS flash-write
   routine polls +0x10 waiting for bit 2 to read back set, and this
   register wasn't modeled at all (span stopped at 0xC), so a
   default/unmapped read of 0 left that loop spinning forever - the
   second of two busy-wait bugs blocking every real app launch this
   session traced far enough to reach (the first being flash_ctrl_read8's
   +0 command readback, see there). Since this emulator doesn't model
   real flash write timing at all (writes complete instantly), +0x10
   always reads back "not busy". Span extended to 0x140 to also cover
   F_BANK_VAL (+0x100..+0x13C, 16 words) -
   F_BANK_FLG says which physical 8KB blocks are
   enabled, F_BANK_VAL says, per PHYSICAL block, which virtual bank slot
   (0-15) it appears at (table[p]=v, deliberately the "backwards"
   direction from a typical page table) - not a simple linear offset as
   an earlier version of this file assumed. The gap between +0x14 and
   +0xFF is unmapped/unknown and keeps falling through to the existing
   default (last_command mirror). */
#define FLASH_CTRL_SPAN 0x140u
#define FLASH_BANK_VAL_OFFSET 0x100u
#define FLASH_BANK_VAL_COUNT 16u

typedef struct flash {
    uint8_t data[PSEMU_FLASH_SIZE];
    uint32_t bank_mask;    /* last value written to FLASH_CTRL+8 (F_BANK_FLG) */
    uint32_t last_command; /* last value written to FLASH_CTRL+0 */
    uint32_t bank_val[FLASH_BANK_VAL_COUNT]; /* F_BANK_VAL, indexed by physical bank */
} flash_t;

void flash_init(flash_t *flash);

/* Validates a PSX Title Sector app image and loads it into a synthesized
   one-entry memory-card directory at slot 1, so the real BIOS's
   app-selection/dispatch routine can actually reach it (see
   docs/hardware-notes.md, "App-selection and dispatch"). `size` is capped
   at 15 blocks (DIRECTORY_MAX_APP_BLOCKS), not the full 16, since block 0
   is reserved for the synthesized directory. */
psemu_status flash_load_app(flash_t *flash, const uint8_t *data, size_t size);

/* FLASH2: physical flash, unwindowed. */
uint8_t flash_read8(flash_t *flash, uint32_t addr);
void flash_write8(flash_t *flash, uint32_t addr, uint8_t value);

/* FLASH1: virtual window onto FLASH2. Each 8KB virtual bank (0-15) is
   resolved live against F_BANK_FLG/F_BANK_VAL (see docs/hardware-notes.md,
   "App-selection and dispatch") - falls back to a contiguous linear
   offset from the lowest enabled physical block when F_BANK_VAL hasn't
   been explicitly configured for a given bank. */
uint8_t flash1_read8(flash_t *flash, uint32_t addr);
void flash1_write8(flash_t *flash, uint32_t addr, uint8_t value);

/* FLASH_CTRL: bank-select registers, reverse-engineered from a real BIOS -
   +8 (F_BANK_FLG) is a bitmask of the app's physical blocks, +0 is a
   commit/activate trigger, +0x100.. (F_BANK_VAL) is the per-physical-block
   virtual-slot assignment. */
uint8_t flash_ctrl_read8(flash_t *flash, uint32_t offset);
void flash_ctrl_write8(flash_t *flash, uint32_t offset, uint8_t value);

#endif
