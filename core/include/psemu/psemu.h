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

/* Loads a PSX Title Sector app image into the emulator.
   This function creates a synthesized one-entry memory-card directory at slot 1.
   The real BIOS menu navigates to and dispatches the app from this directory.
   Dispatch works exactly as it does from a full memory card.
   To get past the date/time screen, press Down, then Action.
   To launch the app, press Right, then Action.
   See docs/app-notes.md, "App-selection and dispatch", for details.
   `size` is capped at 15 blocks (see PSEMU_FLASH_SIZE).
   This is one less than a full card, because block 0 holds the synthesized directory. */
psemu_status psemu_load_app(psemu_t *ps, const uint8_t *data, size_t size);

/* Unwraps a single-save .mcs file and loads the underlying PSX Title Sector.
   A .mcs file has a real PS1 memory-card directory frame (0x80 bytes), followed by the
   save's raw data blocks.
   Most PS1 save tools use this convention for single-save export.
   This function loads the Title Sector the same way psemu_load_app does.
   `data` must start with the directory frame, not the raw Title Sector body.
   To load a raw Title Sector body directly, use psemu_load_app. */
psemu_status psemu_load_mcs(psemu_t *ps, const uint8_t *data, size_t size);

/* Loads a raw FLASH2 image (for example, a whole memory card dump) directly into flash.
   This bypasses psemu_load_app's single-Title-Sector validation.
   Use this function to load a real card image that contains its own directory.
   The real BIOS's app-selection menu (see docs/app-notes.md) then navigates and
   launches apps from it, the same way real hardware does.
   Only psemu_set_buttons is needed; no other setup is required.
   `data` may be shorter than PSEMU_FLASH_SIZE.
   The rest of flash is left zeroed. */
psemu_status psemu_load_flash_image(psemu_t *ps, const uint8_t *data, size_t size);

/* Determines the content type of `data` from its size and content, not from a file extension.
   Loads `data` using the matching loader.
   Both frontends must call this function instead of duplicating this dispatch logic.
   The dispatch logic used to be duplicated between the frontends, and it drifted out of
   sync once.

   Dispatch rules:
   - If `data` is exactly PSEMU_FLASH_SIZE bytes, this function treats it as a full
     memory-card image and calls psemu_load_flash_image.
   - Otherwise, this function first tries `data` as a single-save .mcs file, via
     psemu_load_mcs.
   - If that fails, it tries `data` as a bare Title Sector .pss file, via psemu_load_app.
   - Single-save exports are far more common than bare Title Sector dumps.
   - If neither loader matches, this function returns the status of the last-attempted
     loader. */
psemu_status psemu_load_content(psemu_t *ps, const uint8_t *data, size_t size);

void psemu_set_buttons(psemu_t *ps, uint32_t buttons);

/* F_SN is the PocketStation's hardware serial number.
   Real apps read F_SN via SWI 0Ah (FlashReadSerial).
   Final Fantasy VIII's Chocobo World reads F_SN when a save/Chocobo is created.
   Chocobo World masks off the high byte of F_SN.
   Chocobo World uses the last 3 decimal digits of the remaining value as an "ID" stat.
   This "ID" stat alone determines rank: max HP, weapon value, and item-drop odds.
   Disassembling a real copy of the game confirmed this (see docs/hardware-notes.md).

   PSEMU_DEFAULT_HARDWARE_ID defaults to the equivalent of "410000D3" (see
   psemu_parse_hardware_id).
   The low 24 bits of this default are 211, the community-documented best rank.
   A fresh Chocobo World save gets top rank out of the box with this default.
   A frontend can call psemu_set_hardware_id before loading content, to restore a
   previously-persisted value instead. For example, restore a value after a user edits
   it in-session with a homebrew ID-editing tool. */
#define PSEMU_DEFAULT_HARDWARE_ID (((uint32_t)'A' << 24) | 211u)
uint32_t psemu_get_hardware_id(const psemu_t *ps);
void psemu_set_hardware_id(psemu_t *ps, uint32_t id);

/* The human-readable form is exactly 8 plain hex digits (0-9, A-F/a-f).
   This matches what a real homebrew "ID rewriter" tool shows and edits on a real
   PocketStation's screen.
   Each on-screen digit is one hex nibble of the raw F_SN register.
   Real-hardware testing confirmed there is no "first digit must be a letter" restriction.
   A real unit accepts and persists a value like "EEEEEEEE".
   This 8-hex-digit form is the only form this function accepts.
   A persisted hardware-ID string holds the raw value exactly, with nothing hidden or
   translated.

   Real units also print a "sticker" form under their front cover: one ASCII letter
   followed by 8 decimal digits, for example "A02374684". The sticker form is a
   separate, less-general encoding. It cannot represent every value the hardware
   allows. Converting the sticker form is a frontend-level concern; this function
   does not do it.

   On success, this function returns nonzero and writes *out_id.
   On failure, this function returns 0 and leaves *out_id unchanged. */
#define PSEMU_HARDWARE_ID_STRING_SIZE 9 /* 8 hex digits + '\0' */
int psemu_parse_hardware_id(const char *str, uint32_t *out_id);
/* Inverse of psemu_parse_hardware_id (canonical 8-hex-digit form only).
   `buf` must be at least PSEMU_HARDWARE_ID_STRING_SIZE bytes. */
void psemu_format_hardware_id(uint32_t id, char *buf, size_t buf_size);

/* Runs for approximately `cycles` CPU cycles; returns cycles actually executed. */
uint32_t psemu_run(psemu_t *ps, uint32_t cycles);

/* 1bpp, row-major, PSEMU_LCD_STRIDE bytes per row, bit0 = leftmost pixel. */
const uint8_t *psemu_get_framebuffer(const psemu_t *ps);

/* Returns nonzero exactly once per framebuffer change, then clears the flag. */
int psemu_framebuffer_dirty(psemu_t *ps);

/* This is the fixed output rate of psemu_get_audio_samples.
   Real hardware has no fixed sample rate of its own; software bit-bangs the DAC
   directly (see dac.h).
   This emulator chooses this resampling rate itself. */
#define PSEMU_AUDIO_SAMPLE_RATE_HZ 8000

/* Drains up to max_samples of mono, signed 16-bit PCM audio, at PSEMU_AUDIO_SAMPLE_RATE_HZ,
   into buf.
   Returns the number of samples actually written.
   Call this function periodically, for example once per rendered frame.
   Feed the result to a real audio output API. */
uint32_t psemu_get_audio_samples(psemu_t *ps, int16_t *buf, uint32_t max_samples);

/* Returns nonzero if the CPU has executed an opcode this emulator does not recognize.
   This flag is sticky: it stays set once tripped.
   Real hardware never triggers this fault.
   The fault means one of two things:
   - This emulator's ARM/Thumb decoder has a gap.
   - Something upstream computed a bad jump target, and the CPU is now running through
     non-code data.
   A fault that occurs late in execution more often indicates a bad jump target than a
   decoder gap.
   Once this flag is set, register and memory state is no longer meaningful.
   Frontends should stop stepping and report the fault, instead of continuing and
   silently corrupting state. */
int psemu_cpu_faulted(const psemu_t *ps);

/* Writes a human-readable diagnostic report to the already-open file `f`.
   The report includes:
   - The full register state.
   - The fault opcode and where it was actually fetched from, if psemu_cpu_faulted() is true.
   - The most-recently-executed PCs (see PSEMU_TRACE_SIZE in cpu.h).
   An earlier Chocobo World crash investigation had to add one-off tracing by hand to
   find this same information (see docs/hardware-notes.md).

   A frontend should call this function whenever something looks wrong, not only on a
   confirmed CPU fault. Wiring up a manually-triggered "dump a report" hotkey is
   worthwhile, in addition to automatic fault detection.

   This function does not open, close, or flush `f`. The caller owns the file (or any
   other FILE*, for example stderr). The caller may write its own context, such as
   recent input history, frame count, or a timestamp, before or after this call. */
void psemu_write_crash_report(const psemu_t *ps, FILE *f);

size_t psemu_state_size(const psemu_t *ps);
psemu_status psemu_save_state(const psemu_t *ps, void *buf, size_t size);
psemu_status psemu_load_state(psemu_t *ps, const void *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
