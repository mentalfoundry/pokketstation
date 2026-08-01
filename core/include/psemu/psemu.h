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

/* BIOS-owned settings that live in RAM rather than in any hardware register.
   Both are documented in docs/hardware-notes.md ("System sound volume setting",
   "Where the date/time settings actually live"), and both were located by
   tracing a real BIOS, not from any published register map.

   A frontend uses these to hold a setting at a chosen value: the addresses
   involved are ordinary RAM that the BIOS menus also write, so "forcing" a
   value means re-applying it, typically once per rendered frame. That costs
   a few byte stores against the tens of thousands of instructions a frame
   already executes.

   These functions never read the host clock or any other ambient state, so
   the emulator stays deterministic. A caller that wants "current time" passes
   it in. */

/* The three values the BIOS sound menu itself cycles through. Larger values
   attenuate further; 0x04 is special-cased by the BIOS to full silence. */
#define PSEMU_VOLUME_LOUD 0x00u
#define PSEMU_VOLUME_SOFT 0x02u
#define PSEMU_VOLUME_MUTE 0x04u

/* Writes the volume byte once, and reads it back. The BIOS sound menu and any
   app are free to overwrite it afterwards. */
void psemu_set_volume(psemu_t *ps, uint8_t level);
uint8_t psemu_get_volume(const psemu_t *ps);

/* Holds the volume at `level` until psemu_clear_volume_override: writes it
   now, re-seeds it on every psemu_reset, and makes the byte read-only to
   emulated code (the BIOS sound menu stops having any effect, which is what
   an override means).

   Re-applying once per frame is NOT enough for this particular setting, which
   is why it needs its own call. The value has to be in place before the BIOS
   reads it during sound init, and the BIOS clears RAM early in its own boot -
   both inside the frontend's very first emulated frame, with the boot chime
   already finished by the end of it. A per-frame write never gets a turn in
   between, so the chime plays at full volume however often the frontend
   writes. See docs/hardware-notes.md, "System sound volume setting".

   On real hardware the byte survives because battery-backed SRAM holds it and
   the BIOS treats it as already-present state. This emulator always cold-boots,
   so the lock is what stands in for the battery.

   Costs one compare on the RAM write path; nothing on the read or opcode-fetch
   path. Guard calls on psemu_settings_offsets_known, as with psemu_set_volume. */
void psemu_set_volume_override(psemu_t *ps, uint8_t level);
void psemu_clear_volume_override(psemu_t *ps);

/* Sets the clock the BIOS and apps will report.
   `year` is a full year (for example 2026); `dow` is 1=Sunday..7=Saturday.

   This writes the RTC's time and date registers, and BOTH century bytes: the
   one the BIOS clock screen renders from, and the separate one GetBcdDate
   (SWI 0Dh) returns to apps. Writing only one leaves the on-screen year and
   the app-visible year disagreeing.

   Returns 0 without changing anything if the arguments are out of range, or
   if the RTC is currently in program mode (RTC_MODE bit 0, PRGSEL). The BIOS
   programs the clock by issuing RTC_ADJUST increments in a loop until each
   field reaches a target value, so overwriting the registers underneath that
   loop can stop it ever converging. Callers are expected to simply try again
   on the next frame. */
int psemu_set_datetime(psemu_t *ps, int year, int month, int day, int hour, int minute, int second, int dow);

/* Nonzero if the currently-loaded BIOS is a revision whose RAM layout this
   emulator has actually verified.

   The addresses psemu_set_volume and psemu_set_datetime write were traced
   against specific BIOS revisions. On an unrecognized revision they are just
   arbitrary kernel RAM, and writing them would corrupt unrelated state
   silently. A frontend should disable any settings-override UI when this
   returns 0, rather than write and hope. Returns 0 when no BIOS is loaded. */
int psemu_settings_offsets_known(const psemu_t *ps);

/* Nonzero while a dispatched app - rather than the BIOS shell - owns the
   machine. Tracked by psemu_run from where instructions are actually fetched:
   an app runs out of the FLASH1 window, and the BIOS never does.

   This exists to scope the settings overrides above. Their addresses are
   BIOS-owned RAM only while the BIOS shell is running. The moment the BIOS
   dispatches an app off the card, that RAM is the app's, and a frontend still
   re-applying an override once per frame is writing into a running app's
   memory 32 times a second. That corrupts app state: with a real card loaded
   it is enough to make an app reject its own save data as invalid.

   So: guard any per-frame override on this returning 0, in addition to
   psemu_settings_offsets_known. Overrides then hold across the BIOS shell as
   before, stop for the duration of an app, and resume once control comes back.

   Returns to 0 shortly after the app stops executing, not instantly - a
   running app is inside the BIOS for the length of every SWI it issues, and
   those excursions must not read as "the app exited". A psemu_reset clears it
   immediately. */
int psemu_app_running(const psemu_t *ps);

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

/* IR link. This exposes core's IR peripheral as an edge queue.
   psemu_get_audio_samples already uses the same pull/push shape. It lets a frontend drive real I/O, and core
   knows nothing about that I/O's transport.
   Core models IR as an asynchronous edge relay between two independently-clocked instances.
   Real IR hardware is two separate devices, and they share no clock.
   There is therefore no lockstep timing assumption here. Each instance uses only its own local clock.

   An "edge" is a transition of the demodulated IR signal.
   Level 1 means the carrier/LED turned on. Level 0 means it turned off.
   See core/src/ir.h for the full model, which covers carrier gating, RX debounce, and INT_IRDA delivery.

   Typical use, once per rendered frame, after psemu_run:
   - Drain psemu_ir_pop_tx_edge in a loop, and relay each edge onward. One example is a local transport to
     another running instance.
   - For each edge that arrives from that other instance, convert its timestamp into this instance's own
     timeline. Then call psemu_ir_push_rx_edge. */
typedef struct {
    uint64_t timestamp_us; /* in this instance's own IR clock. See psemu_ir_get_clock_us. */
    int level;
} psemu_ir_edge_t;

/* Pulls the next locally-produced TX edge.
   It returns 1 and fills *out_edge. It returns 0 if none are pending. */
int psemu_ir_pop_tx_edge(psemu_t *ps, psemu_ir_edge_t *out_edge);

/* Queues an RX edge from an external source.
   It delivers the edge at `local_timestamp_us` on this instance's own IR clock.
   A caller that relays edges from another instance must first convert that edge's timestamp into this
   instance's timeline.
   The two IR clocks are not synchronized with each other. One way to convert is a shared wall-clock
   reference that both instances can read. */
void psemu_ir_push_rx_edge(psemu_t *ps, uint64_t local_timestamp_us, int level);

/* This instance's own local IR clock, in microseconds.
   Nothing ever resynchronizes it to another clock.
   It is useful only as one half of a timestamp conversion. See psemu_ir_push_rx_edge. */
uint64_t psemu_ir_get_clock_us(const psemu_t *ps);

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
