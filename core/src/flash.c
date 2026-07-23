#include "flash.h"

#include <string.h>

#define TITLE_SECTOR_HEADER_SIZE 0x80u
#define TITLE_SECTOR_MAGIC_OFFSET 0x52u

/* A real PS1 memory card directory: 16 128-byte frames in block 0 (frame 0
   is the card header, frames 1-15 describe blocks 1-15). Frame layout: byte
   0 in-use marker (0x51 first/solo, 0x52 middle, 0x53 last, 0xA0 free),
   bytes 4-7 total file size (little-endian, first frame of a file only),
   bytes 8-9 next-block link (little-endian, 0-based among the 15 data
   blocks - add 1 for the physical block number, 0xFFFF = end of chain),
   byte 0x7F XOR checksum of the preceding 127 bytes. */
#define DIRECTORY_FRAME_SIZE 128u
#define DIRECTORY_MAX_APP_BLOCKS 15u /* block 0 is reserved for the directory itself */

/* A real, confirmed requirement, isolated by bisecting against a real card
   dump one byte at a time: the BIOS's menu-browsing code (separate from,
   and run before, the app-selection/dispatch routine documented above -
   it's what lets a user navigate to and select an entry at all) requires
   byte 6 of the filename field (frame offset 0x10) to be ASCII 'P'. Two
   real directory entries confirm the pattern - a normal PS1 save ID's
   mandatory region-code hyphen (e.g. "SLUS-00892") is replaced with 'P'
   when that save bundles a PocketStation app ("BASLUSP00892...",
   "BISLPMP86247..." - the latter's product-code prefix `SLPM` already
   contains an unrelated, naturally-occurring 'P', which is what made this
   take two rounds of bisection to isolate from the real marker one byte
   later). Confirmed empirically: a real, working card still dispatches
   correctly with every other filename byte garbage or zeroed, as long as
   this one byte is 'P' - and stops dispatching if it's touched, even with
   an otherwise plausible-looking name in place. There's no real product
   code to put here for an arbitrary loaded app, so the rest of the field
   is left blank; only this one flag byte is load-bearing. */
#define DIRECTORY_POCKETSTATION_FLAG_OFFSET 0x10u

void flash_init(flash_t *flash) {
    memset(flash->data, 0, sizeof(flash->data));
    flash->bank_mask = 0;
    flash->last_command = 0;
    memset(flash->bank_val, 0, sizeof(flash->bank_val));
    flash->f_sn_lo = (uint16_t)(FLASH_DEFAULT_SERIAL & 0xFFFFu);
    flash->f_sn_hi = (uint16_t)((FLASH_DEFAULT_SERIAL >> 16) & 0xFFFFu);
    /* nocash's documented reset default (001Ah) - unrelated to the ID
       feature, just a sane starting value for a register FlashWriteSerial
       rewrites verbatim rather than ever computing. */
    flash->f_cal = 0x001Au;
    flash->unlock_step = 0;
}

uint32_t flash_get_serial(const flash_t *flash) {
    return ((uint32_t)flash->f_sn_hi << 16) | flash->f_sn_lo;
}

void flash_set_serial(flash_t *flash, uint32_t serial) {
    flash->f_sn_lo = (uint16_t)(serial & 0xFFFFu);
    flash->f_sn_hi = (uint16_t)((serial >> 16) & 0xFFFFu);
}

static uint8_t directory_frame_xor(const uint8_t *frame) {
    uint8_t xor_value = 0;
    uint32_t i;
    for (i = 0; i < 0x7Fu; i++) {
        xor_value ^= frame[i];
    }
    return xor_value;
}

psemu_status flash_load_app(flash_t *flash, const uint8_t *data, size_t size) {
    if (size < TITLE_SECTOR_HEADER_SIZE || size > DIRECTORY_MAX_APP_BLOCKS * FLASH_BLOCK_SIZE) {
        return PSEMU_ERR_BAD_SIZE;
    }
    if (memcmp(&data[TITLE_SECTOR_MAGIC_OFFSET], "MCX0", 4) != 0 &&
        memcmp(&data[TITLE_SECTOR_MAGIC_OFFSET], "MCX1", 4) != 0) {
        return PSEMU_ERR_BAD_FORMAT;
    }

    memset(flash->data, 0, sizeof(flash->data));

    /* Real hardware's app-selection routine (docs/hardware-notes.md,
       "App-selection and dispatch") requires FLASH2 to carry a real memory-
       card directory, not just the app's own bytes at offset 0 - it reads
       the selected slot's directory frame, walks its block-chain link, and
       only then locates the entry point at the resulting physical block.
       Synthesize a minimal one-entry card so the loaded app is actually
       reachable that way: a card header, one directory frame per app block
       chained starting at slot 1 (matching "Right from the clock screen
       moves to the first app in the list"), and the app's own data placed
       starting at physical block 1, right after the directory. */
    flash->data[0x00] = 'M';
    flash->data[0x01] = 'C';
    flash->data[0x7F] = directory_frame_xor(flash->data);

    uint32_t block_count = (uint32_t)((size + FLASH_BLOCK_SIZE - 1) / FLASH_BLOCK_SIZE);
    uint32_t block;
    for (block = 1; block <= block_count; block++) {
        uint8_t *frame = &flash->data[block * DIRECTORY_FRAME_SIZE];
        frame[0x00] = (block == 1) ? 0x51u : (block == block_count) ? 0x53u : 0x52u;
        if (block == 1) {
            frame[0x04] = (uint8_t)(size & 0xFFu);
            frame[0x05] = (uint8_t)((size >> 8) & 0xFFu);
            frame[0x06] = (uint8_t)((size >> 16) & 0xFFu);
            frame[0x07] = (uint8_t)((size >> 24) & 0xFFu);
            frame[DIRECTORY_POCKETSTATION_FLAG_OFFSET] = (uint8_t)'P';
        }
        uint32_t link = (block < block_count) ? block : 0xFFFFu;
        frame[0x08] = (uint8_t)(link & 0xFFu);
        frame[0x09] = (uint8_t)((link >> 8) & 0xFFu);
        frame[0x7F] = directory_frame_xor(frame);
    }
    for (block = block_count + 1; block < 16u; block++) {
        uint8_t *frame = &flash->data[block * DIRECTORY_FRAME_SIZE];
        frame[0x00] = 0xA0u;
        frame[0x08] = 0xFFu;
        frame[0x09] = 0xFFu;
        frame[0x7F] = directory_frame_xor(frame);
    }

    memcpy(&flash->data[FLASH_BLOCK_SIZE], data, size);
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
   docs/hardware-notes.md, "Flash memory"): this
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

/* Real hardware's WRITE-side address for F_SN/F_CAL - distinct from where
   the value is READ from (F_EXTRA, FLASH_CTRL+0x300, see F_SN_LO_OFFSET/
   F_SN_HI_OFFSET/F_CAL_OFFSET above). Confirmed two ways: psx-spx
   documents "[8000000h]=new F_SN_LO value [8000002h]=new F_SN_HI value",
   and disassembling a real ID-editing homebrew's flash-write routine
   shows it performs the real F_KEY1/F_KEY2 unlock sequence and then
   writes the new serial to physical offset 0/2, F_CAL to offset 8 -
   confirmed working on real retail hardware, per real-hardware testing
   this session. See docs/hardware-notes.md, "Hardware ID (F_SN)",
   for the full investigation. */
#define FLASH_HEADER_WRITE_SN_LO_OFFSET 0x0000u
#define FLASH_HEADER_WRITE_SN_HI_OFFSET 0x0002u
#define FLASH_HEADER_WRITE_CAL_OFFSET 0x0008u

static int flash_is_header_write_offset(uint32_t offset) {
    return offset == FLASH_HEADER_WRITE_SN_LO_OFFSET || offset == FLASH_HEADER_WRITE_SN_LO_OFFSET + 1u ||
           offset == FLASH_HEADER_WRITE_SN_HI_OFFSET || offset == FLASH_HEADER_WRITE_SN_HI_OFFSET + 1u ||
           offset == FLASH_HEADER_WRITE_CAL_OFFSET || offset == FLASH_HEADER_WRITE_CAL_OFFSET + 1u;
}

uint8_t flash_read8(flash_t *flash, uint32_t addr) {
    return flash->data[addr % PSEMU_FLASH_SIZE];
}

/* Physical offset 0/2/8 doubles as ordinary card-data storage (block 0's
   directory header, in the normal case) AND as the real write-side
   target for F_SN/F_CAL - which one a write means depends on whether it
   was just armed by the real 3-step unlock sequence (see
   flash_is_unlock_key_offset's comment and FLASH_HEADER_WRITE_SN_LO_OFFSET
   above), not on the address alone. `unlock_step` (see flash.h) tracks
   progress through that sequence purely by WHICH key address is hit next
   (matching how this emulator has always treated these as commands, not
   data - real values aren't validated), armed once all 3 steps land in
   order. Deliberately gated this way rather than an unconditional address
   alias: Chocobo World's own real save-write mechanism is independently
   confirmed (see the F_KEY1/F_KEY2 corruption bug above) to use this same
   unlock-then-write-FLASH2 mechanism for its own save data, so an
   unconditional alias would risk misrouting a legitimate data write that
   happens to land on offset 0/2/8. Stays armed across multiple writes
   (a real header update is 3 separate halfword writes - F_SN_LO, F_SN_HI,
   F_CAL - after a single unlock), and disarms on the first write that
   ISN'T one of those three offsets, signaling the write session moved on
   to unrelated data. */
void flash_write8(flash_t *flash, uint32_t addr, uint8_t value) {
    uint32_t offset = addr % PSEMU_FLASH_SIZE;

    if (flash_is_unlock_key_offset(offset)) {
        /* A real key write is one 16-bit halfword (psemu_bus_write16,
           always low byte then high byte - see its own definition and
           exec_halfword_transfer's STRH path, the only real way these
           get written) - psemu_bus_write8 sees that as two separate
           calls. Only the low byte (the base offset, not +1) advances
           `unlock_step`; the high byte is just the second half of the
           same real write and must be a no-op here, not a fresh
           (mismatched, step-resetting) event of its own. */
        int is_high_byte = offset == FLASH_KEY1_OFFSET + 1u || offset == FLASH_KEY2_OFFSET + 1u;
        int is_key2 = offset == FLASH_KEY2_OFFSET;
        int is_key1 = offset == FLASH_KEY1_OFFSET;
        if (is_high_byte) {
            return;
        }
        if (flash->unlock_step == 0 && is_key2) {
            flash->unlock_step = 1;
        } else if (flash->unlock_step == 1 && is_key1) {
            flash->unlock_step = 2;
        } else if (flash->unlock_step == 2 && is_key2) {
            flash->unlock_step = 3; /* armed */
        } else {
            flash->unlock_step = 0;
        }
        return;
    }

    if (flash->unlock_step == 3 && flash_is_header_write_offset(offset)) {
        if (offset == FLASH_HEADER_WRITE_SN_LO_OFFSET || offset == FLASH_HEADER_WRITE_SN_LO_OFFSET + 1u) {
            uint32_t shift = (offset - FLASH_HEADER_WRITE_SN_LO_OFFSET) * 8u;
            flash->f_sn_lo = (uint16_t)((flash->f_sn_lo & ~(0xFFu << shift)) | ((uint32_t)value << shift));
        } else if (offset == FLASH_HEADER_WRITE_SN_HI_OFFSET || offset == FLASH_HEADER_WRITE_SN_HI_OFFSET + 1u) {
            uint32_t shift = (offset - FLASH_HEADER_WRITE_SN_HI_OFFSET) * 8u;
            flash->f_sn_hi = (uint16_t)((flash->f_sn_hi & ~(0xFFu << shift)) | ((uint32_t)value << shift));
        } else {
            uint32_t shift = (offset - FLASH_HEADER_WRITE_CAL_OFFSET) * 8u;
            flash->f_cal = (uint16_t)((flash->f_cal & ~(0xFFu << shift)) | ((uint32_t)value << shift));
        }
        return;
    }

    flash->unlock_step = 0;
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

    /* F_EXTRA (see flash.h): F_SN_LO/F_SN_HI/F_CAL are real, backed
       registers within it; every other byte in the 256-byte region is
       genuinely unknown/unused and reads back 0, same as nocash's own
       documented defaults for the bytes it didn't identify. */
    if (offset == FLASH_SN_LO_OFFSET || offset == FLASH_SN_LO_OFFSET + 1u) {
        return (uint8_t)(flash->f_sn_lo >> ((offset - FLASH_SN_LO_OFFSET) * 8u));
    }
    if (offset == FLASH_SN_HI_OFFSET || offset == FLASH_SN_HI_OFFSET + 1u) {
        return (uint8_t)(flash->f_sn_hi >> ((offset - FLASH_SN_HI_OFFSET) * 8u));
    }
    if (offset == FLASH_CAL_OFFSET || offset == FLASH_CAL_OFFSET + 1u) {
        return (uint8_t)(flash->f_cal >> ((offset - FLASH_CAL_OFFSET) * 8u));
    }
    if (offset >= FLASH_EXTRA_OFFSET && offset < FLASH_EXTRA_OFFSET + FLASH_EXTRA_SPAN) {
        return 0u;
    }
    /* Gap between F_BANK_VAL's end (+0x140) and F_EXTRA's start (+0x300):
       genuinely unmapped/unknown, same as before this span was extended -
       must NOT fall into the word_index switch below (which existed only
       to cover +0x0/+0x4/+0x8/+0x10 and would otherwise wrongly mirror
       last_command across this whole gap now that FLASH_CTRL_SPAN reaches
       past it). */
    if (offset >= 0x140u) {
        return 0u;
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

    /* F_EXTRA: see flash_ctrl_read8. FlashWriteSerial (SWI 0Fh) is what a
       real app uses to change F_SN on real hardware - direct writes here
       model the same effect at the register level, which is what actually
       matters for an ID-editing homebrew that pokes these bytes itself
       rather than going through the SWI. */
    if (offset == FLASH_SN_LO_OFFSET || offset == FLASH_SN_LO_OFFSET + 1u) {
        uint32_t extra_shift = (offset - FLASH_SN_LO_OFFSET) * 8u;
        flash->f_sn_lo = (uint16_t)((flash->f_sn_lo & ~(0xFFu << extra_shift)) | ((uint32_t)value << extra_shift));
        return;
    }
    if (offset == FLASH_SN_HI_OFFSET || offset == FLASH_SN_HI_OFFSET + 1u) {
        uint32_t extra_shift = (offset - FLASH_SN_HI_OFFSET) * 8u;
        flash->f_sn_hi = (uint16_t)((flash->f_sn_hi & ~(0xFFu << extra_shift)) | ((uint32_t)value << extra_shift));
        return;
    }
    if (offset == FLASH_CAL_OFFSET || offset == FLASH_CAL_OFFSET + 1u) {
        uint32_t extra_shift = (offset - FLASH_CAL_OFFSET) * 8u;
        flash->f_cal = (uint16_t)((flash->f_cal & ~(0xFFu << extra_shift)) | ((uint32_t)value << extra_shift));
        return;
    }
    if (offset >= FLASH_EXTRA_OFFSET && offset < FLASH_EXTRA_OFFSET + FLASH_EXTRA_SPAN) {
        return; /* unknown/reserved F_EXTRA byte - ignored */
    }
    if (offset >= 0x140u) {
        return; /* gap between F_BANK_VAL and F_EXTRA - genuinely unmapped */
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
