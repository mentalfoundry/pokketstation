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
/* F_EXTRA (0x300-0x3FF, 256 bytes): a separate "header" region beyond the
   ordinary FLASH_CTRL registers, per psx-spx - holds F_SN_LO/F_SN_HI (the
   32-bit hardware serial number, SWI 0Ah/0Fh's FlashReadSerial/
   FlashWriteSerial) and F_CAL (LCD calibration, silently rewritten
   alongside F_SN by FlashWriteSerial on real hardware). Span extended from
   0x140 to 0x400 to cover it; see flash_ctrl_read8/write8 for why the gap
   between the two (0x140-0x2FF, genuinely unmapped/unknown) still reads
   back 0 rather than falling into F_BANK_VAL/command-mirror logic. */
#define FLASH_CTRL_SPAN 0x400u
#define FLASH_BANK_VAL_OFFSET 0x100u
#define FLASH_BANK_VAL_COUNT 16u
#define FLASH_EXTRA_OFFSET 0x300u
#define FLASH_EXTRA_SPAN 0x100u
#define FLASH_SN_LO_OFFSET 0x300u
#define FLASH_SN_HI_OFFSET 0x302u
#define FLASH_CAL_OFFSET 0x308u

/* Default hardware ID (F_SN): the PocketStation's serial number is read by
   Chocobo World (Final Fantasy VIII) via SWI 0Ah when a new save/Chocobo is
   created - confirmed by disassembling a real copy of Chocobo World (see
   docs/hardware-notes.md, "Hardware ID (F_SN)"): it masks F_SN down to its
   low 24 bits, converts that to a decimal digit string, and uses the LAST
   3 digits as the save's initial "ID" stat, which alone determines rank -
   the community-documented best rank (max HP/weapon value, best item-drop
   odds) is ID 211 (also the day/month of FF8's Japanese release, 2/11).
   The high byte of F_SN (masked off before that calculation, so
   irrelevant to rank) holds an ASCII letter on some real units, printed as
   part of a "letter + 8 decimal digits" serial sticker (e.g. "A02374684") -
   but the raw register has no such structural requirement (see
   psemu_parse_hardware_id/psemu_format_hardware_id, core/src/psemu.c).
   Defaults to 0x410000D3 ("410000D3" in the emulator's own hex string
   form): low 24 bits trivially end in "211" (giving every fresh Chocobo
   World save the best rank out of the box), high byte an arbitrary but
   plausible-looking 'A'. */
#define FLASH_DEFAULT_SERIAL (((uint32_t)'A' << 24) | 211u)

typedef struct flash {
    uint8_t data[PSEMU_FLASH_SIZE];
    uint32_t bank_mask;    /* last value written to FLASH_CTRL+8 (F_BANK_FLG) */
    uint32_t last_command; /* last value written to FLASH_CTRL+0 */
    uint32_t bank_val[FLASH_BANK_VAL_COUNT]; /* F_BANK_VAL, indexed by physical bank */
    uint16_t f_sn_lo;                        /* F_SN_LO: hardware serial number LSBs */
    uint16_t f_sn_hi;                        /* F_SN_HI: hardware serial number MSBs */
    uint16_t f_cal;                          /* F_CAL: LCD calibration, rewritten as-is by FlashWriteSerial */
    /* Progress through the real F_KEY2/F_KEY1/F_KEY2 flash-unlock sequence
       (0 = none, 3 = fully armed) - see flash_write8's comment. Armed
       state is what makes a write to physical offset 0/2/8 update
       F_SN_LO/F_SN_HI/F_CAL instead of ordinary card data. */
    uint8_t unlock_step;
} flash_t;

/* Combines F_SN_LO/F_SN_HI the same way SWI 0Ah (FlashReadSerial) does. */
uint32_t flash_get_serial(const flash_t *flash);
/* Splits into F_SN_LO/F_SN_HI the same way SWI 0Fh (FlashWriteSerial) does;
   leaves F_CAL untouched (matching real hardware's "rewrites the old value"
   behavior - there's nothing else to rewrite it *to* here). */
void flash_set_serial(flash_t *flash, uint32_t serial);

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
