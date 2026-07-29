#ifndef PSEMU_FLASH_H
#define PSEMU_FLASH_H

#include <stddef.h>
#include <stdint.h>

#include "psemu/psemu.h"

#define FLASH_BLOCK_SIZE 8192u
/* FLASH_CTRL register map:
 * +0x0  command/status.
 * +0x4  unused, mirrors the command register.
 * +0x8  F_BANK_FLG, a bitmask of enabled physical 8KB blocks.
 * +0xC  F_WAIT1 (waitstates).
 * +0x10 F_WAIT2 (waitstates and flash-write control/status).
 *
 * Bug, fixed: a real BIOS flash-write routine polls +0x10 and waits for
 * bit 2 to read back set. This register was not modeled before this fix;
 * the span stopped at +0xC. An unmapped read returned 0, so the poll loop
 * spun forever. This was the second of two busy-wait bugs that blocked
 * every real app launch this session reached. The first bug was in
 * flash_ctrl_read8's +0 command readback; see that function's comment.
 * This emulator does not model real flash write timing, so writes
 * complete instantly. Because of this, +0x10 always reads back
 * "not busy".
 *
 * The span extends to 0x140 to also cover F_BANK_VAL (+0x100 to +0x13C,
 * 16 words). F_BANK_FLG marks which physical 8KB blocks are enabled.
 * F_BANK_VAL maps each physical block to a virtual bank slot (0-15):
 * table[physical] = virtual. This is the reverse direction from a
 * typical page table. An earlier version of this file assumed a simple
 * linear offset instead; that assumption was wrong. The gap between
 * +0x14 and +0xFF is unmapped. Reads in this gap fall through to the
 * last_command mirror default.
 */
/* F_EXTRA (0x300-0x3FF, 256 bytes) is a separate "header" region beyond
 * the ordinary FLASH_CTRL registers, per official register documentation.
 * It holds F_SN_LO/
 * F_SN_HI (the 32-bit hardware serial number, read/written by SWI 0Ah/
 * 0Fh's FlashReadSerial/FlashWriteSerial) and F_CAL (LCD calibration).
 * Real hardware rewrites F_CAL alongside F_SN whenever FlashWriteSerial
 * runs. The span extends from 0x140 to 0x400 to cover F_EXTRA. See
 * flash_ctrl_read8/write8 for why the gap between the two (0x140-0x2FF,
 * unmapped and unidentified) still reads back 0, instead of falling into
 * the F_BANK_VAL or command-mirror logic.
 */
#define FLASH_CTRL_SPAN 0x400u
#define FLASH_BANK_VAL_OFFSET 0x100u
#define FLASH_BANK_VAL_COUNT 16u
#define FLASH_EXTRA_OFFSET 0x300u
#define FLASH_EXTRA_SPAN 0x100u
#define FLASH_SN_LO_OFFSET 0x300u
#define FLASH_SN_HI_OFFSET 0x302u
#define FLASH_CAL_OFFSET 0x308u

/* Default hardware ID (F_SN).
 *
 * Chocobo World (Final Fantasy VIII) reads the PocketStation's serial
 * number via SWI 0Ah when a player creates a new save or Chocobo.
 * Confirmed by disassembling a real copy of Chocobo World (see
 * docs/hardware-notes.md, "Hardware ID (F_SN)"). The game masks F_SN
 * down to its low 24 bits, converts that value to a decimal digit
 * string, and uses the last 3 digits as the save's initial "ID" stat.
 * This ID stat alone determines rank. The community-documented best
 * rank is ID 211 (max HP and weapon value, best item-drop odds); 211 is
 * also the day and month of FF8's Japanese release (2/11).
 *
 * F_SN's high byte is masked off before this calculation, so it does not
 * affect rank. On some real units, the high byte holds an ASCII letter.
 * That letter appears as part of a "letter + 8 decimal digits" serial
 * sticker, for example "A02374684". The raw register itself has no such
 * structural requirement (see psemu_parse_hardware_id/
 * psemu_format_hardware_id, core/src/psemu.c).
 *
 * This emulator defaults F_SN to 0x410000D3 ("410000D3" in this
 * emulator's own hex string form). The low 24 bits end in "211", so
 * every fresh Chocobo World save gets the best rank by default. The
 * high byte is an arbitrary letter, 'A', chosen to look like a real
 * serial.
 */
#define FLASH_DEFAULT_SERIAL (((uint32_t)'A' << 24) | 211u)

typedef struct flash {
    uint8_t data[PSEMU_FLASH_SIZE];
    uint32_t bank_mask;    /* last value written to FLASH_CTRL+8 (F_BANK_FLG) */
    uint32_t last_command; /* last value written to FLASH_CTRL+0 */
    uint32_t bank_val[FLASH_BANK_VAL_COUNT]; /* F_BANK_VAL, indexed by physical bank */
    uint16_t f_sn_lo;                        /* F_SN_LO: hardware serial number LSBs */
    uint16_t f_sn_hi;                        /* F_SN_HI: hardware serial number MSBs */
    uint16_t f_cal;                          /* F_CAL: LCD calibration, rewritten as-is by FlashWriteSerial */
    /* Tracks progress through the real F_KEY2/F_KEY1/F_KEY2 flash-unlock
       sequence. 0 = not started, 3 = fully armed. See flash_write8's
       comment for details. When armed, a write to physical offset 0/2/8
       updates F_SN_LO/F_SN_HI/F_CAL instead of ordinary card data. */
    uint8_t unlock_step;
} flash_t;

/* Combines F_SN_LO/F_SN_HI the same way SWI 0Ah (FlashReadSerial) does. */
uint32_t flash_get_serial(const flash_t *flash);
/* Splits serial into F_SN_LO/F_SN_HI the same way SWI 0Fh
   (FlashWriteSerial) does. Leaves F_CAL untouched. Real hardware
   rewrites F_CAL's old value here; there is nothing else to rewrite it
   to. */
void flash_set_serial(flash_t *flash, uint32_t serial);

void flash_init(flash_t *flash);

/* Resets only the volatile FLASH_CTRL register state (bank_mask,
   last_command, bank_val[], unlock_step) to power-on defaults. Deliberately
   preserves data[] (loaded card/app content) and f_sn_lo/f_sn_hi/f_cal
   (hardware ID and LCD calibration) - these are flash-backed content, not
   volatile registers, and must survive a reset the same way real flash
   content and a factory-programmed serial would. Used by psemu_reset. */
void flash_reset_registers(flash_t *flash);

/* Validates a PSX Title Sector app image. Loads it into a synthesized
   one-entry memory-card directory at slot 1. This lets the real BIOS's
   app-selection/dispatch routine reach it (see docs/app-notes.md,
   "App-selection and dispatch"). `size` is capped at 15 blocks
   (DIRECTORY_MAX_APP_BLOCKS), not the full 16, because block 0 is
   reserved for the synthesized directory. */
psemu_status flash_load_app(flash_t *flash, const uint8_t *data, size_t size);

/* FLASH2: physical flash, unwindowed. */
uint8_t flash_read8(flash_t *flash, uint32_t addr);
void flash_write8(flash_t *flash, uint32_t addr, uint8_t value);

/* FLASH1: virtual window onto FLASH2. Each 8KB virtual bank (0-15) is
   resolved live against F_BANK_FLG/F_BANK_VAL (see docs/app-notes.md,
   "App-selection and dispatch"). If F_BANK_VAL has not been explicitly
   configured for a bank, resolution falls back to a contiguous linear
   offset from the lowest enabled physical block. */
uint8_t flash1_read8(flash_t *flash, uint32_t addr);
void flash1_write8(flash_t *flash, uint32_t addr, uint8_t value);

/* FLASH_CTRL: bank-select registers, reverse-engineered from a real BIOS.
   +8 (F_BANK_FLG) is a bitmask of the app's physical blocks. +0 is a
   commit/activate trigger. +0x100.. (F_BANK_VAL) assigns a virtual slot
   to each physical block. */
uint8_t flash_ctrl_read8(flash_t *flash, uint32_t offset);
void flash_ctrl_write8(flash_t *flash, uint32_t offset, uint8_t value);

#endif
