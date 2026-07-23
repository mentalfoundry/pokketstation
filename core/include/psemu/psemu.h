#ifndef PSEMU_H
#define PSEMU_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSEMU_LCD_WIDTH 32
#define PSEMU_LCD_HEIGHT 32
#define PSEMU_LCD_STRIDE (PSEMU_LCD_WIDTH / 8)

#define PSEMU_BIOS_SIZE (16 * 1024)
#define PSEMU_FLASH_SIZE (128 * 1024)

typedef enum {
    PSEMU_BUTTON_UP = 1 << 0,
    PSEMU_BUTTON_RIGHT = 1 << 1,
    PSEMU_BUTTON_DOWN = 1 << 2,
    PSEMU_BUTTON_LEFT = 1 << 3,
    PSEMU_BUTTON_FIRE = 1 << 4
} psemu_button;

typedef enum {
    PSEMU_OK = 0,
    PSEMU_ERR_BAD_SIZE = -1,
    PSEMU_ERR_BAD_FORMAT = -2,
    PSEMU_ERR_NO_BIOS = -3
} psemu_status;

typedef struct psemu psemu_t;

psemu_t *psemu_create(void);
void psemu_destroy(psemu_t *ps);
void psemu_reset(psemu_t *ps);

/* `data` must be exactly PSEMU_BIOS_SIZE bytes. */
psemu_status psemu_load_bios(psemu_t *ps, const uint8_t *data, size_t size);

/* Loads a PSX Title Sector app image into a synthesized one-entry memory-
   card directory at slot 1, so the real BIOS's menu can navigate to and
   dispatch it exactly as it would from a full card - press Down then
   Action to get past the date/time screen, then Right then Action to
   launch (see docs/hardware-notes.md, "App-selection and dispatch"). `size`
   is capped at 15 blocks' worth (see PSEMU_FLASH_SIZE), one less than a
   full card, since block 0 is reserved for the synthesized directory. */
psemu_status psemu_load_app(psemu_t *ps, const uint8_t *data, size_t size);

/* Unwraps a single-save .mcs file (a real PS1 memory-card directory frame,
   0x80 bytes, followed by that save's raw data blocks - the same convention
   DuckStation, MemcardRex, and most other PS1 save tools use for single-save
   export) and loads the underlying PSX Title Sector the same way
   psemu_load_app does. `data` must start with the directory frame, not the
   raw Title Sector body - use psemu_load_app directly for the latter. */
psemu_status psemu_load_mcs(psemu_t *ps, const uint8_t *data, size_t size);

/* Loads a raw FLASH2 image (e.g. a whole memory card dump) directly into
   flash, bypassing psemu_load_app's single-Title-Sector validation. Use
   this to load a real card image containing its own directory - the real
   BIOS's app-selection menu (see docs/hardware-notes.md) then navigates
   and launches apps from it the same way real hardware does, entirely
   through psemu_set_buttons - no other setup is required. `data` may be
   shorter than PSEMU_FLASH_SIZE; the rest of flash is left zeroed. */
psemu_status psemu_load_flash_image(psemu_t *ps, const uint8_t *data, size_t size);

/* Figures out what `data` is by its size/content, not by a file extension,
   and loads it the right way - the single entry point both frontends
   should use rather than hand-rolling this dispatch themselves (it used to
   be duplicated between them, and drifted out of sync once already):
     - Exactly PSEMU_FLASH_SIZE bytes: a full memory-card image, via
       psemu_load_flash_image.
     - Otherwise, tried as a single-save .mcs (psemu_load_mcs) before a bare
       Title Sector .pss (psemu_load_app) - single-save exports are by far
       the more common format in the wild, a bare Title Sector dump much
       less so. Returns the last-attempted loader's status if neither
       matches. */
psemu_status psemu_load_content(psemu_t *ps, const uint8_t *data, size_t size);

void psemu_set_buttons(psemu_t *ps, uint32_t buttons);

/* The PocketStation's hardware serial number (F_SN, read by real apps via
   SWI 0Ah/FlashReadSerial). Final Fantasy VIII's Chocobo World reads this
   when a save/Chocobo is created, masks off the high byte, and uses the
   last 3 decimal digits of what remains as an "ID" stat that alone
   determines rank (max HP/weapon value, item-drop odds) - confirmed by
   disassembling a real copy of the game, see docs/hardware-notes.md.
   Defaults to the equivalent of "410000D3" (see
   psemu_parse_hardware_id) - low 24 bits 211, the community-documented
   best rank, so a fresh Chocobo World save gets top rank out of the box;
   a frontend may call psemu_set_hardware_id before loading content to
   restore a previously-persisted value instead (e.g. after it was edited
   in-session via a homebrew ID-editing tool). */
#define PSEMU_DEFAULT_HARDWARE_ID (((uint32_t)'A' << 24) | 211u)
uint32_t psemu_get_hardware_id(const psemu_t *ps);
void psemu_set_hardware_id(psemu_t *ps, uint32_t id);

/* Human-readable form: exactly 8 plain hex digits (0-9, A-F/a-f), matching
   exactly what a real homebrew "ID rewriter" tool itself shows and edits
   on a real PocketStation's screen - the raw F_SN register, one hex
   nibble per on-screen digit. Confirmed via real-hardware testing that
   there is no "first digit must be a letter" restriction of any kind - a
   real unit happily accepts and persists a value like "EEEEEEEE". This is
   the only form accepted - what you see in a persisted hardware-ID string
   is exactly the raw value, nothing hidden or translated. (Real units
   also print a "sticker" form under their front cover - one ASCII letter
   followed by 8 decimal digits, e.g. "A02374684" - but that's a separate,
   less-general encoding that can't represent every value the hardware
   actually allows; converting it is a frontend-level concern, not
   something this function does.) Returns nonzero and writes *out_id on
   success, else returns 0 and leaves *out_id unchanged. */
#define PSEMU_HARDWARE_ID_STRING_SIZE 9 /* 8 hex digits + '\0' */
int psemu_parse_hardware_id(const char *str, uint32_t *out_id);
/* Inverse of psemu_parse_hardware_id (canonical 8-hex-digit form only) -
   `buf` must be at least PSEMU_HARDWARE_ID_STRING_SIZE bytes. */
void psemu_format_hardware_id(uint32_t id, char *buf, size_t buf_size);

/* Runs for approximately `cycles` CPU cycles; returns cycles actually executed. */
uint32_t psemu_run(psemu_t *ps, uint32_t cycles);

/* 1bpp, row-major, PSEMU_LCD_STRIDE bytes per row, bit0 = leftmost pixel. */
const uint8_t *psemu_get_framebuffer(const psemu_t *ps);

/* Returns nonzero exactly once per framebuffer change, then clears the flag. */
int psemu_framebuffer_dirty(psemu_t *ps);

/* Fixed output rate of psemu_get_audio_samples - real hardware has no
   fixed sample rate of its own (software bit-bangs the DAC directly, see
   dac.h), so this is this emulator's own choice of resampling rate. */
#define PSEMU_AUDIO_SAMPLE_RATE_HZ 8000

/* Drains up to max_samples of mono, signed 16-bit PCM audio (at
   PSEMU_AUDIO_SAMPLE_RATE_HZ) into buf, returns the number actually
   written. Call periodically (e.g. once per rendered frame) and feed the
   result to a real audio output API. */
uint32_t psemu_get_audio_samples(psemu_t *ps, int16_t *buf, uint32_t max_samples);

/* Nonzero if the CPU has hit an opcode this emulator doesn't recognize -
   sticky, stays set once tripped. Real hardware obviously never does
   this; it means either a genuine gap in this emulator's ARM/Thumb
   decoder, or (more likely the deeper this happens into real execution)
   that something upstream computed a bad jump target and the CPU is now
   running through non-code data. Once set, register/memory state is no
   longer meaningful - frontends should stop stepping and report this
   rather than continue silently corrupting state. */
int psemu_cpu_faulted(const psemu_t *ps);

/* Writes a human-readable diagnostic report to the already-open file `f`:
   full register state, the fault opcode and where it was actually fetched
   from (if psemu_cpu_faulted()), and the most-recently-executed PCs (see
   PSEMU_TRACE_SIZE in cpu.h) - the same kind of information this session's
   real Chocobo World crash investigation had to add one-off tracing to
   find by hand (see docs/hardware-notes.md). Meant for a frontend to call
   whenever something looks wrong - not just on a confirmed CPU fault, so
   a manually-triggered "dump a report" hotkey is worth wiring up too, not
   only automatic fault detection. Does not open, close, or flush `f` -
   callers own the file (or any other FILE*, e.g. stderr) and may write
   their own context (recent input history, frame count, a timestamp)
   before or after this call. */
void psemu_write_crash_report(const psemu_t *ps, FILE *f);

size_t psemu_state_size(const psemu_t *ps);
psemu_status psemu_save_state(const psemu_t *ps, void *buf, size_t size);
psemu_status psemu_load_state(psemu_t *ps, const void *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
