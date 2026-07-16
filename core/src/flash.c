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

/* F_KEY1 (0x08002A54) and F_KEY2 (0x080055AA) are real hardware's flash
   unlock-sequence trigger addresses (these addresses unlock FLASH memory for
   writing - a real BIOS write routine writes FFAAh/FF55h/FFA0h to these
   in sequence before actually writing sector data, the standard
   command-latch idiom NOR flash chips use). They are NOT storage
   locations - real flash hardware intercepts writes to these specific
   addresses as unlock commands rather than passing them through to the
   data array, so the byte physically "at" that address is unaffected. A
   REAL, CONFIRMED BUG found via a real crash report (see
   docs/hardware-notes.md, "Chocobo World event-screen crash"): this
   emulator had no such interception, so every real flash-write
   operation permanently corrupted a live data byte at whichever
   physical offset happened to numerically coincide with these two fixed
   addresses - which happens to be live app code partway through
   Chocobo World's own compiled binary, turning an ordinary in-game save
   into silent code corruption that only surfaces later, once execution
   reaches the mangled bytes. */
#define FLASH_KEY1_OFFSET 0x2A54u
#define FLASH_KEY2_OFFSET 0x55AAu

/* Both keys are written as a 16-bit halfword on real hardware
   ([8002A54h]=FF55h), so both bytes of each halfword need
   guarding, not just the base offset - a 32-bit store, for example,
   still touches the two bytes past the base address. */
static int flash_is_unlock_key_offset(uint32_t offset) {
    return offset == FLASH_KEY1_OFFSET || offset == FLASH_KEY1_OFFSET + 1u || offset == FLASH_KEY2_OFFSET ||
           offset == FLASH_KEY2_OFFSET + 1u;
}

uint8_t flash_read8(flash_t *flash, uint32_t addr) {
    return flash->data[addr % PSEMU_FLASH_SIZE];
}

void flash_write8(flash_t *flash, uint32_t addr, uint8_t value) {
    uint32_t offset = addr % PSEMU_FLASH_SIZE;
    if (flash_is_unlock_key_offset(offset)) {
        return;
    }
    flash->data[offset] = value;
}

/* Resolves which physical 8KB block backs a given FLASH1 virtual bank
   (0-15). F_BANK_VAL is indexed
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
       value is 0 for every physical bank, matching real hardware's reset
       state for this register) - fall back to this emulator's previously-validated
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
    uint32_t offset = (physical_bank * FLASH_BLOCK_SIZE + offset_in_bank) % PSEMU_FLASH_SIZE;
    /* F_KEY1/F_KEY2 unlock addresses are a real hardware chip decode, not
       data storage - see flash_write8's comment. Applies regardless of
       which bus window (this virtual one, or FLASH2 direct) reaches the
       same physical offset. */
    if (flash_is_unlock_key_offset(offset)) {
        return;
    }
    flash->data[offset] = value;
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
