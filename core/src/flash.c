#include "flash.h"

#include <string.h>

#define TITLE_SECTOR_HEADER_SIZE 0x80u
#define TITLE_SECTOR_MAGIC_OFFSET 0x52u

void flash_init(flash_t *flash) {
    memset(flash->data, 0, sizeof(flash->data));
    flash->bank_mask = 0;
    flash->last_command = 0;
    flash->bank_offset = 0;
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

uint8_t flash1_read8(flash_t *flash, uint32_t addr) {
    return flash->data[(flash->bank_offset + addr) % PSEMU_FLASH_SIZE];
}

void flash1_write8(flash_t *flash, uint32_t addr, uint8_t value) {
    flash->data[(flash->bank_offset + addr) % PSEMU_FLASH_SIZE] = value;
}

static void flash_commit_bank(flash_t *flash) {
    if (flash->bank_mask == 0) {
        flash->bank_offset = 0;
        return;
    }
    uint32_t first_block = 0;
    for (uint32_t i = 0; i < 32; i++) {
        if (flash->bank_mask & (1u << i)) {
            first_block = i;
            break;
        }
    }
    flash->bank_offset = first_block * FLASH_BLOCK_SIZE;
}

uint8_t flash_ctrl_read8(flash_t *flash, uint32_t offset) {
    uint32_t word_index = offset / 4u;
    uint32_t reg;
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
    uint32_t reg_index = offset / 4u;
    uint32_t shift = (offset % 4u) * 8u;

    if (reg_index == 2u) { /* +8: block bitmask */
        flash->bank_mask = (flash->bank_mask & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        return;
    }
    if (reg_index == 0u) { /* +0: commit/activate trigger */
        flash->last_command = (flash->last_command & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        /* The real BIOS always writes the bitmask before the command, so the
           mask is already complete by the time any byte of the command
           register is written - safe to recompute on every such write. */
        flash_commit_bank(flash);
        return;
    }
}
