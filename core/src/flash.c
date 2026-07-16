#include "flash.h"

#include <string.h>

#define TITLE_SECTOR_HEADER_SIZE 0x80u
#define TITLE_SECTOR_MAGIC_OFFSET 0x52u

void flash_init(flash_t *flash) {
    memset(flash->data, 0, sizeof(flash->data));
    flash->bank_mask = 0;
    flash->last_command = 0;
    memset(flash->bank_val, 0, sizeof(flash->bank_val));
}

psemu_status flash_load_app(flash_t *flash, const uint8_t *data, size_t size) {
    if (size < TITLE_SECTOR_HEADER_SIZE || size > sizeof(flash->data)) {
        return PSEMU_ERR_BAD_SIZE;
    }
    if (memcmp(&data[TITLE_SECTOR_MAGIC_OFFSET], "MCX0", 4) != 0 &&
        memcmp(&data[TITLE_SECTOR_MAGIC_OFFSET], "MCX1", 4) != 0) {
        return PSEMU_ERR_BAD_FORMAT;
    }
    memset(flash->data, 0, sizeof(flash->data));
    memcpy(flash->data, data, size);
    return PSEMU_OK;
}

uint8_t flash_read8(flash_t *flash, uint32_t addr) {
    return flash->data[addr % PSEMU_FLASH_SIZE];
}

void flash_write8(flash_t *flash, uint32_t addr, uint8_t value) {
    flash->data[addr % PSEMU_FLASH_SIZE] = value;
}

/* Resolves which physical 8KB block backs a given FLASH1 virtual bank
   (0-15). Confirmed via the documentation: F_BANK_VAL is indexed
   by PHYSICAL bank (table[p]=v - deliberately the "backwards" direction
   from a typical page table), so resolving virtual->physical requires a
   reverse linear search over the 16 entries. */
static uint32_t flash_resolve_physical_bank(const flash_t *flash, uint32_t virtual_bank) {
    uint32_t p;
    uint32_t lowest = 0;

    for (p = 0; p < FLASH_BANK_VAL_COUNT; p++) {
        if ((flash->bank_mask & (1u << p)) && (flash->bank_val[p] & 0xFu) == virtual_bank) {
            return p;
        }
    }
    /* No F_BANK_VAL entry explicitly claims this virtual slot (its reset
       value is 0 for every physical bank, matching the officially documented
       reset state) - fall back to this emulator's previously-validated
       behavior: treat the enabled physical blocks as one contiguous run
       starting at the lowest-numbered enabled block. Confirmed via real
       disassembly that this emulator's app-selection routine (see
       "App-selection and dispatch" in docs/hardware-notes.md) only ever
       writes F_BANK_FLG, never F_BANK_VAL - so this fallback is what
       keeps ordinary multi-block app dispatch/execution working exactly
       as before for every case already tested this session. */
    for (p = 0; p < FLASH_BANK_VAL_COUNT; p++) {
        if (flash->bank_mask & (1u << p)) {
            lowest = p;
            break;
        }
    }
    return lowest + virtual_bank;
}

uint8_t flash1_read8(flash_t *flash, uint32_t addr) {
    uint32_t virtual_bank = (addr / FLASH_BLOCK_SIZE) % FLASH_BANK_VAL_COUNT;
    uint32_t offset_in_bank = addr % FLASH_BLOCK_SIZE;
    uint32_t physical_bank = flash_resolve_physical_bank(flash, virtual_bank);
    return flash->data[(physical_bank * FLASH_BLOCK_SIZE + offset_in_bank) % PSEMU_FLASH_SIZE];
}

void flash1_write8(flash_t *flash, uint32_t addr, uint8_t value) {
    uint32_t virtual_bank = (addr / FLASH_BLOCK_SIZE) % FLASH_BANK_VAL_COUNT;
    uint32_t offset_in_bank = addr % FLASH_BLOCK_SIZE;
    uint32_t physical_bank = flash_resolve_physical_bank(flash, virtual_bank);
    flash->data[(physical_bank * FLASH_BLOCK_SIZE + offset_in_bank) % PSEMU_FLASH_SIZE] = value;
}

uint8_t flash_ctrl_read8(flash_t *flash, uint32_t offset) {
    uint32_t word_index;
    uint32_t reg;

    if (offset >= FLASH_BANK_VAL_OFFSET && offset < FLASH_BANK_VAL_OFFSET + FLASH_BANK_VAL_COUNT * 4u) {
        uint32_t bank_index = (offset - FLASH_BANK_VAL_OFFSET) / 4u;
        return (uint8_t)(flash->bank_val[bank_index] >> ((offset % 4u) * 8u));
    }

    word_index = offset / 4u;
    if (word_index == 2u) {
        reg = flash->bank_mask;
    } else if (word_index == 0u) {
        /* +0 is write-command/read-status on real hardware, not a plain
           mirror - a real, confirmed bug: a real BIOS routine writes a
           command here then busy-waits on this same address's bit 0
           reading back 1 ("ready"). Our bank commit always completes
           synchronously, so bit 0 is always ready immediately after any
           write - echoing back the raw command value (whose bit 0
           happened to be 0 for the observed command, 2) left that loop
           spinning forever, silently blocking every real app launch
           this session traced this far into the real BIOS. */
        reg = flash->last_command | 1u;
    } else if (word_index == 4u) {
        /* +0x10 (F_WAIT2): a second, confirmed busy-wait bug - a real
           app's own flash-write routine polls this bit 2, expecting it
           to read back set once the write completes. Not modeled at
           all before (span stopped at +0xC), so a default/unmapped read
           of 0 left this spinning forever too, immediately after fixing
           the +0 bug above. Since writes complete instantly here,
           always report "not busy". */
        reg = 0x04u;
    } else {
        reg = flash->last_command;
    }
    return (uint8_t)(reg >> ((offset % 4u) * 8u));
}

void flash_ctrl_write8(flash_t *flash, uint32_t offset, uint8_t value) {
    uint32_t reg_index;
    uint32_t shift;

    if (offset >= FLASH_BANK_VAL_OFFSET && offset < FLASH_BANK_VAL_OFFSET + FLASH_BANK_VAL_COUNT * 4u) {
        uint32_t bank_index = (offset - FLASH_BANK_VAL_OFFSET) / 4u;
        uint32_t bank_shift = (offset % 4u) * 8u;
        flash->bank_val[bank_index] =
            (flash->bank_val[bank_index] & ~(0xFFu << bank_shift)) | ((uint32_t)value << bank_shift);
        return;
    }

    reg_index = offset / 4u;
    shift = (offset % 4u) * 8u;

    if (reg_index == 2u) { /* +8: block bitmask (F_BANK_FLG) */
        flash->bank_mask = (flash->bank_mask & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        return;
    }
    if (reg_index == 0u) { /* +0: commit/activate trigger */
        flash->last_command = (flash->last_command & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        /* No cached offset to recompute anymore - flash1_read8/write8
           resolve F_BANK_FLG/F_BANK_VAL live on every access. */
        return;
    }
}
