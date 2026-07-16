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

/* Loads a PSX Title Sector app image into flash. See docs/hardware-notes.md. */
psemu_status psemu_load_app(psemu_t *ps, const uint8_t *data, size_t size);

/* Loads a raw FLASH2 image (e.g. a whole memory card dump) directly into
   flash, bypassing psemu_load_app's single-Title-Sector validation. Use
   this to load a real card image containing its own directory - the real
   BIOS's app-selection menu (see docs/hardware-notes.md) then navigates
   and launches apps from it the same way real hardware does, entirely
   through psemu_set_buttons - no other setup is required. `data` may be
   shorter than PSEMU_FLASH_SIZE; the rest of flash is left zeroed. */
psemu_status psemu_load_flash_image(psemu_t *ps, const uint8_t *data, size_t size);

void psemu_set_buttons(psemu_t *ps, uint32_t buttons);

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
