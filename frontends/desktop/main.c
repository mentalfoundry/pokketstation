#include <SDL.h>
#include <SDL_syswm.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef _MSC_VER
/* SysLink is Help > About's clickable repo link.
   SysLink exists only in ComCtl32 v6 or later.
   Without this manifest, the OS loader binds the old v5.82 system DLL instead.
   The old DLL has no manifest, so it supports no side-by-side version selection.
   Without v6, CreateDialog silently fails to create the SysLink control. */
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "psemu/psemu.h"
#include "resource.h"
#include "ir_link.h"

#define SCALE 8

/* Shown in Help > About.
   Bump this value by hand to match each release, for example the latest git tag.
   This is deliberately not wired up to git or CMake automatically.
   This string is the one spot to edit for a new release.
   A source zip build has no git available, so an automatic value could silently drift. */
#define POKKETSTATION_VERSION "v1.8.1"

/* Returns the directory the running executable lives in.
   This derives the directory from argv[0], not an OS-specific "current module path" API.
   Explorer already passes the full path in argv[0] when a double-click launches the exe.
   This is the one case that needs this function.
   A CLI invocation passes explicit paths and never reaches this code. */
static void get_exe_dir(const char *argv0, char *out, size_t out_size) {
    const char *last_sep = NULL;
    const char *p;
    for (p = argv0; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last_sep = p;
        }
    }
    size_t len = last_sep ? (size_t)(last_sep - argv0) : 0;
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, argv0, len);
    out[len] = '\0';
}

static void join_path(char *out, size_t out_size, const char *dir, const char *name) {
    if (dir[0] == '\0') {
        snprintf(out, out_size, "%s", name);
    } else {
        snprintf(out, out_size, "%s/%s", dir, name);
    }
}

static uint32_t fnv1a_hash(const uint8_t *data, size_t size) {
    uint32_t hash = 2166136261u;
    size_t i;
    for (i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

/* How many save slots File > Save State/Load State offer.
   Slot 0 is the quick slot, the only one with key bindings (see
   button_scancodes). Slots 1 and 2 are reachable from the menu alone, so a
   state parked in one cannot be lost to a mistyped hotkey - which is the whole
   point of having them. Raising this number needs matching menu items and a
   wider ID_FILE_SAVE_SLOT_BASE/ID_FILE_LOAD_SLOT_BASE range in resource.h/rc;
   nothing else here is fixed at 3. */
#define SAVE_SLOT_COUNT 3

/* Save State/Load State use one file per loaded app/card per slot.
   The file name is the app/card's own file name plus extension, so a user
   browsing the exe's folder can tell at a glance which save goes with
   which app/card. It is derived from app_path rather than cached, because
   the loaded app can change mid-session.
   Slot 0 keeps the bare "<name>.sav" the single slot always used, so saves
   written before the slots existed still load. Slots 1 and 2 append "_1"/"_2"
   after the extension ("chocobo.mcr_1.sav"), which sorts them next to slot 0's
   file rather than scattering them by extension.
   Two different apps/cards that happen to share a file name (e.g. loaded
   from different folders) will share a save file; the app_size/app_hash
   check in the header (below) still refuses to load a mismatched one, so this
   can only cost you a save, never load the wrong state. */
static void get_save_slot_path(char *out, size_t out_size, const char *exe_dir, const char *app_path, int slot) {
    const char *base = app_path;
    const char *slash;
    char name[1024];
    for (slash = app_path; *slash != '\0'; slash++) {
        if (*slash == '/' || *slash == '\\') {
            base = slash + 1;
        }
    }
    if (slot == 0) {
        snprintf(name, sizeof(name), "%s.sav", base);
    } else {
        snprintf(name, sizeof(name), "%s_%d.sav", base, slot);
    }
    join_path(out, out_size, exe_dir, name);
}

/* Names a slot for the message boxes below, so a failure says which of the
   three it was about rather than just "the save state". Reads as part of a
   sentence ("...found in the quick slot..."), hence the lower case. */
static const char *save_slot_label(int slot) {
    static const char *const labels[SAVE_SLOT_COUNT] = {"the quick slot", "slot 1", "slot 2"};
    return (slot >= 0 && slot < SAVE_SLOT_COUNT) ? labels[slot] : "an unknown slot";
}

#define QUICKSAVE_MAGIC "PKQS"
/* 2: psemu_t gained the RAM write lock behind psemu_set_volume_override, so
   the raw state blob changed size. Version-1 files are rejected outright
   rather than read as a truncated version-2 state.
   3: psemu_t gained the app-execution tracking behind psemu_app_running, so
   the blob changed size again. The state blob is a raw struct dump
   (psemu_state_size is sizeof(psemu_t)), so every field added to psemu_t
   needs a bump here - the size check in psemu_load_state only catches a
   state that got smaller, never one that grew. */
#define QUICKSAVE_VERSION 3u

/* app_size/app_hash guard against loading a state saved under a different
   app/card. The per-app filename already makes that unlikely to even be
   attempted; this is a safety net against a hash collision or a
   hand-edited/renamed file. */
typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t app_size;
    uint32_t app_hash;
} quicksave_header_t;

/* Writes a timestamped diagnostic report to disk, and points the user at it on stderr.
   The report has frontend context: the reason string and the frame number.
   The report also has psemu_write_crash_report's full CPU and trace dump.
   Two callers trigger this: an automatic call on a detected CPU fault, and an
   on-demand call via a hotkey (F12 by default, remappable via Tools > Remap Controls...).
   Not every issue worth reporting trips psemu_cpu_faulted(). "The game looks wrong" and
   "no sound" are real issues too.
   This session's Chocobo World crash investigation (see docs/hardware-notes.md) needed
   exactly this kind of state dump, built by hand through one-off tracing. */
static void write_diagnostic_report(
    const psemu_t *ps, const char *reason, unsigned long frame, const char *bios_path, const char *app_path) {
    char path[64];
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    FILE *f;

    strftime(path, sizeof(path), "pokketstation_report_%Y%m%d_%H%M%S.log", tmv);
    f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "psemu: failed to write diagnostic report to %s\n", path);
        return;
    }
    fprintf(f, "reason: %s\n", reason);
    fprintf(f, "bios: %s\n", bios_path);
    fprintf(f, "app: %s\n", app_path);
    fprintf(f, "frame: %lu\n", frame);
    psemu_write_crash_report(ps, f);
    fclose(f);
    fprintf(stderr, "psemu: wrote diagnostic report to %s\n", path);
}

/* This app's own small preferences file.
   No external tool reads or writes this format.
   This differs from the hardware ID's own encoding (see psemu_parse_hardware_id's
   comment in psemu.h).

   settings.cfg remembers:
     - The last successfully-loaded BIOS path.
       This lets a double-click launch (no CLI args) work without bios.bin sitting
       next to the .exe, once a real BIOS has been picked at least once via
       File > Open or a CLI argument.
     - The PocketStation hardware serial number (F_SN).
       The core's own default (PSEMU_DEFAULT_HARDWARE_ID) already gives every fresh
       Chocobo World save the best rank.
       A homebrew ID-editing tool can change this value in-session.
       No other persistent store exists for this value: it lives in a flash "header"
       region outside the ordinary 128KB card image, so it is not part of any
       .mcr/.mcs file.
       This app stores the value as exactly 8 plain hex digits, the same form a real
       "ID rewriter" homebrew itself displays and edits.
       Real hardware confirms there is no "first digit must be a letter" restriction:
       a real unit accepts and persists "EEEEEEEE".
       An empty value means "use the default".
     - Whether to show a console window (show_console). This app has no in-app
       toggle for it. Only a hand edit of settings.cfg sets it.
     - Whether the window title carries live IR Link counters (ir_link_diagnostics),
       off by default and likewise only settable by a hand edit. They report edges
       sent, received, dropped, and arriving too late to be placed in time. That
       last one is the reason this exists at all: a link can report Connected, with
       matching sent and received counts and nothing dropped, and still decode
       nothing, and no other symptom tells those two apart. Useful when diagnosing a
       link that will not transfer, meaningless otherwise, and it changes every
       frame, so it stays off unless asked for.
       --console and --no-console override it for a single run, the same as any
       other CLI flag. They do not write back to settings.cfg.
       A one-off flag on the command line is not the same thing as a persisted
       preference, and must not overwrite one.

   This app saves settings.cfg immediately at the point each value actually
   changes: BIOS loaded via a CLI arg or File > Open, hardware ID edited via
   Tools > Edit Hardware ID. This app does not batch changes for a single
   write at exit. A force-kill or crash mid-session would otherwise silently
   lose whatever changed since launch. */
#define SETTINGS_CONFIG_NAME "settings.cfg"

typedef struct {
    char bios_path[1024];
    char hardware_id[PSEMU_HARDWARE_ID_STRING_SIZE];
    /* "RRGGBB" hex, or empty for the Classic preset's default.
       The Classic preset is a fresh settings.cfg's starting point.
       See load_settings and the DISPLAY_*_CLASSIC constants below. */
    char pixel_color[7];
    char bg_color[7];
    /* "RRGGBB" hex, or empty for DISPLAY_SHADOW_COLOR's default. */
    char shadow_color[7];
    int show_console;
    int show_shadows;
    /* Appends live IR Link edge counters to the window title while connected. Off by default: the numbers
       mean nothing to someone just using the link, and they change every frame. See ir_link_diagnostics in
       load_settings's comment above. */
    int ir_link_diagnostics;
    /* SDL_GetScancodeName()-formatted key names (e.g. "Up", "Z", "Left Ctrl").
       Empty means "use that button's hardcoded default". See resolve_key_binding. */
    char key_up[32];
    char key_down[32];
    char key_left[32];
    char key_right[32];
    char key_fire[32];
    /* Not a PocketStation button. Triggers write_diagnostic_report on demand
       (see button_scancodes in main). */
    char key_debug_log[32];
    /* Not PocketStation buttons. Trigger reset_emulation, and the quick slot's
       save_state_to_slot/load_state_from_slot, on demand (see button_scancodes
       in main). */
    char key_reset[32];
    char key_quick_save[32];
    char key_quick_load[32];
    /* Tools > Date/Time Override.
       This holds a BIOS setting that lives in emulated RAM rather than in any
       hardware register (see docs/hardware-notes.md). DATETIME_OVERRIDE_OFF
       means "don't interfere", so the PocketStation's own system menus behave
       normally. Otherwise it is re-applied every frame, which is what makes it
       stick against the BIOS menu writing its own value. */
    int datetime_override;
    /* Tools > Sound > Volume. 0-100, in whole percent. This is the emulator's own output
       level and nothing the emulated machine can see: it scales the PCM on its
       way to the audio device, after the core has already applied the
       PocketStation's own volume setting. The two therefore multiply.
       There was once a volume_override here too, pinning that PocketStation
       setting the same way datetime_override pins the clock. It is gone: it
       bought three coarse steps, only on a traced BIOS, to do a job this does
       better on any of them. A volume_override= line in an older settings.cfg
       is ignored and drops out at the next save. */
    int master_volume;
    /* Tools > Sound > Speaker. One of the SPEAKER_SIM_* values below. Like
       master_volume this is purely an output-side effect the emulated machine
       cannot see, and it is applied before master_volume so the percentage
       stays a plain loudness control on top of it. */
    int speaker_sim;
} app_settings_t;

#define DATETIME_OVERRIDE_OFF 0
#define DATETIME_OVERRIDE_OS 1

/* Tools > Sound > Volume. The menu offers 10% steps, but the setting is stored as plain
   percent so a finer control later (a slider, a hotkey nudge) needs no new
   format on disk - only a value the menu cannot currently produce, which
   sound_menu_id below rounds to the nearest step it can show. */
#define MASTER_VOLUME_MAX 100
#define MASTER_VOLUME_DEFAULT 100

static int clamp_master_volume(int percent) {
    if (percent < 0) {
        return 0;
    }
    if (percent > MASTER_VOLUME_MAX) {
        return MASTER_VOLUME_MAX;
    }
    return percent;
}

/* The percentage is a loudness, not an amplitude, so it is not what the samples
   are multiplied by.
   Hearing is logarithmic: perceived loudness roughly halves for every 10 dB of
   attenuation. Multiplying the samples by 0.5 is only -6 dB, which still sounds
   around two thirds as loud - a "50%" that plainly is not half. Half as loud
   needs -10 dB, an amplitude of 0.316.
   Solving "each halving of the percentage costs 10 dB" gives
       amplitude = p ^ (log2(10) / 2) = p ^ 1.660964,  p = percent / 100
   which is the exponent below. It lands 100% at 0 dB, 50% at -10 dB (half as
   loud), 25% at -20 dB (a quarter), and 10% at -33 dB.
   There is room for this: the DAC's 10-bit field is rescaled to the full int16
   range before it ever reaches here (see dac.c), so even the -33 dB step still
   leaves a ~700-count waveform rather than dithering itself apart. */
#define MASTER_VOLUME_LOUDNESS_EXPONENT 1.6609640474436813

/* Attenuates one frame's worth of samples in place.
   The 100% case returns without touching the buffer, so the default path costs
   nothing. */
static void apply_master_volume(int16_t *samples, uint32_t count, int percent) {
    if (percent >= MASTER_VOLUME_MAX) {
        return;
    }
    if (percent <= 0) {
        memset(samples, 0, (size_t)count * sizeof(samples[0]));
        return;
    }
    /* pow() once per frame, then integer math per sample. Q16.16: the gain is
       below 1.0 on every path that reaches here, so gain_q16 <= 65535 and the
       product of it with a full-scale sample stays inside int32. */
    double gain = pow((double)percent / (double)MASTER_VOLUME_MAX, MASTER_VOLUME_LOUDNESS_EXPONENT);
    int32_t gain_q16 = (int32_t)(gain * 65536.0 + 0.5);
    for (uint32_t i = 0; i < count; i++) {
        samples[i] = (int16_t)(((int32_t)samples[i] * gain_q16) / 65536);
    }
}

/* Tools > Sound > Speaker.

   The core hands the frontend exactly what the DAC held, sample for sample
   (see dac.c). That is the signal at the *terminals* of the PocketStation's
   speaker, not the sound anyone ever heard come out of one. A real unit's
   speaker is a ~1cm transducer in a plastic shell with no enclosure worth the
   name: it reproduces almost nothing below its own resonance, somewhere in the
   1-2kHz region, and falls away fast below that.

   Feeding the raw signal to a laptop or desktop speaker instead - which does
   have real low-end - reproduces everything the real device physically could
   not, and the result sounds thick and muddy next to the hardware. The content
   makes it worse: the DAC is bit-banged one held level at a time, so nearly
   every tone an app plays is a square-ish wave whose lowest harmonics carry
   most of the energy, plus whatever DC offset the last written DACV level left
   sitting there.

   So this filters what the real speaker would have filtered mechanically: a
   second-order high-pass (RBJ cookbook biquad), which is the right shape
   because a driver below its resonance rolls off at about 12dB per octave. Q
   sets how pronounced the resonant peak at the corner is - a cheap small
   speaker has an obvious one, which is a good part of why it sounds the way it
   does.

   Every preset ends up roughly 4dB quieter than the raw signal. That is not a
   tuning choice to correct: the low end being removed is real energy, and a
   speaker that cannot produce it is quieter, which is also true of the device.
   Tools > Sound > Volume is right there if the result wants turning up.

   These values are voiced by ear against how the hardware sounds, not measured
   off a real unit's speaker. They are presets rather than a free cutoff
   control because "which of these sounds most like the device on your
   speakers" is the actual question; the numbers are here for anyone who wants
   to retune them. */
#define SPEAKER_SIM_OFF 0
#define SPEAKER_SIM_LIGHT 1
#define SPEAKER_SIM_POCKETSTATION 2
#define SPEAKER_SIM_TINNY 3
#define SPEAKER_SIM_COUNT 4
/* Unlike master_volume, this does not default to "behave exactly as before".
   The raw signal is the one that does not match the hardware, so a fresh
   install gets the speaker it is emulating and Full Range is there for anyone
   who would rather hear the DAC untouched. */
#define SPEAKER_SIM_DEFAULT SPEAKER_SIM_POCKETSTATION

/* Indexed by SPEAKER_SIM_*. `name` is the settings.cfg token, and the order
   here is also the Tools > Sound > Speaker menu order (see the
   ID_TOOLS_SPEAKER_BASE range in resource.h). The OFF row carries no
   coefficients: nothing reads them, it is only here to keep the table indexable
   by the same value everything else uses.

   `trim` is a small level adjustment applied with the coefficients, not a
   makeup gain trying to win back what the high-pass removed - pushing gain into
   the limiter below only compresses. Its job is to leave the three presets
   about equally loud as each other, which measures out at around -4dB from raw
   for all three. */
static const struct {
    const char *name;
    double cutoff_hz;
    double q;
    double trim;
} SPEAKER_SIM_PRESETS[SPEAKER_SIM_COUNT] = {
    {"off", 0.0, 0.0, 1.0},
    /* Takes the boom off without changing the character much. For someone on
       speakers that are already small, or who finds the other two thin. */
    {"light", 500.0, 0.70, 1.10},
    /* The device. Corner just under the resonance a transducer that size sits
       at, with enough Q to leave the peak audible. */
    {"pocketstation", 1100.0, 1.10, 1.20},
    /* Further than the hardware goes, for big speakers or a subwoofer where
       even the default still has more low end than the device ever had. */
    {"tinny", 1800.0, 1.60, 1.10},
};

static int clamp_speaker_sim(int preset) {
    if (preset < 0 || preset >= SPEAKER_SIM_COUNT) {
        return SPEAKER_SIM_DEFAULT;
    }
    return preset;
}

/* Parses a settings.cfg speaker= token. An unrecognised one (hand-edited, or
   written by a later version that grew a preset) falls back to the default
   rather than to Off: an unreadable value should not silently turn the feature
   off. */
static int speaker_sim_from_name(const char *name) {
    for (int i = 0; i < SPEAKER_SIM_COUNT; i++) {
        if (strcmp(name, SPEAKER_SIM_PRESETS[i].name) == 0) {
            return i;
        }
    }
    return SPEAKER_SIM_DEFAULT;
}

/* Direct Form I biquad state, plus the preset its coefficients were built for.
   The history has to live across frames: the main loop filters one frame's
   worth of samples at a time, and a filter restarted at zero every 31ms would
   click at every boundary. */
typedef struct {
    int preset; /* SPEAKER_SIM_*, or -1 before the first configure */
    double b0, b1, b2, a1, a2;
    double x1, x2, y1, y2;
} speaker_filter_t;

static void speaker_filter_init(speaker_filter_t *f) {
    memset(f, 0, sizeof(*f));
    f->preset = -1;
}

/* math.h's M_PI is not in standard C and MSVC only defines it behind
   _USE_MATH_DEFINES, so the constant is spelled out here. */
#define SPEAKER_TWO_PI 6.283185307179586

/* Rebuilds the coefficients, and only when the preset actually changed - the
   main loop calls this every frame with whatever the menu currently says.
   The history is cleared on a change so a switch settles from silence instead
   of from samples that belonged to the previous filter. */
static void speaker_filter_configure(speaker_filter_t *f, int preset) {
    if (f->preset == preset) {
        return;
    }
    f->preset = preset;
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0;
    if (preset == SPEAKER_SIM_OFF) {
        return;
    }
    /* RBJ audio-EQ-cookbook high-pass, normalised by a0, with the trim folded
       into the feed-forward half so it costs nothing per sample. */
    double w0 = SPEAKER_TWO_PI * SPEAKER_SIM_PRESETS[preset].cutoff_hz / (double)PSEMU_AUDIO_SAMPLE_RATE_HZ;
    double cos_w0 = cos(w0);
    double alpha = sin(w0) / (2.0 * SPEAKER_SIM_PRESETS[preset].q);
    double a0 = 1.0 + alpha;
    double gain = SPEAKER_SIM_PRESETS[preset].trim;
    f->b0 = gain * ((1.0 + cos_w0) / 2.0) / a0;
    f->b1 = gain * (-(1.0 + cos_w0)) / a0;
    f->b2 = f->b0;
    f->a1 = (-2.0 * cos_w0) / a0;
    f->a2 = (1.0 - alpha) / a0;
}

/* Where the soft limiter below stops being transparent, as a fraction of full
   scale. Everything under this passes through untouched. */
#define SPEAKER_LIMIT_KNEE 0.75

/* A high-pass differentiates a step, so a full-scale square wave - which is
   very nearly all this DAC ever produces (see dac.h) - comes out of the filter
   overshooting to about twice full scale at every edge, before any gain is
   applied at all. That is inherent to the filter shape, not something the
   coefficients can be tuned out of, and it has to go somewhere.

   Hard-clamping it would square those spikes off into exactly the kind of harsh
   digital clipping this feature exists to avoid. Attenuating the whole signal
   enough to fit them (about -6dB) would spend real loudness on transients one
   or two samples wide.

   So the peaks are folded over instead: linear below the knee, tanh above it,
   which is asymptotic to full scale and therefore can never overflow. It is
   also the more faithful of the three, because it is what the real speaker
   does - a driver that small runs out of excursion and compresses peaks rather
   than reproducing them. Measured over square waves from 220Hz to 2.2kHz this
   costs under half a dB of level against no limiting at all, so nearly all of
   the ~4dB the presets sit below raw is the high-pass, not this. */
static double speaker_soft_limit(double y) {
    double u = y / 32768.0;
    double magnitude = fabs(u);
    if (magnitude <= SPEAKER_LIMIT_KNEE) {
        return y;
    }
    double over = (magnitude - SPEAKER_LIMIT_KNEE) / (1.0 - SPEAKER_LIMIT_KNEE);
    double folded = SPEAKER_LIMIT_KNEE + (1.0 - SPEAKER_LIMIT_KNEE) * tanh(over);
    return (u < 0.0 ? -folded : folded) * 32768.0;
}

/* Filters one frame's worth of samples in place. Off returns without touching
   the buffer, so that path costs nothing beyond the preset comparison.
   ~250 samples per frame at 8kHz makes the per-sample double math irrelevant
   next to everything else a frame does. */
static void apply_speaker_filter(speaker_filter_t *f, int16_t *samples, uint32_t count, int preset) {
    speaker_filter_configure(f, clamp_speaker_sim(preset));
    if (f->preset == SPEAKER_SIM_OFF) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        double x = (double)samples[i];
        double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
        /* The pre-limiter y is what goes into the history, on purpose. Feeding
           the limited value back would put the non-linearity inside the
           recursion, where it is no longer a filter with a known response. */
        f->x2 = f->x1;
        f->x1 = x;
        f->y2 = f->y1;
        f->y1 = y;
        samples[i] = (int16_t)speaker_soft_limit(y);
    }
}

/* Packs 8-bit R/G/B into the same 0xRRGGBBAA layout render_framebuffer writes
   into the pixel buffer. See render_framebuffer's own comment on
   SDL_PIXELFORMAT_RGBA8888's byte order. Alpha is always opaque. */
#define RGBA_PACK(r, g, b) \
    ((((uint32_t)(r)) << 24) | (((uint32_t)(g)) << 16) | (((uint32_t)(b)) << 8) | 0xFFu)

#define DISPLAY_PIXEL_LIGHT RGBA_PACK(0x00, 0x00, 0x00)
#define DISPLAY_BG_LIGHT RGBA_PACK(0xFF, 0xFF, 0xFF)
#define DISPLAY_PIXEL_DARK RGBA_PACK(0xFF, 0xFF, 0xFF)
#define DISPLAY_BG_DARK RGBA_PACK(0x00, 0x00, 0x00)
/* Approximates an unlit reflective/transflective LCD, like a watch or a Tamagotchi's.
   Pixel color: a dark, slightly warm ink color, not pure black.
   Background color: a muted sage-gray, not white.
   This is the default color scheme for a freshly-initialized settings.cfg.
   See load_settings. */
#define DISPLAY_PIXEL_CLASSIC RGBA_PACK(0x11, 0x1A, 0x15)
#define DISPLAY_BG_CLASSIC RGBA_PACK(0xBC, 0xC7, 0xB9)

/* Real late-90s STN/passive-matrix LCDs (watches, Tamagotchis, and the
   PocketStation itself) show faint "ghosting" trailing a lit pixel.
   This comes from slow crystal response, not a real drop shadow.
   This is the ghosting color a fresh settings.cfg starts with, matching the
   Classic scheme it also starts with.
   It is not a fixed color: switching color scheme re-matches the ghosting to
   the new scheme (see theme_shadow_for), because a ghost is a half-lit pixel
   of that scheme rather than a color of its own.
   This value is exactly what theme_shadow_for returns for the Classic scheme,
   so a fresh settings.cfg and picking View > Colors > Classic by hand agree
   to the byte. It was 8E9B8E when it was hand-picked and independent of the
   rule, which is within a rounding step of this - close enough to be the same
   color on screen, far enough to leave two disagreeing values on disk. */
#define DISPLAY_SHADOW_COLOR RGBA_PACK(0x90, 0x9A, 0x8E)

/* Inverse of RGBA_PACK's top 3 bytes. Formats the value the way settings.cfg
   stores colors ("RRGGBB"). See save_settings. */
static void format_rgba_hex(uint32_t rgba, char *out, size_t out_size) {
    snprintf(out, out_size, "%02X%02X%02X", (unsigned)(rgba >> 24) & 0xFFu, (unsigned)(rgba >> 16) & 0xFFu,
        (unsigned)(rgba >> 8) & 0xFFu);
}

/* Returns nonzero if `path` already existed and was read from. Returns 0 if it
   did not exist. Callers use this to tell "first run, nothing to read yet"
   apart from "an existing file just happened to not set every field". */
static int load_settings(app_settings_t *settings, const char *path) {
    FILE *f = fopen(path, "r");
    char line[1200];
    int existed = f != NULL;
    settings->bios_path[0] = '\0';
    settings->hardware_id[0] = '\0';
    settings->pixel_color[0] = '\0';
    settings->bg_color[0] = '\0';
    settings->shadow_color[0] = '\0';
    settings->key_up[0] = '\0';
    settings->key_down[0] = '\0';
    settings->key_left[0] = '\0';
    settings->key_right[0] = '\0';
    settings->key_fire[0] = '\0';
    settings->key_debug_log[0] = '\0';
    settings->key_reset[0] = '\0';
    settings->key_quick_save[0] = '\0';
    settings->key_quick_load[0] = '\0';
    settings->show_console = 0;
    settings->ir_link_diagnostics = 0;
    settings->show_shadows = 0;
    /* Defaults to off, so a fresh install behaves exactly as it did before this
       override existed. */
    settings->datetime_override = DATETIME_OVERRIDE_OFF;
    /* Full volume, so an install that has never touched Tools > Sound > Volume sounds
       exactly as it did before this setting existed. */
    settings->master_volume = MASTER_VOLUME_DEFAULT;
    /* Unlike master_volume this default is not "as it was before" - see
       SPEAKER_SIM_DEFAULT for why. */
    settings->speaker_sim = SPEAKER_SIM_DEFAULT;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (strncmp(line, "bios=", 5) == 0) {
                snprintf(settings->bios_path, sizeof(settings->bios_path), "%s", line + 5);
            } else if (strncmp(line, "hardware_id=", 12) == 0) {
                snprintf(settings->hardware_id, sizeof(settings->hardware_id), "%s", line + 12);
            } else if (strncmp(line, "pixel_color=", 12) == 0) {
                snprintf(settings->pixel_color, sizeof(settings->pixel_color), "%s", line + 12);
            } else if (strncmp(line, "bg_color=", 9) == 0) {
                snprintf(settings->bg_color, sizeof(settings->bg_color), "%s", line + 9);
            } else if (strncmp(line, "shadow_color=", 13) == 0) {
                snprintf(settings->shadow_color, sizeof(settings->shadow_color), "%s", line + 13);
            } else if (strncmp(line, "key_up=", 7) == 0) {
                snprintf(settings->key_up, sizeof(settings->key_up), "%s", line + 7);
            } else if (strncmp(line, "key_down=", 9) == 0) {
                snprintf(settings->key_down, sizeof(settings->key_down), "%s", line + 9);
            } else if (strncmp(line, "key_left=", 9) == 0) {
                snprintf(settings->key_left, sizeof(settings->key_left), "%s", line + 9);
            } else if (strncmp(line, "key_right=", 10) == 0) {
                snprintf(settings->key_right, sizeof(settings->key_right), "%s", line + 10);
            } else if (strncmp(line, "key_fire=", 9) == 0) {
                snprintf(settings->key_fire, sizeof(settings->key_fire), "%s", line + 9);
            } else if (strncmp(line, "key_debug_log=", 14) == 0) {
                snprintf(settings->key_debug_log, sizeof(settings->key_debug_log), "%s", line + 14);
            } else if (strncmp(line, "key_reset=", 10) == 0) {
                snprintf(settings->key_reset, sizeof(settings->key_reset), "%s", line + 10);
            } else if (strncmp(line, "key_quick_save=", 15) == 0) {
                snprintf(settings->key_quick_save, sizeof(settings->key_quick_save), "%s", line + 15);
            } else if (strncmp(line, "key_quick_load=", 15) == 0) {
                snprintf(settings->key_quick_load, sizeof(settings->key_quick_load), "%s", line + 15);
            } else if (strncmp(line, "datetime_override=", 18) == 0) {
                settings->datetime_override =
                    strcmp(line + 18, "os") == 0 ? DATETIME_OVERRIDE_OS : DATETIME_OVERRIDE_OFF;
            } else if (strncmp(line, "master_volume=", 14) == 0) {
                /* Clamped rather than rejected: a hand-edited file with 150 in
                   it means "as loud as possible", and there is no wrong-enough
                   value here to be worth refusing to start over. */
                settings->master_volume = clamp_master_volume(atoi(line + 14));
            } else if (strncmp(line, "speaker=", 8) == 0) {
                settings->speaker_sim = speaker_sim_from_name(line + 8);
            } else if (strncmp(line, "show_console=", 13) == 0) {
                settings->show_console = atoi(line + 13) != 0;
            } else if (strncmp(line, "ir_link_diagnostics=", 20) == 0) {
                settings->ir_link_diagnostics = atoi(line + 20) != 0;
            } else if (strncmp(line, "show_shadows=", 13) == 0) {
                settings->show_shadows = atoi(line + 13) != 0;
            }
        }
        fclose(f);
    }
    /* This handles two cases: no settings.cfg yet, or an existing file written
       before one of these fields existed.
       Fill in the real default for each field that is still blank:
       PSEMU_DEFAULT_HARDWARE_ID, the Classic color scheme, the default shadow
       color, and the original hardcoded key bindings.
       Do not leave any field blank.
       This way, the file always shows what is actually in effect, instead of
       an implicit fallback that nothing on disk hints at. */
    if (settings->hardware_id[0] == '\0') {
        psemu_format_hardware_id(PSEMU_DEFAULT_HARDWARE_ID, settings->hardware_id, sizeof(settings->hardware_id));
    }
    if (settings->pixel_color[0] == '\0') {
        format_rgba_hex(DISPLAY_PIXEL_CLASSIC, settings->pixel_color, sizeof(settings->pixel_color));
    }
    if (settings->bg_color[0] == '\0') {
        format_rgba_hex(DISPLAY_BG_CLASSIC, settings->bg_color, sizeof(settings->bg_color));
    }
    if (settings->shadow_color[0] == '\0') {
        format_rgba_hex(DISPLAY_SHADOW_COLOR, settings->shadow_color, sizeof(settings->shadow_color));
    }
    if (settings->key_up[0] == '\0') {
        snprintf(settings->key_up, sizeof(settings->key_up), "%s", SDL_GetScancodeName(SDL_SCANCODE_UP));
    }
    if (settings->key_down[0] == '\0') {
        snprintf(settings->key_down, sizeof(settings->key_down), "%s", SDL_GetScancodeName(SDL_SCANCODE_DOWN));
    }
    if (settings->key_left[0] == '\0') {
        snprintf(settings->key_left, sizeof(settings->key_left), "%s", SDL_GetScancodeName(SDL_SCANCODE_LEFT));
    }
    if (settings->key_right[0] == '\0') {
        snprintf(settings->key_right, sizeof(settings->key_right), "%s", SDL_GetScancodeName(SDL_SCANCODE_RIGHT));
    }
    if (settings->key_fire[0] == '\0') {
        snprintf(settings->key_fire, sizeof(settings->key_fire), "%s", SDL_GetScancodeName(SDL_SCANCODE_Z));
    }
    if (settings->key_debug_log[0] == '\0') {
        snprintf(settings->key_debug_log, sizeof(settings->key_debug_log), "%s", SDL_GetScancodeName(SDL_SCANCODE_F12));
    }
    if (settings->key_reset[0] == '\0') {
        snprintf(settings->key_reset, sizeof(settings->key_reset), "%s", SDL_GetScancodeName(SDL_SCANCODE_F8));
    }
    if (settings->key_quick_save[0] == '\0') {
        snprintf(settings->key_quick_save, sizeof(settings->key_quick_save), "%s", SDL_GetScancodeName(SDL_SCANCODE_F5));
    }
    if (settings->key_quick_load[0] == '\0') {
        snprintf(settings->key_quick_load, sizeof(settings->key_quick_load), "%s", SDL_GetScancodeName(SDL_SCANCODE_F9));
    }
    return existed;
}

static void save_settings(const app_settings_t *settings, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "psemu: failed to persist settings to %s\n", path);
        return;
    }
    fprintf(f, "bios=%s\n", settings->bios_path);
    fprintf(f, "hardware_id=%s\n", settings->hardware_id);
    fprintf(f, "pixel_color=%s\n", settings->pixel_color);
    fprintf(f, "bg_color=%s\n", settings->bg_color);
    fprintf(f, "shadow_color=%s\n", settings->shadow_color);
    fprintf(f, "key_up=%s\n", settings->key_up);
    fprintf(f, "key_down=%s\n", settings->key_down);
    fprintf(f, "key_left=%s\n", settings->key_left);
    fprintf(f, "key_right=%s\n", settings->key_right);
    fprintf(f, "key_fire=%s\n", settings->key_fire);
    fprintf(f, "key_debug_log=%s\n", settings->key_debug_log);
    fprintf(f, "key_reset=%s\n", settings->key_reset);
    fprintf(f, "key_quick_save=%s\n", settings->key_quick_save);
    fprintf(f, "key_quick_load=%s\n", settings->key_quick_load);
    fprintf(f, "datetime_override=%s\n", settings->datetime_override == DATETIME_OVERRIDE_OS ? "os" : "default");
    fprintf(f, "master_volume=%d\n", settings->master_volume);
    fprintf(f, "speaker=%s\n", SPEAKER_SIM_PRESETS[clamp_speaker_sim(settings->speaker_sim)].name);
    fprintf(f, "show_console=%d\n", settings->show_console ? 1 : 0);
    fprintf(f, "ir_link_diagnostics=%d\n", settings->ir_link_diagnostics ? 1 : 0);
    fprintf(f, "show_shadows=%d\n", settings->show_shadows ? 1 : 0);
    fclose(f);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Parses exactly 6 hex digits ("RRGGBB") and nothing else.
   This is deliberately strict, with no alternate format allowed, matching
   psemu_parse_hardware_id in psemu.h. */
static int parse_hex_rgb(const char *s, uint8_t *r, uint8_t *g, uint8_t *b) {
    int nibbles[6];
    int i;
    if (!s || strlen(s) != 6) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        nibbles[i] = hex_nibble(s[i]);
        if (nibbles[i] < 0) {
            return 0;
        }
    }
    *r = (uint8_t)((nibbles[0] << 4) | nibbles[1]);
    *g = (uint8_t)((nibbles[2] << 4) | nibbles[3]);
    *b = (uint8_t)((nibbles[4] << 4) | nibbles[5]);
    return 1;
}

/* One entry of the live key-to-PocketStation-button mapping.
   The main loop polls this mapping every frame (see button_scancodes in main).
   display_name is used only for remap prompts and labels. This app never
   persists display_name itself; it persists the scancode's own
   SDL_GetScancodeName to settings.cfg instead.
   `bit` is 0 for the trailing "Create Debug Log" entry.
   This entry is not a real PocketStation button.
   See button_scancodes in main for how this entry is actually consumed. */
typedef struct {
    SDL_Scancode scancode;
    uint32_t bit;
    const char *display_name;
} button_binding_t;

/* Number of rows in Tools > Remap Controls: Up, Down, Left, Right, Fire,
   plus 4 non-button hotkeys - Create Debug Log, Reset, Save State,
   Load State.
   This matches IDD_REMAP_CONTROLS' row count.
   This also matches the IDC_REMAP_LABEL_BASE/IDC_REMAP_CHANGE_BASE ranges in
   resource.h (9 consecutive IDs each). */
#define REMAP_BINDING_COUNT 9

/* This app writes this marker to a settings.cfg key_* field in place of a
   real key name, when that row was explicitly cleared because another row
   just claimed its key (see prompt_remap_controls).
   This marker is distinct from an empty field. An empty field means "never
   set, use the hardcoded default" (see resolve_key_binding).
   Without this distinction, persisting an unbound row as "" would silently
   revert it to its original default key on the next launch. */
#define KEY_BINDING_UNBOUND_MARKER "(unbound)"

/* `saved_name` is a settings.cfg key_* field.
   This function parses it via SDL_GetScancodeFromName.
   It falls back to `fallback` if `saved_name` is empty, or does not name a
   real key (for example, hand-edited to garbage).
   An explicitly unbound field (see KEY_BINDING_UNBOUND_MARKER) returns
   SDL_SCANCODE_UNKNOWN instead of falling back.
   This is a deliberate "no key" state, not a missing or invalid one. */
static SDL_Scancode resolve_key_binding(const char *saved_name, SDL_Scancode fallback) {
    if (strcmp(saved_name, KEY_BINDING_UNBOUND_MARKER) == 0) {
        return SDL_SCANCODE_UNKNOWN;
    }
    if (saved_name[0] != '\0') {
        SDL_Scancode sc = SDL_GetScancodeFromName(saved_name);
        if (sc != SDL_SCANCODE_UNKNOWN) {
            return sc;
        }
    }
    return fallback;
}

/* SDL_GetScancodeName(SDL_SCANCODE_UNKNOWN) returns "".
   Once written to settings.cfg, "" is indistinguishable from "never set"
   (see resolve_key_binding).
   This function writes KEY_BINDING_UNBOUND_MARKER instead, for an
   explicitly-unbound row. */
static void format_key_binding_name(char *out, size_t out_size, SDL_Scancode scancode) {
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        snprintf(out, out_size, "%s", KEY_BINDING_UNBOUND_MARKER);
    } else {
        snprintf(out, out_size, "%s", SDL_GetScancodeName(scancode));
    }
}

/* Inverse of resolve_key_binding.
   `bindings` must hold exactly REMAP_BINDING_COUNT entries, in fixed
   Up/Down/Left/Right/Fire/Create-Debug-Log/Reset/Save-State/
   Load-State order (see button_scancodes in main). */
static void save_key_bindings(app_settings_t *settings, const button_binding_t bindings[REMAP_BINDING_COUNT]) {
    format_key_binding_name(settings->key_up, sizeof(settings->key_up), bindings[0].scancode);
    format_key_binding_name(settings->key_down, sizeof(settings->key_down), bindings[1].scancode);
    format_key_binding_name(settings->key_left, sizeof(settings->key_left), bindings[2].scancode);
    format_key_binding_name(settings->key_right, sizeof(settings->key_right), bindings[3].scancode);
    format_key_binding_name(settings->key_fire, sizeof(settings->key_fire), bindings[4].scancode);
    format_key_binding_name(settings->key_debug_log, sizeof(settings->key_debug_log), bindings[5].scancode);
    format_key_binding_name(settings->key_reset, sizeof(settings->key_reset), bindings[6].scancode);
    format_key_binding_name(settings->key_quick_save, sizeof(settings->key_quick_save), bindings[7].scancode);
    format_key_binding_name(settings->key_quick_load, sizeof(settings->key_quick_load), bindings[8].scancode);
}

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

/* State the menu bar's WM_COMMAND handlers need to reach.
   This is bundled because SDL_SetWindowsMessageHook only takes a single
   void *userdata. */
typedef struct {
    psemu_t *ps;
    uint8_t **bios;
    size_t *bios_size;
    char *bios_path;
    size_t bios_path_cap;
    uint8_t **app;
    size_t *app_size;
    char *app_path;
    size_t app_path_cap;
    HWND hwnd;
    int *running;
    int *cpu_faulted_reported;
    uint32_t *pixel_rgba;
    uint32_t *bg_rgba;
    int *show_shadows;
    uint32_t *shadow_rgba;
    button_binding_t *button_scancodes; /* fixed REMAP_BINDING_COUNT-element Up/Down/Left/Right/Fire/Debug-Log/Reset/Save-State/Load-State array, see main */
    app_settings_t *settings;
    const char *settings_path;
    const char *exe_dir;
    ir_link_t *ir_link;
} menu_context_t;

/* Shows IR Link's live status as the entire window title, e.g. "IR - Connected", replacing the plain
   "pokketstation" title while the link is active. Kept intentionally short: the default window is small
   enough that a longer title just gets clipped by Windows, and the status is the one thing worth seeing
   at a glance.
   Hosting and connecting get their own fixed short strings rather than ir_link_status_text's wording
   verbatim, since that text is shared with ir_link_selftest.c/ir_probe's console output and can carry
   more detail than fits here. Connected and error states pass status_text through as-is: "Connected" is
   already short (plus live counters if ir_link_diagnostics is on), and an error's detail is worth keeping.
   Every action that can change ir_link's state calls this. Those actions are Host, Connect, and Disconnect
   from the menu.
   main's loop also calls it once per frame. A hosting or connecting link can change status on its own, with no
   menu click: a peer connects, or the link fails. */
static void ir_link_refresh_title(menu_context_t *ctx) {
    char title[192];
    if (!ir_link_is_active(ctx->ir_link)) {
        snprintf(title, sizeof(title), "pokketstation");
    } else if (ctx->ir_link->state == IR_LINK_HOSTING) {
        snprintf(title, sizeof(title), "IR - Waiting...");
    } else if (ctx->ir_link->state == IR_LINK_CONNECTING) {
        snprintf(title, sizeof(title), "IR - Connecting...");
    } else {
        snprintf(title, sizeof(title), "IR - %s", ir_link_status_text(ctx->ir_link));
    }
    SetWindowTextA(ctx->hwnd, title);
}

/* Core wipes ir_t's clock and any queued edges on both psemu_reset and psemu_load_state (see ir_link.h's
   comment on ir_link_disconnect). A link left connected across either would silently desync: this instance's
   IR clock jumps back to zero while the peer's does not, so every subsequently-relayed timestamp would be
   wrong. Every call site that resets or loads state calls this first. */
static void drop_ir_link_if_active(menu_context_t *ctx) {
    if (ir_link_is_active(ctx->ir_link)) {
        ir_link_disconnect(ctx->ir_link);
        ir_link_refresh_title(ctx);
    }
}

static void prompt_open_bios(menu_context_t *ctx) {
    char path[1024] = {0};
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ctx->hwnd;
    ofn.lpstrFilter = "BIOS dump (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrTitle = "Load BIOS dump";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) {
        return;
    }

    size_t new_size = 0;
    uint8_t *new_bios = read_file(path, &new_size);
    if (!new_bios) {
        MessageBoxA(ctx->hwnd, "Couldn't read that file.", "pokketstation", MB_ICONERROR);
        return;
    }
    if (psemu_load_bios(ctx->ps, new_bios, new_size) != PSEMU_OK) {
        MessageBoxA(ctx->hwnd, "Not a valid BIOS image (expected a 16384-byte dump).", "pokketstation", MB_ICONERROR);
        free(new_bios);
        return;
    }

    free(*ctx->bios);
    *ctx->bios = new_bios;
    *ctx->bios_size = new_size;
    snprintf(ctx->bios_path, ctx->bios_path_cap, "%s", path);
    psemu_reset(ctx->ps);
    drop_ir_link_if_active(ctx);
    *ctx->cpu_faulted_reported = 0;

    snprintf(ctx->settings->bios_path, sizeof(ctx->settings->bios_path), "%s", path);
    save_settings(ctx->settings, ctx->settings_path);
}

static void prompt_open_app(menu_context_t *ctx) {
    char path[1024] = {0};
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = ctx->hwnd;
    ofn.lpstrFilter =
        "App or memory-card image (*.pss;*.mcs;*.mcr)\0*.pss;*.mcs;*.mcr\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof(path);
    ofn.lpstrTitle = "Open app or memory card";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) {
        return;
    }

    size_t new_size = 0;
    uint8_t *new_app = read_file(path, &new_size);
    if (!new_app) {
        MessageBoxA(ctx->hwnd, "Couldn't read that file.", "pokketstation", MB_ICONERROR);
        return;
    }
    if (psemu_load_content(ctx->ps, new_app, new_size) != PSEMU_OK) {
        MessageBoxA(ctx->hwnd, "Not a valid app or memory-card image.", "pokketstation", MB_ICONERROR);
        free(new_app);
        return;
    }

    free(*ctx->app);
    *ctx->app = new_app;
    *ctx->app_size = new_size;
    snprintf(ctx->app_path, ctx->app_path_cap, "%s", path);
    psemu_reset(ctx->ps);
    drop_ir_link_if_active(ctx);
    *ctx->cpu_faulted_reported = 0;
}

/* Restarts the currently loaded BIOS and app/card from a clean state.
   It reloads neither file.
   psemu_reset already performs this same reset after a fresh load. See its comment in psemu.c.
   This triggers it on demand, instead of only after a file dialog succeeds. */
static void reset_emulation(menu_context_t *ctx) {
    psemu_reset(ctx->ps);
    drop_ir_link_if_active(ctx);
    *ctx->cpu_faulted_reported = 0;
}

static void save_state_to_slot(menu_context_t *ctx, int slot) {
    char path[1024];
    char msg[256];
    get_save_slot_path(path, sizeof(path), ctx->exe_dir, ctx->app_path, slot);

    size_t state_size = psemu_state_size(ctx->ps);
    size_t total_size = sizeof(quicksave_header_t) + state_size;
    uint8_t *buf = (uint8_t *)malloc(total_size);
    if (!buf) {
        MessageBoxA(ctx->hwnd, "Out of memory.", "pokketstation", MB_ICONERROR);
        return;
    }

    quicksave_header_t header;
    memcpy(header.magic, QUICKSAVE_MAGIC, 4);
    header.version = QUICKSAVE_VERSION;
    header.app_size = (uint32_t)*ctx->app_size;
    header.app_hash = fnv1a_hash(*ctx->app, *ctx->app_size);
    memcpy(buf, &header, sizeof(header));
    psemu_save_state(ctx->ps, buf + sizeof(header), state_size);

    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, total_size, f) != total_size) {
        snprintf(msg, sizeof(msg), "Couldn't write the save state file for %s.", save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
    }
    if (f) {
        fclose(f);
    }
    free(buf);
}

static void load_state_from_slot(menu_context_t *ctx, int slot) {
    char path[1024];
    char msg[256];
    get_save_slot_path(path, sizeof(path), ctx->exe_dir, ctx->app_path, slot);

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(msg, sizeof(msg), "No save state in %s for the currently loaded app/card.", save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONWARNING);
        return;
    }

    quicksave_header_t header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header) || memcmp(header.magic, QUICKSAVE_MAGIC, 4) != 0 ||
        header.version != QUICKSAVE_VERSION) {
        snprintf(msg, sizeof(msg), "The file for %s isn't a valid save state.", save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        fclose(f);
        return;
    }

    uint32_t current_hash = fnv1a_hash(*ctx->app, *ctx->app_size);
    if (header.app_size != (uint32_t)*ctx->app_size || header.app_hash != current_hash) {
        snprintf(msg, sizeof(msg), "The save state in %s doesn't match the currently loaded app/card.",
            save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        fclose(f);
        return;
    }

    size_t state_size = psemu_state_size(ctx->ps);

    /* The blob has to be exactly one state for THIS build, and only the file's
       own length can say so. psemu_load_state cannot: its size argument comes
       from psemu_state_size right here, so it is always exactly sizeof(psemu_t)
       and its own check can never fire. A blob saved by a build whose psemu_t
       was smaller is caught by the short read below; one saved by a build whose
       psemu_t was larger is not - the fread succeeds on the leading bytes and
       loads a silently misaligned state. QUICKSAVE_VERSION is meant to catch
       both, but it only works if every psemu_t change remembers to bump it, and
       this check does not depend on anyone remembering. */
    long body_start = ftell(f);
    if (body_start < 0 || fseek(f, 0, SEEK_END) != 0) {
        snprintf(msg, sizeof(msg), "Couldn't read the save state in %s.", save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        fclose(f);
        return;
    }
    long body_size = ftell(f) - body_start;
    if (body_size != (long)state_size || fseek(f, body_start, SEEK_SET) != 0) {
        snprintf(msg, sizeof(msg), "Couldn't load the save state in %s (wrong build/version?).",
            save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        fclose(f);
        return;
    }

    uint8_t *buf = (uint8_t *)malloc(state_size);
    if (!buf || fread(buf, 1, state_size, f) != state_size || psemu_load_state(ctx->ps, buf, state_size) != PSEMU_OK) {
        snprintf(msg, sizeof(msg), "Couldn't load the save state in %s (wrong build/version?).",
            save_slot_label(slot));
        MessageBoxA(ctx->hwnd, msg, "pokketstation", MB_ICONERROR);
        free(buf);
        fclose(f);
        return;
    }

    free(buf);
    fclose(f);
    drop_ir_link_if_active(ctx);
    *ctx->cpu_faulted_reported = 0;
}

/* lParam payload for hwid_dialog_proc.
   Passed in via DialogBoxParamA, retrieved with GetWindowLongPtrA(GWLP_USERDATA).
   parsed_id is filled in, and IDOK is allowed to close the dialog, only
   after the edit control's text passes psemu_parse_hardware_id. */
typedef struct {
    char text[PSEMU_HARDWARE_ID_STRING_SIZE];
    uint32_t parsed_id;
} hwid_dialog_data_t;

static INT_PTR CALLBACK hwid_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)lparam);
        SetDlgItemTextA(hdlg, IDC_HWID_EDIT, ((hwid_dialog_data_t *)lparam)->text);
        /* PSEMU_HARDWARE_ID_STRING_SIZE includes the '\0'.
           The canonical form is always exactly 8 hex digits.
           Cap typed input at that length instead of letting
           psemu_parse_hardware_id reject it later. */
        SendDlgItemMessageA(hdlg, IDC_HWID_EDIT, EM_SETLIMITTEXT, PSEMU_HARDWARE_ID_STRING_SIZE - 1, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            hwid_dialog_data_t *data = (hwid_dialog_data_t *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
            char text[PSEMU_HARDWARE_ID_STRING_SIZE];
            GetDlgItemTextA(hdlg, IDC_HWID_EDIT, text, sizeof(text));
            if (!psemu_parse_hardware_id(text, &data->parsed_id)) {
                MessageBoxA(hdlg, "Expected exactly 8 hex digits (0-9, A-F).", "pokketstation", MB_ICONERROR);
                return TRUE;
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        } else if (LOWORD(wparam) == IDCANCEL) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void prompt_edit_hardware_id(menu_context_t *ctx) {
    hwid_dialog_data_t data;
    psemu_format_hardware_id(psemu_get_hardware_id(ctx->ps), data.text, sizeof(data.text));
    if (DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_HWID), ctx->hwnd, hwid_dialog_proc,
            (LPARAM)&data) == IDOK) {
        psemu_set_hardware_id(ctx->ps, data.parsed_id);
        psemu_format_hardware_id(data.parsed_id, ctx->settings->hardware_id, sizeof(ctx->settings->hardware_id));
        save_settings(ctx->settings, ctx->settings_path);
    }
}

/* Resizes the window so its *client* area, where the framebuffer actually
   renders, becomes exactly PSEMU_LCD_{WIDTH,HEIGHT} * SCALE * multiplier.
   This holds regardless of how much chrome (menu bar, title bar, borders)
   the window currently has.
   This uses the same before/after-GetClientRect technique that compensates
   for the menu bar's height at startup. */
static void resize_client_to_scale(HWND hwnd, int multiplier) {
    RECT client, window_rect;
    int chrome_w, chrome_h, target_w, target_h;

    GetClientRect(hwnd, &client);
    GetWindowRect(hwnd, &window_rect);
    chrome_w = (window_rect.right - window_rect.left) - (client.right - client.left);
    chrome_h = (window_rect.bottom - window_rect.top) - (client.bottom - client.top);
    target_w = PSEMU_LCD_WIDTH * SCALE * multiplier;
    target_h = PSEMU_LCD_HEIGHT * SCALE * multiplier;
    SetWindowPos(hwnd, NULL, 0, 0, target_w + chrome_w, target_h + chrome_h, SWP_NOMOVE | SWP_NOZORDER);
}

/* --- Deriving a whole scheme from one picked color -----------------------
   View > Colors > Advanced Colors... asks for one color, the screen
   (background), and works the other two out from it.
   The background is the anchor rather than the active pixel color because
   most of the LCD is unlit most of the time: the background *is* the color a
   user perceives the screen to be, the same way an LCD is described by its
   panel tint ("that green Game Boy screen"), not by its ink.
   The two derived colors stay in the picked color's own hue, which is what
   keeps any pick from coming out jarring - a real STN LCD's ink is its panel
   color darkened, not a separate color. Only lightness and saturation move.
   Sanity check: feeding the shipped Classic background (BCC7B9) through
   derive_theme_colors returns 20291D / 939E90, a slightly softer take on the
   hand-picked Classic ink and shadow (111A15 / 909A8E) this project already
   used - close enough to say the rule agrees with a human eye. */

/* Undoes sRGB's transfer function for one 0..1 channel, so channels can be
   weighted into a real luminance. Straight RGB averages are not perceptual:
   full green looks far brighter than full blue at the same numeric value. */
static double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

/* WCAG 2.x relative luminance of an RGBA_PACK value (alpha ignored). */
static double rgba_luminance(uint32_t rgba) {
    double r = srgb_to_linear((double)((rgba >> 24) & 0xFFu) / 255.0);
    double g = srgb_to_linear((double)((rgba >> 16) & 0xFFu) / 255.0);
    double b = srgb_to_linear((double)((rgba >> 8) & 0xFFu) / 255.0);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/* WCAG 2.x contrast ratio, 1.0 (identical) to 21.0 (black on white). */
static double contrast_ratio(uint32_t a, uint32_t b) {
    double la = rgba_luminance(a) + 0.05;
    double lb = rgba_luminance(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}

/* All three outputs are 0..1. Hue is a fraction of the circle, not degrees. */
static void rgba_to_hsl(uint32_t rgba, double *h, double *s, double *l) {
    double r = (double)((rgba >> 24) & 0xFFu) / 255.0;
    double g = (double)((rgba >> 16) & 0xFFu) / 255.0;
    double b = (double)((rgba >> 8) & 0xFFu) / 255.0;
    double max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double delta = max - min;
    *l = (max + min) / 2.0;
    if (delta <= 0.0) {
        /* A pure gray has no meaningful hue. Reporting 0 (red) is harmless:
           derive_theme_colors keeps saturation at 0 for it either way, so the
           derived colors stay gray too. */
        *h = 0.0;
        *s = 0.0;
        return;
    }
    /* Safe from a divide by zero: l is only 0.0 or 1.0 when max == min, which
       the delta check above already returned on. */
    *s = delta / (1.0 - fabs(2.0 * (*l) - 1.0));
    if (max == r) {
        *h = (g - b) / delta;
        if (*h < 0.0) {
            *h += 6.0;
        }
    } else if (max == g) {
        *h = (b - r) / delta + 2.0;
    } else {
        *h = (r - g) / delta + 4.0;
    }
    *h /= 6.0;
}

static uint8_t channel_to_byte(double v) {
    int scaled = (int)(v * 255.0 + 0.5);
    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > 255) {
        scaled = 255;
    }
    return (uint8_t)scaled;
}

static double hue_to_channel(double p, double q, double t) {
    if (t < 0.0) {
        t += 1.0;
    }
    if (t > 1.0) {
        t -= 1.0;
    }
    if (t < 1.0 / 6.0) {
        return p + (q - p) * 6.0 * t;
    }
    if (t < 1.0 / 2.0) {
        return q;
    }
    if (t < 2.0 / 3.0) {
        return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    }
    return p;
}

/* Inverse of rgba_to_hsl. Hue is expected in 0..1 (it wraps), s and l in 0..1. */
static uint32_t hsl_to_rgba(double h, double s, double l) {
    double r, g, b;
    if (s <= 0.0) {
        r = g = b = l;
    } else {
        double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
        double p = 2.0 * l - q;
        r = hue_to_channel(p, q, h + 1.0 / 3.0);
        g = hue_to_channel(p, q, h);
        b = hue_to_channel(p, q, h - 1.0 / 3.0);
    }
    return RGBA_PACK(channel_to_byte(r), channel_to_byte(g), channel_to_byte(b));
}

/* Mixes `t` (0..1) of `to` into `from`, per channel. */
static uint32_t blend_rgba(uint32_t from, uint32_t to, double t) {
    uint8_t out[3];
    int i;
    for (i = 0; i < 3; i++) {
        int shift = 24 - i * 8;
        double a = (double)((from >> shift) & 0xFFu);
        double b = (double)((to >> shift) & 0xFFu);
        out[i] = channel_to_byte((a + (b - a) * t) / 255.0);
    }
    return RGBA_PACK(out[0], out[1], out[2]);
}

/* The derived ink has to clear this contrast ratio against the background
   wherever the picked color physically allows it. 8.5:1 is comfortably past
   WCAG AAA (7:1) without going all the way to the harshest possible pair;
   the shipped Classic scheme sits at about 10:1 for reference. */
#define THEME_INK_MIN_CONTRAST 8.5
/* Contrast alone is not enough to make ink *look* like ink. A light
   background whose contrast target is already met at 34% lightness still
   reads as gray-on-white rather than as a display, so dark ink is pushed at
   least this dark, and light ink at least this light. */
#define THEME_INK_DARK_MAX_LIGHTNESS 0.30
#define THEME_INK_LIGHT_MIN_LIGHTNESS 0.78
/* The ghosting trail is a partially-switched pixel, so it belongs between the
   background and the ink, much nearer the background. 0.26 is not arbitrary:
   it is where the ghosting color hand-picked for the Classic scheme before
   any of this existed (8E9B8E) sits between that scheme's background and its
   ink, to within a rounding step. The rule was fitted to the eye, not the
   other way round. */
#define THEME_SHADOW_MIX 0.26

/* The one rule for what a scheme's ghosting trail looks like, shared by the
   Light/Dark/Classic presets and by Advanced Colors...' matched scheme, so no
   path can drift from the others.
   DISPLAY_SHADOW_COLOR is this function's own result for the Classic scheme,
   kept as a constant because settings.cfg's defaults are filled in long
   before any scheme is chosen. */
static uint32_t theme_shadow_for(uint32_t bg_rgba, uint32_t pixel_rgba) {
    return blend_rgba(bg_rgba, pixel_rgba, THEME_SHADOW_MIX);
}

/* Works out the active pixel color and the sprite shadow color that go with
   `bg_rgba`. Neither output is ever equal to the input.
   The caller keeps the picked background exactly as picked - it is never
   "corrected" - so a deliberately loud pick still gets a usable scheme rather
   than a silently different color. */
static void derive_theme_colors(uint32_t bg_rgba, uint32_t *out_pixel, uint32_t *out_shadow) {
    double h, s, l, ink_s, ink_l, step;
    /* Ink goes whichever way has more room to move. Comparing against both
       ends rather than testing lightness > 50% is what gets mid-tone picks
       right: a saturated mid blue has more headroom towards white than the
       naive lightness test suggests. */
    int dark_ink = contrast_ratio(bg_rgba, RGBA_PACK(0x00, 0x00, 0x00))
        >= contrast_ratio(bg_rgba, RGBA_PACK(0xFF, 0xFF, 0xFF));
    int i;

    rgba_to_hsl(bg_rgba, &h, &s, &l);
    if (dark_ink) {
        /* Saturation has to go *up* for dark ink or the hue disappears into
           black; a near-gray dark tone is what makes a scheme look muddy. */
        ink_s = s * 1.5 > 0.45 ? 0.45 : s * 1.5;
        step = -0.005;
    } else {
        /* Light ink is the opposite case: carrying the background's full
           saturation up into a near-white tone comes out neon. */
        ink_s = s * 0.6 > 0.30 ? 0.30 : s * 0.6;
        step = 0.005;
    }
    /* Walk away from the background's own lightness and stop at the first
       step that clears the contrast target, rather than jumping straight to
       black or white. Stopping early is the whole point: the softest ink that
       is still legible is the one that does not fight the background.
       Starting the search from the extreme end covers picks where the target
       is physically unreachable (a saturated mid-tone red screen tops out
       around 5:1) - those get the best contrast available instead. */
    ink_l = dark_ink ? 0.0 : 1.0;
    for (i = 0; i <= 200; i++) {
        double candidate = l + step * (double)i;
        if (candidate < 0.0 || candidate > 1.0) {
            break;
        }
        if (contrast_ratio(hsl_to_rgba(h, ink_s, candidate), bg_rgba) >= THEME_INK_MIN_CONTRAST) {
            ink_l = candidate;
            break;
        }
    }
    if (dark_ink && ink_l > THEME_INK_DARK_MAX_LIGHTNESS) {
        ink_l = THEME_INK_DARK_MAX_LIGHTNESS;
    }
    if (!dark_ink && ink_l < THEME_INK_LIGHT_MIN_LIGHTNESS) {
        ink_l = THEME_INK_LIGHT_MIN_LIGHTNESS;
    }
    *out_pixel = hsl_to_rgba(h, ink_s, ink_l);
    *out_shadow = theme_shadow_for(bg_rgba, *out_pixel);
}

static void apply_display_colors_full(menu_context_t *ctx, uint32_t pixel_rgba, uint32_t bg_rgba,
    uint32_t shadow_rgba) {
    *ctx->pixel_rgba = pixel_rgba;
    *ctx->bg_rgba = bg_rgba;
    *ctx->shadow_rgba = shadow_rgba;
    format_rgba_hex(pixel_rgba, ctx->settings->pixel_color, sizeof(ctx->settings->pixel_color));
    format_rgba_hex(bg_rgba, ctx->settings->bg_color, sizeof(ctx->settings->bg_color));
    format_rgba_hex(shadow_rgba, ctx->settings->shadow_color, sizeof(ctx->settings->shadow_color));
    save_settings(ctx->settings, ctx->settings_path);
}

/* Switching to a whole scheme - the Light/Dark/Classic presets - re-matches
   the sprite shadow to it, the same way Advanced Colors... does. A scheme is
   all three colors: the sage-gray ghost that suits Classic is plainly wrong
   over Light's white, and picking a preset is a decision about the whole
   look, not about two thirds of it.
   This does mean a preset discards a shadow color set by hand in Advanced
   Colors..., which is the same thing it already does to a hand-set pixel or
   background color.
   Note this re-matches around each preset's own hand-picked ink rather than
   running the preset's background back through derive_theme_colors - the
   presets' pairings are deliberate and stay exactly as authored. */
static void apply_display_colors(menu_context_t *ctx, uint32_t pixel_rgba, uint32_t bg_rgba) {
    apply_display_colors_full(ctx, pixel_rgba, bg_rgba, theme_shadow_for(bg_rgba, pixel_rgba));
}

/* Fills the hex edit control at `edit_id` with whatever ChooseColorA returns,
   and reports whether the user actually picked something.
   This seeds ChooseColorA from that field's current text, falling back to
   black if the text is not valid hex yet.
   A "Choose..." click and hand-typing the hex code can freely mix: either
   one just overwrites the same field.
   The return value matters for IDC_SCREEN_CHOOSE, which re-derives the other
   two colors afterwards - re-deriving on a cancelled pick would throw away
   hand-set colors in exchange for nothing. */
static int choose_color_into_hex_field(HWND hdlg, int edit_id) {
    /* CHOOSECOLOR requires a caller-owned 16-entry custom-color scratch array.
       This array is static so the user's custom-palette additions survive
       between picks, both within one dialog session and across separate menu
       invocations, instead of resetting every time. */
    static COLORREF custom_colors[16] = {0};
    char text[7], new_hex[7];
    uint8_t r, g, b;
    CHOOSECOLORA cc;

    GetDlgItemTextA(hdlg, edit_id, text, sizeof(text));
    memset(&cc, 0, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hdlg;
    cc.lpCustColors = custom_colors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    cc.rgbResult = parse_hex_rgb(text, &r, &g, &b) ? RGB(r, g, b) : RGB(0, 0, 0);
    if (!ChooseColorA(&cc)) {
        return 0;
    }
    snprintf(new_hex, sizeof(new_hex), "%02X%02X%02X", GetRValue(cc.rgbResult), GetGValue(cc.rgbResult),
        GetBValue(cc.rgbResult));
    SetDlgItemTextA(hdlg, edit_id, new_hex);
    return 1;
}

/* lParam payload for advanced_colors_dialog_proc.
   Same pattern as hwid_dialog_data_t: parsed_*_rgba is filled in, and IDOK
   is allowed to close the dialog, only after every hex field passes
   parse_hex_rgb.
   The three hex fields are always the source of truth on OK, never the
   derivation - a hand-set color has to survive. IDC_SCREEN_CHOOSE and
   IDC_REMATCH are the only controls that write a field the user did not
   click into, and both are unambiguous requests to re-match. */
typedef struct {
    char pixel_hex[7];
    char bg_hex[7];
    char shadow_hex[7];
    /* Mirrors IDC_SHADOWS_ENABLE, so the preview can draw the ghosting trail
       only when it is actually switched on rather than advertising one that
       is not there. Applied on OK like every other field here. */
    int show_shadows;
    int custom_visible;
    /* Window heights in pixels. expanded_height is measured from the template
       at WM_INITDIALOG; collapsed_height is derived from IDC_CUSTOM_TOGGLE's
       own position, so neither is a second copy of a resource.rc number. */
    int collapsed_height;
    int expanded_height;
    uint32_t parsed_pixel_rgba;
    uint32_t parsed_bg_rgba;
    uint32_t parsed_shadow_rgba;
} advanced_colors_dialog_data_t;

/* Everything inside (and including) the Custom Colors group box. Hidden
   controls drop out of the tab order on their own, so a collapsed dialog
   cannot be keyboard-navigated into the fields it is hiding. */
static const int advanced_colors_custom_ids[] = {IDC_CUSTOM_GROUP, IDC_CUSTOM_BG_LABEL, IDC_BG_HEX, IDC_BG_CHOOSE,
    IDC_CUSTOM_PIXEL_LABEL, IDC_PIXEL_HEX, IDC_PIXEL_CHOOSE, IDC_CUSTOM_SHADOW_LABEL, IDC_SHADOW_HEX,
    IDC_SHADOW_CHOOSE, IDC_REMATCH};

/* 8x8 sample sprite for the preview swatch, MSB (0x80) = leftmost column.
   A face, rather than a scrap of a real PocketStation frame: it has isolated
   lit pixels, solid runs, and a flat bottom edge, so every part of the scheme
   (ink, background, and the ghosting row under a lit pixel) shows up. */
static const uint8_t preview_sprite[8] = {0x3C, 0x42, 0xA5, 0x81, 0xA5, 0x99, 0x42, 0x3C};

static COLORREF rgba_to_colorref(uint32_t rgba) {
    return RGB((rgba >> 24) & 0xFFu, (rgba >> 16) & 0xFFu, (rgba >> 8) & 0xFFu);
}

/* Reads `edit_id`'s current text as a color, falling back to `fallback` while
   the field is mid-edit and does not parse yet. */
static uint32_t hex_field_rgba(HWND hdlg, int edit_id, uint32_t fallback) {
    char text[7];
    uint8_t r, g, b;
    GetDlgItemTextA(hdlg, edit_id, text, sizeof(text));
    return parse_hex_rgb(text, &r, &g, &b) ? RGBA_PACK(r, g, b) : fallback;
}

/* Draws the IDC_COLOR_PREVIEW swatch: preview_sprite rendered exactly the way
   render_framebuffer renders the real LCD, in whatever colors the three hex
   fields currently hold. This is the dialog's answer to "what do these three
   look like together", which is the actual question a color picker leaves
   unanswered. */
static void draw_color_preview(HWND hdlg, const DRAWITEMSTRUCT *dis) {
    advanced_colors_dialog_data_t *data = (advanced_colors_dialog_data_t *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
    uint32_t bg_rgba = hex_field_rgba(hdlg, IDC_BG_HEX, DISPLAY_BG_CLASSIC);
    uint32_t pixel_rgba = hex_field_rgba(hdlg, IDC_PIXEL_HEX, DISPLAY_PIXEL_CLASSIC);
    uint32_t shadow_rgba = hex_field_rgba(hdlg, IDC_SHADOW_HEX, DISPLAY_SHADOW_COLOR);
    RECT rc = dis->rcItem;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    /* 8 sprite rows plus the ghosting row below them, with the rest of the
       height left as margin so the background reads as a screen. */
    int cell = height / 12;
    int origin_x, origin_y, row, col;
    HBRUSH bg_brush, pixel_brush, shadow_brush;
    int show_shadows = data ? data->show_shadows : 0;

    if (cell < 1) {
        cell = 1;
    }
    origin_x = rc.left + (width - cell * 8) / 2;
    origin_y = rc.top + (height - cell * 9) / 2;

    bg_brush = CreateSolidBrush(rgba_to_colorref(bg_rgba));
    pixel_brush = CreateSolidBrush(rgba_to_colorref(pixel_rgba));
    shadow_brush = CreateSolidBrush(rgba_to_colorref(shadow_rgba));
    FillRect(dis->hDC, &rc, bg_brush);
    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++) {
            RECT cell_rc;
            int lit = (preview_sprite[row] >> (7 - col)) & 1;
            int lit_below = row < 7 ? (preview_sprite[row + 1] >> (7 - col)) & 1 : 0;
            int draw_row;
            if (!lit) {
                continue;
            }
            /* Same two cases as render_framebuffer's second pass: the ghost
               lands one row below a lit pixel, and only where that row is
               dark, so two stacked lit pixels never dim each other. */
            for (draw_row = 0; draw_row < 2; draw_row++) {
                if (draw_row == 1 && (lit_below || !show_shadows)) {
                    continue;
                }
                cell_rc.left = origin_x + col * cell;
                cell_rc.top = origin_y + (row + draw_row) * cell;
                cell_rc.right = cell_rc.left + cell;
                cell_rc.bottom = cell_rc.top + cell;
                FillRect(dis->hDC, &cell_rc, draw_row == 0 ? pixel_brush : shadow_brush);
            }
        }
    }
    DeleteObject(bg_brush);
    DeleteObject(pixel_brush);
    DeleteObject(shadow_brush);
}

/* Refills the active pixel and sprite shadow fields from whatever IDC_BG_HEX
   currently holds. Only IDC_SCREEN_CHOOSE and IDC_REMATCH call this, so a
   color set by hand is never overwritten behind the user's back.
   A field that does not parse is left alone rather than treated as an error:
   there is nothing to derive from a half-typed hex code, and the user is
   mid-edit, not mistaken. */
static void rematch_derived_fields(HWND hdlg) {
    char text[7], hex[7];
    uint8_t r, g, b;
    uint32_t pixel_rgba, shadow_rgba;
    GetDlgItemTextA(hdlg, IDC_BG_HEX, text, sizeof(text));
    if (!parse_hex_rgb(text, &r, &g, &b)) {
        return;
    }
    derive_theme_colors(RGBA_PACK(r, g, b), &pixel_rgba, &shadow_rgba);
    format_rgba_hex(pixel_rgba, hex, sizeof(hex));
    SetDlgItemTextA(hdlg, IDC_PIXEL_HEX, hex);
    format_rgba_hex(shadow_rgba, hex, sizeof(hex));
    SetDlgItemTextA(hdlg, IDC_SHADOW_HEX, hex);
}

static void set_custom_colors_visible(HWND hdlg, advanced_colors_dialog_data_t *data, int visible) {
    RECT window_rect;
    size_t i;
    for (i = 0; i < sizeof(advanced_colors_custom_ids) / sizeof(advanced_colors_custom_ids[0]); i++) {
        ShowWindow(GetDlgItem(hdlg, advanced_colors_custom_ids[i]), visible ? SW_SHOW : SW_HIDE);
    }
    SetDlgItemTextA(hdlg, IDC_CUSTOM_TOGGLE, visible ? "Custom Colors <<" : "Custom Colors >>");
    GetWindowRect(hdlg, &window_rect);
    SetWindowPos(hdlg, NULL, 0, 0, window_rect.right - window_rect.left,
        visible ? data->expanded_height : data->collapsed_height, SWP_NOMOVE | SWP_NOZORDER);
    data->custom_visible = visible;
}

static INT_PTR CALLBACK advanced_colors_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG: {
        advanced_colors_dialog_data_t *data = (advanced_colors_dialog_data_t *)lparam;
        RECT window_rect, client, toggle, margin = {0, 0, 0, 7};
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)data);

        /* Measure before anything is hidden or resized: the template is
           authored at its expanded size (see resource.rc). The collapsed size
           ends one dialog-unit margin below the Custom Colors toggle row,
           which is read back off the live control instead of hardcoded again
           here. */
        GetWindowRect(hdlg, &window_rect);
        data->expanded_height = window_rect.bottom - window_rect.top;
        GetClientRect(hdlg, &client);
        GetWindowRect(GetDlgItem(hdlg, IDC_CUSTOM_TOGGLE), &toggle);
        MapWindowPoints(NULL, hdlg, (POINT *)&toggle, 2);
        MapDialogRect(hdlg, &margin);
        data->collapsed_height =
            (data->expanded_height - (client.bottom - client.top)) + toggle.bottom + margin.bottom;

        SetDlgItemTextA(hdlg, IDC_BG_HEX, data->bg_hex);
        SetDlgItemTextA(hdlg, IDC_PIXEL_HEX, data->pixel_hex);
        SetDlgItemTextA(hdlg, IDC_SHADOW_HEX, data->shadow_hex);
        SendDlgItemMessageA(hdlg, IDC_BG_HEX, EM_SETLIMITTEXT, 6, 0);
        SendDlgItemMessageA(hdlg, IDC_PIXEL_HEX, EM_SETLIMITTEXT, 6, 0);
        SendDlgItemMessageA(hdlg, IDC_SHADOW_HEX, EM_SETLIMITTEXT, 6, 0);
        CheckDlgButton(hdlg, IDC_SHADOWS_ENABLE, data->show_shadows ? BST_CHECKED : BST_UNCHECKED);
        /* Always opens collapsed. The whole point of matching the other two
           colors is that the common case is one decision, not three. */
        set_custom_colors_visible(hdlg, data, 0);
        return TRUE;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lparam;
        if (dis->CtlID == IDC_COLOR_PREVIEW) {
            draw_color_preview(hdlg, dis);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        advanced_colors_dialog_data_t *data = (advanced_colors_dialog_data_t *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
        if (HIWORD(wparam) == EN_CHANGE) {
            int edit_id = LOWORD(wparam);
            if (edit_id != IDC_BG_HEX && edit_id != IDC_PIXEL_HEX && edit_id != IDC_SHADOW_HEX) {
                break;
            }
            /* Typing a color only repaints the preview. Editing the screen
               color by hand deliberately does *not* re-derive the other two:
               inside the Custom Colors group all three fields are the user's,
               and IDC_REMATCH is there to re-match them on request. */
            InvalidateRect(GetDlgItem(hdlg, IDC_COLOR_PREVIEW), NULL, TRUE);
            return TRUE;
        }
        switch (LOWORD(wparam)) {
        case IDC_SCREEN_CHOOSE:
            /* The one-color path: pick a screen color and take the matching
               pixel and shadow colors with it. Nothing happens on a cancelled
               pick, so this can never quietly discard hand-set colors. */
            if (choose_color_into_hex_field(hdlg, IDC_BG_HEX)) {
                rematch_derived_fields(hdlg);
            }
            return TRUE;
        case IDC_BG_CHOOSE:
            /* Each of these writes its own field with SetDlgItemTextA, so the
               EN_CHANGE path above is what repaints the preview. */
            choose_color_into_hex_field(hdlg, IDC_BG_HEX);
            return TRUE;
        case IDC_PIXEL_CHOOSE:
            choose_color_into_hex_field(hdlg, IDC_PIXEL_HEX);
            return TRUE;
        case IDC_SHADOW_CHOOSE:
            choose_color_into_hex_field(hdlg, IDC_SHADOW_HEX);
            return TRUE;
        case IDC_REMATCH:
            rematch_derived_fields(hdlg);
            return TRUE;
        case IDC_SHADOWS_ENABLE:
            /* AUTOCHECKBOX has already flipped its own state by the time this
               arrives; this just mirrors it so the preview matches. */
            data->show_shadows = IsDlgButtonChecked(hdlg, IDC_SHADOWS_ENABLE) == BST_CHECKED;
            InvalidateRect(GetDlgItem(hdlg, IDC_COLOR_PREVIEW), NULL, TRUE);
            return TRUE;
        case IDC_CUSTOM_TOGGLE:
            set_custom_colors_visible(hdlg, data, !data->custom_visible);
            return TRUE;
        case IDOK: {
            char pixel_text[7], bg_text[7], shadow_text[7];
            uint8_t px_r, px_g, px_b, bg_r, bg_g, bg_b, sh_r, sh_g, sh_b;
            GetDlgItemTextA(hdlg, IDC_PIXEL_HEX, pixel_text, sizeof(pixel_text));
            GetDlgItemTextA(hdlg, IDC_BG_HEX, bg_text, sizeof(bg_text));
            GetDlgItemTextA(hdlg, IDC_SHADOW_HEX, shadow_text, sizeof(shadow_text));
            if (!parse_hex_rgb(pixel_text, &px_r, &px_g, &px_b) || !parse_hex_rgb(bg_text, &bg_r, &bg_g, &bg_b)
                || !parse_hex_rgb(shadow_text, &sh_r, &sh_g, &sh_b)) {
                MessageBoxA(hdlg, "Every color needs exactly 6 hex digits (0-9, A-F), e.g. \"1A2B3C\".",
                    "pokketstation", MB_ICONERROR);
                return TRUE;
            }
            data->parsed_pixel_rgba = RGBA_PACK(px_r, px_g, px_b);
            data->parsed_bg_rgba = RGBA_PACK(bg_r, bg_g, bg_b);
            data->parsed_shadow_rgba = RGBA_PACK(sh_r, sh_g, sh_b);
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static void prompt_advanced_colors(menu_context_t *ctx) {
    advanced_colors_dialog_data_t data;
    memset(&data, 0, sizeof(data));
    format_rgba_hex(*ctx->pixel_rgba, data.pixel_hex, sizeof(data.pixel_hex));
    format_rgba_hex(*ctx->bg_rgba, data.bg_hex, sizeof(data.bg_hex));
    format_rgba_hex(*ctx->shadow_rgba, data.shadow_hex, sizeof(data.shadow_hex));
    data.show_shadows = *ctx->show_shadows;
    if (DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_ADVANCED_COLORS), ctx->hwnd,
            advanced_colors_dialog_proc, (LPARAM)&data) != IDOK) {
        return;
    }
    /* The sprite shadow toggle is set here rather than through a helper of
       its own so that all four values this dialog owns land in one
       save_settings, instead of writing settings.cfg twice for one OK. */
    *ctx->show_shadows = data.show_shadows;
    ctx->settings->show_shadows = data.show_shadows;
    apply_display_colors_full(ctx, data.parsed_pixel_rgba, data.parsed_bg_rgba, data.parsed_shadow_rgba);
}

/* IDD_CAPTURE_PROMPT is purely a static text display.
   It has no buttons, so this handler only lets the default dialog
   handling run. */
static INT_PTR CALLBACK capture_prompt_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    (void)hdlg;
    (void)wparam;
    (void)lparam;
    return msg == WM_INITDIALOG ? TRUE : FALSE;
}

/* Blocks by draining SDL's own event queue directly.
   This is the same way DialogBoxParamA blocks the caller with its own
   message loop.
   Returns the pressed key once the user presses one.
   Returns SDL_SCANCODE_UNKNOWN, treated as "cancelled", if the user presses
   Esc or closes the window.

   This uses SDL_PollEvent instead of a native dialog or message loop.
   Capturing a raw Win32 WM_KEYDOWN and converting it to an SDL_Scancode by
   hand would mean reimplementing SDL's own per-platform scancode table.
   That table is nontrivial. button_scancodes[].scancode and
   SDL_GetKeyboardState both already rely on SDL to get it right.
   Asking SDL directly is safer, the same way normal gameplay input already does.

   A real SDL_QUIT during this wait cannot cleanly bubble back out through
   the nested call stack that led here (WM_COMMAND handler, then this
   function). This function treats SDL_QUIT as a cancel too.
   The user's next close attempt proceeds normally after that, since this
   function only ever blocks for a single keypress. */
static SDL_Scancode capture_next_key(HWND hwnd, const char *button_name) {
    char message[160];
    HWND prompt;
    RECT owner_rect, prompt_rect;
    int have_result = 0;
    SDL_Scancode result = SDL_SCANCODE_UNKNOWN;

    /* This uses CreateDialogParamA (modeless), not DialogBoxParamA.
       A modal dialog would block this function from ever reaching the
       SDL_PollEvent loop below. MessageBoxA had this same problem.
       This shows the dialog via SW_SHOWNOACTIVATE, so it never takes
       activation or keyboard focus away from `hwnd`.
       Without SW_SHOWNOACTIVATE, Windows' own menu-mnemonic handling ate the
       pressed key (heard as the system beep) instead of it ever reaching SDL
       as a real SDL_KEYDOWN. */
    prompt = CreateDialogParamA(
        GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_CAPTURE_PROMPT), hwnd, capture_prompt_dialog_proc, 0);
    snprintf(message, sizeof(message), "Press the key you want to use for %s.\n\nPress Esc to cancel.", button_name);
    SetDlgItemTextA(prompt, IDC_CAPTURE_PROMPT_TEXT, message);
    GetWindowRect(hwnd, &owner_rect);
    GetWindowRect(prompt, &prompt_rect);
    SetWindowPos(prompt, HWND_TOP,
        owner_rect.left + ((owner_rect.right - owner_rect.left) - (prompt_rect.right - prompt_rect.left)) / 2,
        owner_rect.top + ((owner_rect.bottom - owner_rect.top) - (prompt_rect.bottom - prompt_rect.top)) / 2, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(prompt, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    while (!have_result) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_KEYDOWN) {
                result = event.key.keysym.scancode == SDL_SCANCODE_ESCAPE ? SDL_SCANCODE_UNKNOWN
                                                                           : event.key.keysym.scancode;
                have_result = 1;
                break;
            }
            if (event.type == SDL_QUIT) {
                have_result = 1;
                break;
            }
        }
        if (!have_result) {
            SDL_Delay(10);
        }
    }
    DestroyWindow(prompt);
    return result;
}

static INT_PTR CALLBACK remap_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG: {
        button_binding_t *bindings = (button_binding_t *)lparam;
        int i;
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)bindings);
        for (i = 0; i < REMAP_BINDING_COUNT; i++) {
            /* SDL_GetScancodeName(SDL_SCANCODE_UNKNOWN) returns "".
               Show a label the user will recognize as "this row lost its key to a
               clash" (see prompt_remap_controls), instead of a blank label. */
            const char *name =
                bindings[i].scancode == SDL_SCANCODE_UNKNOWN ? "(unbound)" : SDL_GetScancodeName(bindings[i].scancode);
            SetDlgItemTextA(hdlg, IDC_REMAP_LABEL_BASE + i, name);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int cmd = LOWORD(wparam);
        if (cmd == IDOK || cmd == IDCANCEL) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        if (cmd >= IDC_REMAP_CHANGE_BASE && cmd < IDC_REMAP_CHANGE_BASE + REMAP_BINDING_COUNT) {
            /* 100+ is well clear of any real IDOK/IDCANCEL/control ID.
               prompt_remap_controls uses this range to tell "row N's Change... was
               clicked" apart from a plain close. */
            EndDialog(hdlg, 100 + (cmd - IDC_REMAP_CHANGE_BASE));
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

/* Shows the current REMAP_BINDING_COUNT bindings, each with a per-row
   "Change..." button.
   Clicking a "Change..." button closes this dialog (see remap_dialog_proc).
   This lets capture_next_key below capture the actual keypress, outside of
   any native dialog's own keyboard-navigation message loop.
   This function then reopens the dialog to show the result, and to allow
   changing another row.
   This loops until the user clicks Close. */
static void prompt_remap_controls(menu_context_t *ctx) {
    for (;;) {
        INT_PTR result = DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_REMAP_CONTROLS), ctx->hwnd,
            remap_dialog_proc, (LPARAM)ctx->button_scancodes);
        int index;
        SDL_Scancode captured;
        if (result < 100) {
            break;
        }
        index = (int)(result - 100);
        captured = capture_next_key(ctx->hwnd, ctx->button_scancodes[index].display_name);
        if (captured != SDL_SCANCODE_UNKNOWN) {
            int i;
            /* Two rows cannot share a key.
               Whichever other row previously held this key becomes unbound.
               Otherwise, both rows would silently point at the same physical key.
               SDL_GetKeyboardState could then never tell the two rows apart. */
            for (i = 0; i < REMAP_BINDING_COUNT; i++) {
                if (i != index && ctx->button_scancodes[i].scancode == captured) {
                    ctx->button_scancodes[i].scancode = SDL_SCANCODE_UNKNOWN;
                }
            }
            ctx->button_scancodes[index].scancode = captured;
            save_key_bindings(ctx->settings, ctx->button_scancodes);
            save_settings(ctx->settings, ctx->settings_path);
        }
    }
}

static INT_PTR CALLBACK about_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextA(hdlg, IDC_ABOUT_VERSION, "pokketstation " POKKETSTATION_VERSION);
        return TRUE;
    case WM_NOTIFY: {
        NMHDR *nmhdr = (NMHDR *)lparam;
        if (nmhdr->idFrom == IDC_ABOUT_LINK && (nmhdr->code == NM_CLICK || nmhdr->code == NM_RETURN)) {
            ShellExecuteA(hdlg, "open", "https://github.com/mentalfoundry/pokketstation", NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        if (nmhdr->idFrom == IDC_ABOUT_LICENSE_LINK && (nmhdr->code == NM_CLICK || nmhdr->code == NM_RETURN)) {
            ShellExecuteA(hdlg, "open", "https://www.gnu.org/licenses/gpl-3.0.html", NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void show_about(menu_context_t *ctx) {
    DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_ABOUT), ctx->hwnd, about_dialog_proc, 0);
}

/* Handlers for IR Link > Host Session, Connect, and Disconnect.
   These use one fixed well-known pipe name, IR_LINK_DEFAULT_PIPE_NAME, rather than a dialog that asks for one.
   That is the simplest thing that works for two instances on one machine.
   See ir_link.h for the transport itself. */
static void ir_link_host_from_menu(menu_context_t *ctx) {
    ir_link_host(ctx->ir_link, IR_LINK_DEFAULT_PIPE_NAME);
    ir_link_refresh_title(ctx);
}

static void ir_link_connect_from_menu(menu_context_t *ctx) {
    ir_link_connect(ctx->ir_link, IR_LINK_DEFAULT_PIPE_NAME);
    ir_link_refresh_title(ctx);
}

static void ir_link_disconnect_from_menu(menu_context_t *ctx) {
    ir_link_disconnect(ctx->ir_link);
    ir_link_refresh_title(ctx);
}

/* Which of the three color presets is currently in effect, as its menu
   command ID, or 0 for none.

   There is no stored "current scheme" to read: a scheme is just the three
   colors it produced, and Advanced Colors... can leave those at any values at
   all. So the active preset is recovered by asking which preset, if applied
   right now, would produce exactly what is already in effect. The shadow is
   part of that comparison because a preset sets all three (see
   apply_display_colors) - a hand-picked shadow over Classic's ink and
   background is no longer Classic, and should not claim to be.

   Returning 0 for a custom scheme is deliberate: the menu then shows no
   preset checked, which is honest, rather than checking whichever one the
   colors were last derived from. */
static int active_color_preset_id(const menu_context_t *ctx) {
    static const struct {
        int id;
        uint32_t pixel;
        uint32_t bg;
    } presets[] = {
        {ID_COLORS_CLASSIC, DISPLAY_PIXEL_CLASSIC, DISPLAY_BG_CLASSIC},
        {ID_COLORS_STANDARD, DISPLAY_PIXEL_LIGHT, DISPLAY_BG_LIGHT},
        {ID_COLORS_REVERSED, DISPLAY_PIXEL_DARK, DISPLAY_BG_DARK},
    };
    for (int i = 0; i < (int)(sizeof(presets) / sizeof(presets[0])); i++) {
        if (*ctx->pixel_rgba == presets[i].pixel && *ctx->bg_rgba == presets[i].bg &&
            *ctx->shadow_rgba == theme_shadow_for(presets[i].bg, presets[i].pixel)) {
            return presets[i].id;
        }
    }
    return 0;
}

/* CheckMenuRadioItem wants the menu that directly contains the group, and
   both override groups plus Colors live in nested popups. Rather than hard-code
   submenu positions, which would break the moment the menu is reordered, find
   the containing popup by command ID. */
static HMENU find_menu_containing(HMENU menu, UINT id) {
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; i++) {
        HMENU sub = GetSubMenu(menu, i);
        if (sub != NULL) {
            HMENU found = find_menu_containing(sub, id);
            if (found != NULL) {
                return found;
            }
        } else if (GetMenuItemID(menu, i) == id) {
            return menu;
        }
    }
    return NULL;
}

/* Brings every checkable menu item in line with the current state.

   This is called after each menu command rather than from WM_INITMENUPOPUP,
   which would be the natural place for it. SDL2 invokes its Windows message
   hook from inside its own PeekMessage loop, so the hook only ever sees
   POSTED messages. WM_COMMAND from a menu is posted, which is why the command
   handlers work; WM_INITMENUPOPUP is sent straight to the window procedure by
   the menu system and never enters the queue, so a handler for it there never
   runs at all. Catching it would mean subclassing the window.

   Syncing eagerly is enough because nothing changes this state except the
   commands below: the overrides are set from the menu, the color scheme is set
   from the menu or from the dialog one of those commands opens, and whether
   the override groups are usable at all depends on the loaded BIOS, which also
   only changes via a menu command. */
static void sync_menu_state(menu_context_t *ctx) {
    HMENU root = GetMenu(ctx->hwnd);
    if (root == NULL) {
        return;
    }

    /* The override addresses are only valid on a BIOS revision this project
       has traced; on anything else the group is greyed rather than left
       clickable (see psemu_settings_offsets_known). */
    int usable = psemu_settings_offsets_known(ctx->ps);
    static const int datetime_ids[] = {ID_TOOLS_DATETIME_DEFAULT, ID_TOOLS_DATETIME_OS};

    HMENU datetime_menu = find_menu_containing(root, ID_TOOLS_DATETIME_DEFAULT);
    if (datetime_menu != NULL) {
        for (int i = 0; i < (int)(sizeof(datetime_ids) / sizeof(datetime_ids[0])); i++) {
            EnableMenuItem(datetime_menu, datetime_ids[i], MF_BYCOMMAND | (usable ? MF_ENABLED : MF_GRAYED));
        }
        CheckMenuRadioItem(
            datetime_menu, ID_TOOLS_DATETIME_DEFAULT, ID_TOOLS_DATETIME_OS,
            datetime_ids[ctx->settings->datetime_override], MF_BYCOMMAND);
    }

    /* Tools > Sound > Volume is this application's own output level, so unlike
       the group above it is never greyed: it works with no BIOS loaded at all.
       A stored percent that is not a whole 10% step (only reachable by hand-
       editing settings.cfg) rounds to the nearest item the menu can show. The
       stored value itself is left alone - the menu just cannot draw it. */
    HMENU volume_menu = find_menu_containing(root, ID_TOOLS_SOUND_BASE);
    if (volume_menu != NULL) {
        int step = (clamp_master_volume(ctx->settings->master_volume) + 5) / 10;
        CheckMenuRadioItem(
            volume_menu, ID_TOOLS_SOUND_BASE, ID_TOOLS_SOUND_LAST, ID_TOOLS_SOUND_BASE + step, MF_BYCOMMAND);
    }

    /* Tools > Sound > Speaker sits beside Volume rather than inside it, so it
       needs its own lookup - find_menu_containing returns the popup that
       directly holds the group, and these are two different popups. Never
       greyed, for the same reason as Volume: it is this application's own
       output filtering and needs no BIOS. */
    HMENU speaker_menu = find_menu_containing(root, ID_TOOLS_SPEAKER_BASE);
    if (speaker_menu != NULL) {
        CheckMenuRadioItem(speaker_menu, ID_TOOLS_SPEAKER_BASE, ID_TOOLS_SPEAKER_LAST,
            ID_TOOLS_SPEAKER_BASE + clamp_speaker_sim(ctx->settings->speaker_sim), MF_BYCOMMAND);
    }

    /* The radio range stops at ID_COLORS_CLASSIC so "Advanced Colors..." stays
       an action item rather than joining the group - it opens a dialog, it is
       not a scheme you can be "on". A custom scheme checks nothing, so every
       preset is cleared first instead of leaving whichever was last checked. */
    HMENU colors_menu = find_menu_containing(root, ID_COLORS_CLASSIC);
    if (colors_menu != NULL) {
        static const int color_ids[] = {ID_COLORS_CLASSIC, ID_COLORS_STANDARD, ID_COLORS_REVERSED};
        for (int i = 0; i < (int)(sizeof(color_ids) / sizeof(color_ids[0])); i++) {
            CheckMenuItem(colors_menu, color_ids[i], MF_BYCOMMAND | MF_UNCHECKED);
        }
        int preset = active_color_preset_id(ctx);
        if (preset != 0) {
            CheckMenuRadioItem(colors_menu, ID_COLORS_STANDARD, ID_COLORS_CLASSIC, preset, MF_BYCOMMAND);
        }
    }
}

/* Installed via SDL_SetWindowsMessageHook.
   Fires synchronously from within SDL_PollEvent's own message pump, on
   the same thread, so no locking is needed.
   It is safe to mutate *ctx and call psemu_* functions directly here. */
static void SDLCALL handle_windows_message(void *userdata, void *hwnd, unsigned int message, Uint64 wparam,
    Sint64 lparam) {
    (void)hwnd;
    (void)lparam;
    menu_context_t *ctx = (menu_context_t *)userdata;
    if (message != WM_COMMAND) {
        return;
    }
    /* Tools > Sound > Volume is one contiguous ID range rather than a named ID per step
       (see resource.h), so it is matched here instead of as eleven cases in the
       switch below. The main loop reads settings->master_volume every frame, so
       storing it is the whole of applying it. */
    if (LOWORD(wparam) >= ID_TOOLS_SOUND_BASE && LOWORD(wparam) <= ID_TOOLS_SOUND_LAST) {
        ctx->settings->master_volume = (int)(LOWORD(wparam) - ID_TOOLS_SOUND_BASE) * 10;
        save_settings(ctx->settings, ctx->settings_path);
        sync_menu_state(ctx);
        return;
    }
    /* Tools > Sound > Speaker, matched the same way and for the same reason.
       Storing it is likewise all of applying it: the main loop reads
       settings->speaker_sim every frame, and the filter rebuilds itself when it
       sees the value change (see apply_speaker_filter). */
    if (LOWORD(wparam) >= ID_TOOLS_SPEAKER_BASE && LOWORD(wparam) <= ID_TOOLS_SPEAKER_LAST) {
        ctx->settings->speaker_sim = (int)(LOWORD(wparam) - ID_TOOLS_SPEAKER_BASE);
        save_settings(ctx->settings, ctx->settings_path);
        sync_menu_state(ctx);
        return;
    }
    /* File > Save State and File > Load State, one contiguous ID range each
       (see resource.h), matched the same way as the two Sound groups above.
       Slot 0 of each is the quick slot, so these two checks also cover what
       the Save State/Load State hotkeys do. */
    if (LOWORD(wparam) >= ID_FILE_SAVE_SLOT_BASE && LOWORD(wparam) <= ID_FILE_SAVE_SLOT_LAST) {
        save_state_to_slot(ctx, (int)(LOWORD(wparam) - ID_FILE_SAVE_SLOT_BASE));
        return;
    }
    if (LOWORD(wparam) >= ID_FILE_LOAD_SLOT_BASE && LOWORD(wparam) <= ID_FILE_LOAD_SLOT_LAST) {
        load_state_from_slot(ctx, (int)(LOWORD(wparam) - ID_FILE_LOAD_SLOT_BASE));
        return;
    }
    switch (LOWORD(wparam)) {
    case ID_FILE_OPEN_BIOS:
        prompt_open_bios(ctx);
        break;
    case ID_FILE_OPEN_APP:
        prompt_open_app(ctx);
        break;
    case ID_FILE_RESET:
        reset_emulation(ctx);
        break;
    case ID_FILE_EXIT:
        *ctx->running = 0;
        break;
    case ID_TOOLS_EDIT_HWID:
        prompt_edit_hardware_id(ctx);
        break;
    case ID_TOOLS_REMAP_CONTROLS:
        prompt_remap_controls(ctx);
        break;
    /* Switching to Default just stops the frontend re-applying its value.
       Whatever the BIOS last had stays put, and its system menus work
       normally again from that point on. */
    case ID_TOOLS_DATETIME_DEFAULT:
    case ID_TOOLS_DATETIME_OS:
        ctx->settings->datetime_override = (int)(LOWORD(wparam) - ID_TOOLS_DATETIME_DEFAULT);
        save_settings(ctx->settings, ctx->settings_path);
        break;
    case ID_VIEW_NATIVE_SIZE:
        resize_client_to_scale(ctx->hwnd, 1);
        break;
    case ID_VIEW_DOUBLE_SIZE:
        resize_client_to_scale(ctx->hwnd, 2);
        break;
    case ID_COLORS_STANDARD:
        apply_display_colors(ctx, DISPLAY_PIXEL_LIGHT, DISPLAY_BG_LIGHT);
        break;
    case ID_COLORS_REVERSED:
        apply_display_colors(ctx, DISPLAY_PIXEL_DARK, DISPLAY_BG_DARK);
        break;
    case ID_COLORS_CLASSIC:
        apply_display_colors(ctx, DISPLAY_PIXEL_CLASSIC, DISPLAY_BG_CLASSIC);
        break;
    case ID_COLORS_ADVANCED:
        prompt_advanced_colors(ctx);
        break;
    case ID_HELP_ABOUT:
        show_about(ctx);
        break;
    case ID_IR_HOST:
        ir_link_host_from_menu(ctx);
        break;
    case ID_IR_CONNECT:
        ir_link_connect_from_menu(ctx);
        break;
    case ID_IR_DISCONNECT:
        ir_link_disconnect_from_menu(ctx);
        break;
    }
    /* Every command that can change a checkable item's state runs through the
       switch above, so one sync here covers all of them - including the color
       scheme changing inside the Advanced Colors... dialog, and the override
       groups becoming usable after a BIOS is loaded. See sync_menu_state for
       why this is not done from WM_INITMENUPOPUP. */
    sync_menu_state(ctx);
}

static int lcd_bit_on(const uint8_t *fb, int row, int col) {
    int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
    int bit_index = col % 8;
    return (fb[byte_index] >> bit_index) & 1;
}

static void render_framebuffer(const psemu_t *ps, uint32_t *pixels, uint32_t pixel_rgba, uint32_t bg_rgba,
    int show_shadows, uint32_t shadow_rgba) {
    const uint8_t *fb = psemu_get_framebuffer(ps);
    for (int row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            /* SDL_PIXELFORMAT_RGBA8888 packs a 32-bit value as (R<<24)|(G<<16)|(B<<8)|A,
               regardless of host endianness.
               Example: 0xFF000000 means R=0xFF, G=0, B=0, A=0. This is pure red with
               transparent alpha, not opaque black.
               pixel_rgba and bg_rgba (see RGBA_PACK) already account for this layout.
               Callers must not pass plain 0xRRGGBB values here. */
            pixels[row * PSEMU_LCD_WIDTH + col] = lcd_bit_on(fb, row, col) ? pixel_rgba : bg_rgba;
        }
    }
    if (!show_shadows) {
        return;
    }
    /* Approximates the faint "ghosting" a real late-90s STN/passive-matrix LCD
       shows trailing a lit pixel.
       This ghosting comes from slow crystal response, not a real drop shadow.
       This draws shadow_rgba (matched to the active color scheme, and settable
       by hand from View > Colors > Advanced Colors...) one row below each lit
       pixel.
       This runs as a second pass, so it never overwrites an actually-lit pixel.
       This checks against `fb` directly, not the just-written output.
       Two adjacent lit source pixels must never dim each other. */
    for (int row = 0; row < PSEMU_LCD_HEIGHT - 1; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            if (lcd_bit_on(fb, row, col) && !lcd_bit_on(fb, row + 1, col)) {
                pixels[(row + 1) * PSEMU_LCD_WIDTH + col] = shadow_rgba;
            }
        }
    }
}

int main(int argc, char **argv) {
    char exe_dir[900];
    char settings_config_path[1024];
    /* Mutable, not just const char * into argv.
       File > Load BIOS.../Open App can overwrite the current path in place
       after a successful reload.
       Used for the F12/crash diagnostic report's "bios:"/"app:" lines. */
    char bios_path[1024];
    char app_path[1024];
    const char *positional[2];
    int npositional = 0;
    int saw_console_flag = 0;
    int saw_no_console_flag = 0;
    int show_console;
    app_settings_t settings;
    int i;

    {
        /* Registers SysLink, Help > About's clickable repo link.
           This is a no-op for every other dialog in this file.
           Doing this once up front is simpler than threading it through each
           DialogBoxParamA call site individually. */
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LINK_CLASS;
        InitCommonControlsEx(&icc);
    }

    /* Computed regardless of how paths or flags are given below.
       The settings file always lives next to the executable, not next to
       whatever content path the user passed on the command line. */
    get_exe_dir(argv[0], exe_dir, sizeof(exe_dir));
    join_path(settings_config_path, sizeof(settings_config_path), exe_dir, SETTINGS_CONFIG_NAME);
    if (!load_settings(&settings, settings_config_path)) {
        /* This is a first run: settings.cfg did not exist yet.
           Write it out right away with the defaults load_settings just filled in
           (for example, hardware_id).
           Do not wait for some other change (BIOS load, hardware ID edit, etc.)
           to trigger the first save.
           An existing file is left exactly as read here, even if a field happened
           to be blank in it.
           This app rewrites a field only on an actual update, the same as
           everything else in settings.cfg. */
        save_settings(&settings, settings_config_path);
    }

    /* --console and --no-console are the only flags; everything else is a
       positional arg.
       This silently ignores extra positional args beyond 2.
       This matches this parsing's long-standing behavior of reading only the
       first two positional args. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0) {
            saw_console_flag = 1;
        } else if (strcmp(argv[i], "--no-console") == 0) {
            saw_no_console_flag = 1;
        } else if (npositional < 2) {
            positional[npositional++] = argv[i];
        }
    }
    /* An explicit flag always wins, but only for this run.
       Absent either flag, this falls back to the persisted preference from
       settings.cfg (0 if there is no settings file yet).
       A one-off --console/--no-console must not silently rewrite settings.cfg:
       that file only reflects a choice the user made to persist, not whatever
       flag happened to be on the command line this time. There is currently no
       in-app toggle for this value; changing it for good means editing
       settings.cfg's show_console line directly. */
    show_console = saw_console_flag ? 1 : (saw_no_console_flag ? 0 : settings.show_console);

    if (show_console) {
        /* This app is built as a GUI-subsystem executable (see CMakeLists.txt's
           add_executable(... WIN32 ...)).
           No console is attached by default, so every fprintf(stderr, ...) below
           would otherwise go nowhere visible.
           --console opts back into a console, for anyone who wants to see this
           output, for example while testing from a terminal. */
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    if (npositional >= 2) {
        snprintf(bios_path, sizeof(bios_path), "%s", positional[0]);
        snprintf(app_path, sizeof(app_path), "%s", positional[1]);
    } else if (npositional == 0) {
        /* No positional arguments at all means Explorer double-click-launched the
           .exe, rather than a terminal invocation.
           There is no command line to pass a path on, so this falls back first to
           the BIOS path remembered in settings.cfg from a previous run's
           File > Open (or CLI argument), then to a BIOS dump sitting next to the
           .exe.
           settings.cfg does not remember the memory-card/app path; it remembers
           only the BIOS path (see its comment above).
           The app path always falls back to the same next-to-the-.exe convention. */
        if (settings.bios_path[0] != '\0') {
            snprintf(bios_path, sizeof(bios_path), "%s", settings.bios_path);
        } else {
            join_path(bios_path, sizeof(bios_path), exe_dir, "bios.bin");
        }
        join_path(app_path, sizeof(app_path), exe_dir, "memcard.mcr");
    } else {
        fprintf(stderr, "usage: %s [--console|--no-console] <bios.bin> <app.pss | app.mcs | memory-card.mcr>\n",
            argv[0]);
        fprintf(
            stderr,
            "  a %d-byte file is loaded as a raw memory card image (with its own directory) -\n"
            "  navigate and launch apps from the real BIOS menu with the keyboard, same as real\n"
            "  hardware. Anything else is loaded as a single Title Sector app (MCX0/MCX1), tried\n"
            "  either bare (.pss) or wrapped in a single-save directory frame (.mcs).\n",
            PSEMU_FLASH_SIZE);
        return 1;
    }

    /* A missing or invalid BIOS/app-or-card file is no longer fatal.
       The menu bar's File > Load BIOS.../Open App/Card... lets the user
       browse to one after the window comes up.
       This app launches either way, and leaves psemu without one loaded.
       psemu_run no-ops until a BIOS is actually loaded (see psemu_run's
       !ps->has_bios check). */
    size_t bios_size = 0, app_size = 0;
    uint8_t *bios = read_file(bios_path, &bios_size);
    uint8_t *app = read_file(app_path, &app_size);
    if (!bios) {
        fprintf(stderr, "psemu: couldn't read a BIOS dump at %s - launching anyway; use File > Load BIOS...\n",
            bios_path);
    }
    if (!app) {
        fprintf(stderr,
            "psemu: couldn't read an app or memory-card image at %s - launching anyway; use File > Open "
            "App/Card...\n",
            app_path);
    }

    psemu_t *ps = psemu_create();
    if (settings.hardware_id[0] != '\0') {
        uint32_t id;
        if (psemu_parse_hardware_id(settings.hardware_id, &id)) {
            psemu_set_hardware_id(ps, id);
        } else {
            fprintf(stderr,
                "psemu: couldn't parse hardware ID from %s (expected exactly 8 hex digits, e.g. \"EEEEEEEE\") - "
                "ignoring, using the default\n",
                settings_config_path);
        }
    }

    /* Colors > Light/Dark/Classic/Advanced Colors... all funnel through here on
       the next launch (see save_settings above).
       load_settings always fills in a real value for both fields (Classic, on
       a fresh settings.cfg).
       This Light fallback matters only if settings.cfg's content fails to
       parse, for example if it was hand-edited to something invalid. */
    uint32_t pixel_rgba = DISPLAY_PIXEL_LIGHT;
    uint32_t bg_rgba = DISPLAY_BG_LIGHT;
    {
        uint8_t r, g, b;
        if (settings.pixel_color[0] != '\0' && parse_hex_rgb(settings.pixel_color, &r, &g, &b)) {
            pixel_rgba = RGBA_PACK(r, g, b);
        }
        if (settings.bg_color[0] != '\0' && parse_hex_rgb(settings.bg_color, &r, &g, &b)) {
            bg_rgba = RGBA_PACK(r, g, b);
        }
    }
    int show_shadows = settings.show_shadows;
    uint32_t shadow_rgba = DISPLAY_SHADOW_COLOR;
    {
        uint8_t r, g, b;
        if (settings.shadow_color[0] != '\0' && parse_hex_rgb(settings.shadow_color, &r, &g, &b)) {
            shadow_rgba = RGBA_PACK(r, g, b);
        }
    }

    if (bios && psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        fprintf(stderr, "psemu: %s isn't a valid BIOS image (expected %d bytes) - ignoring; use File > Load BIOS...\n",
            bios_path, PSEMU_BIOS_SIZE);
        free(bios);
        bios = NULL;
        bios_size = 0;
    }
    if (app && psemu_load_content(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr,
            "psemu: %s isn't a valid app or memory-card image - ignoring; use File > Open App/Card...\n", app_path);
        free(app);
        app = NULL;
        app_size = 0;
    }
    psemu_reset(ps);

    /* Persist a successfully-loaded BIOS path right away, whether it came from
       a CLI argument or the settings-remembered/next-to-the-.exe default.
       Do not wait for exit. See the settings-file comment above for why.
       A failed load leaves settings.bios_path untouched. */
    if (bios) {
        snprintf(settings.bios_path, sizeof(settings.bios_path), "%s", bios_path);
        save_settings(&settings, settings_config_path);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* This window is resizable.
       The 32x32 framebuffer stretches to fill the entire render target on
       every frame, regardless of its size (render_copy's NULL dstrect below).
       Free-form resizing works without any extra handling.
       View > Native/Double Size are just a shortcut back to a known-good size,
       not the only sizes this app supports. */
    SDL_Window *window = SDL_CreateWindow("pokketstation", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        PSEMU_LCD_WIDTH * SCALE, PSEMU_LCD_HEIGHT * SCALE, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, PSEMU_LCD_WIDTH, PSEMU_LCD_HEIGHT);

    SDL_AudioSpec audio_spec;
    SDL_zero(audio_spec);
    audio_spec.freq = PSEMU_AUDIO_SAMPLE_RATE_HZ;
    audio_spec.format = AUDIO_S16SYS;
    audio_spec.channels = 1;
    audio_spec.samples = 512;
    SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, 0, &audio_spec, NULL, 0);
    if (audio_dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s (continuing without sound)\n", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(audio_dev, 0);
    }

    int running = 1;
    int cpu_faulted_reported = 0;

    ir_link_t ir_link;
    ir_link_init(&ir_link);
    /* After ir_link_init, which zeroes the struct. Off unless settings.cfg asks for it. */
    ir_link.show_diagnostics = settings.ir_link_diagnostics;

    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info)) {
        fprintf(stderr, "SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return 1;
    }
    HWND hwnd = wm_info.info.win.window;

    /* SDL registers its own window class with no icon.
       The exe's resource icon (see resource.rc) is already the taskbar and
       Explorer icon, but it does not show up on the title bar or Alt-Tab on
       its own.
       Load the icon explicitly at both sizes Windows actually asks for, and
       set it on the window directly. */
    HICON icon_big = (HICON)LoadImageA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_MAINICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON icon_small = (HICON)LoadImageA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_MAINICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon_big);
    SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon_small);

    HMENU menu = LoadMenuA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDR_MAINMENU));
    RECT client_before;
    GetClientRect(hwnd, &client_before);
    SetMenu(hwnd, menu);
    /* SetMenu shrinks the client area to make room for the menu bar, unless
       the window grows to compensate.
       Grow the window by exactly however much the client area just shrank.
       This keeps the emulator rendering at its native SCALE-scaled size,
       instead of getting clipped or letterboxed. */
    RECT client_after;
    GetClientRect(hwnd, &client_after);
    int shrink =
        (client_before.bottom - client_before.top) - (client_after.bottom - client_after.top);
    if (shrink > 0) {
        RECT window_rect;
        GetWindowRect(hwnd, &window_rect);
        /* SWP_FRAMECHANGED forces Windows to recompute the menu bar's cached item
           rects for the new window size.
           Without it, the menu bar keeps the geometry it had at SetMenu time,
           before this resize.
           The first click on any menu then opens its dropdown using stale
           coordinates: visibly left-facing instead of the normal right-facing
           direction.
           This wrong direction persists until some later hover or click forces a
           recalculation on its own. */
        SetWindowPos(hwnd, NULL, 0, 0, window_rect.right - window_rect.left,
            (window_rect.bottom - window_rect.top) + shrink, SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    /* Live key-to-PocketStation-button mapping. The main loop polls this
       mapping every frame.
       Fixed order: Up, Down, Left, Right, Fire, Create-Debug-Log, Reset,
       Quick-Save-State, Quick-Load-State.
       This order matches IDC_REMAP_LABEL_BASE/IDC_REMAP_CHANGE_BASE and
       save_key_bindings.
       Not const or static: Tools > Remap Controls... mutates entries in place
       through menu_ctx.button_scancodes, which points at this same array.
       The trailing 4 entries (Create Debug Log, Reset, Save State,
       Load State) are not real PocketStation buttons. Their bit is 0,
       so the polling loop below harmlessly ORs them into nothing.
       The SDL_KEYDOWN checks further down are what actually use their
       scancodes. Each check triggers its action on a real edge, once per
       press, not every frame the key is held. */
    button_binding_t button_scancodes[REMAP_BINDING_COUNT] = {
        {resolve_key_binding(settings.key_up, SDL_SCANCODE_UP), PSEMU_BUTTON_UP, "Up"},
        {resolve_key_binding(settings.key_down, SDL_SCANCODE_DOWN), PSEMU_BUTTON_DOWN, "Down"},
        {resolve_key_binding(settings.key_left, SDL_SCANCODE_LEFT), PSEMU_BUTTON_LEFT, "Left"},
        {resolve_key_binding(settings.key_right, SDL_SCANCODE_RIGHT), PSEMU_BUTTON_RIGHT, "Right"},
        {resolve_key_binding(settings.key_fire, SDL_SCANCODE_Z), PSEMU_BUTTON_FIRE, "Fire/Action"},
        {resolve_key_binding(settings.key_debug_log, SDL_SCANCODE_F12), 0, "Create Debug Log"},
        {resolve_key_binding(settings.key_reset, SDL_SCANCODE_F8), 0, "Reset"},
        {resolve_key_binding(settings.key_quick_save, SDL_SCANCODE_F5), 0, "Save State"},
        {resolve_key_binding(settings.key_quick_load, SDL_SCANCODE_F9), 0, "Load State"},
    };

    fprintf(stderr, "psemu: press %s at any time to write a diagnostic report to a pokketstation_report_*.log file\n",
        SDL_GetScancodeName(button_scancodes[5].scancode));

    menu_context_t menu_ctx;
    menu_ctx.ps = ps;
    menu_ctx.bios = &bios;
    menu_ctx.bios_size = &bios_size;
    menu_ctx.bios_path = bios_path;
    menu_ctx.bios_path_cap = sizeof(bios_path);
    menu_ctx.app = &app;
    menu_ctx.app_size = &app_size;
    menu_ctx.app_path = app_path;
    menu_ctx.app_path_cap = sizeof(app_path);
    menu_ctx.hwnd = hwnd;
    menu_ctx.running = &running;
    menu_ctx.cpu_faulted_reported = &cpu_faulted_reported;
    menu_ctx.pixel_rgba = &pixel_rgba;
    menu_ctx.bg_rgba = &bg_rgba;
    menu_ctx.show_shadows = &show_shadows;
    menu_ctx.button_scancodes = button_scancodes;
    menu_ctx.shadow_rgba = &shadow_rgba;
    menu_ctx.settings = &settings;
    menu_ctx.settings_path = settings_config_path;
    menu_ctx.exe_dir = exe_dir;
    menu_ctx.ir_link = &ir_link;
    SDL_SetWindowsMessageHook(handle_windows_message, &menu_ctx);

    uint32_t pixels[PSEMU_LCD_WIDTH * PSEMU_LCD_HEIGHT];
    int16_t audio_buf[1024];
    /* Lives out here, not inside the loop: its history has to carry across
       frames. See speaker_filter_t. */
    speaker_filter_t speaker_filter;
    speaker_filter_init(&speaker_filter);
    unsigned long frame = 0;

    /* Minimum number of frames a button reads as pressed, once detected.
       This stretches a quick real tap to match the duration confirmed, via
       scripted headless testing, to reliably register with the real BIOS.
       At 32Hz, a real ~40ms tap is only ~1.3 frames.
       If a tap lands awkwardly between two per-frame SDL_GetKeyboardState
       polls, this emulator could see it for only a small fraction of a frame.
       That fraction is too short for the BIOS's own input handling to count
       it as a completed press. */
#define BUTTON_LATCH_FRAMES 5
    int latch_frames_remaining[sizeof(button_scancodes) / sizeof(button_scancodes[0])] = {0};

    /* Reflect the settings just loaded from settings.cfg before the menu is
       ever opened. Afterwards each command keeps it in step; see
       sync_menu_state. */
    sync_menu_state(&menu_ctx);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == button_scancodes[5].scancode) {
                /* On-demand snapshot for manual testing.
                   Press this key the moment something looks wrong (frozen screen, missing
                   sound, garbled graphics), whether or not the CPU has actually faulted.
                   Remappable via Tools > Remap Controls..., default F12.
                   See button_scancodes above. */
                char reason[64];
                snprintf(reason, sizeof(reason), "manual (%s)", SDL_GetScancodeName(button_scancodes[5].scancode));
                write_diagnostic_report(ps, reason, frame, bios_path, app_path);
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == button_scancodes[6].scancode) {
                /* Remappable via Tools > Remap Controls..., default F8. See button_scancodes above. */
                reset_emulation(&menu_ctx);
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == button_scancodes[7].scancode) {
                /* Remappable via Tools > Remap Controls..., default F5. See button_scancodes above.
                   Slot 0, the quick slot: only it has key bindings, so slots 1
                   and 2 cannot be overwritten by a stray keypress. */
                save_state_to_slot(&menu_ctx, 0);
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == button_scancodes[8].scancode) {
                /* Remappable via Tools > Remap Controls..., default F9. See button_scancodes above. */
                load_state_from_slot(&menu_ctx, 0);
            }
        }

        const uint8_t *keys = SDL_GetKeyboardState(NULL);
        uint32_t buttons = 0;
        size_t bi;
        for (bi = 0; bi < sizeof(button_scancodes) / sizeof(button_scancodes[0]); bi++) {
            int held = keys[button_scancodes[bi].scancode] != 0;
            if (held) {
                latch_frames_remaining[bi] = BUTTON_LATCH_FRAMES;
            } else if (latch_frames_remaining[bi] > 0) {
                latch_frames_remaining[bi]--;
            }
            if (held || latch_frames_remaining[bi] > 0) {
                buttons |= button_scancodes[bi].bit;
            }
        }
        psemu_set_buttons(ps, buttons);

        /* This budget is 33000 cycles per frame at a 32Hz refresh, ~1.056MHz effective.
           This reverts an earlier attempt to match rtc.h's RTC_TICK_CYCLES (~4MHz).
           That earlier attempt was an unvalidated guess, matching one uncalibrated
           constant to another.
           Real-hardware testing showed that "fix" made on-screen blinking visibly
           too fast.
           Real hardware runs at a variable clock, up to ~7.995MHz (see
           the CPU_FREQ table in core/src/clk.c, and docs/hardware-notes.md,
           "CLK_MODE").
           psemu_run scales its overall throughput by both the app's
           currently-programmed CLK_MODE, and each instruction's real
           per-instruction cycle cost (see "Memory access timing" in
           docs/hardware-notes.md).
           This fixed per-frame budget is a real-time reference rate, not an
           instruction-count approximation.
           33000 cycles per frame is kept here because real-hardware testing
           confirmed it looks right, not because it is independently derived.
           See dac.h's PSEMU_ASSUMED_CPU_HZ for the matching audio-rate conversion
           (33000 * 32). Keep both values in sync if this ever changes. */
        /* If the CPU hits an opcode this emulator does not recognize, register and
           memory state are no longer meaningful.
           A real, confirmed bug found this way (see docs/hardware-notes.md,
           "Known open questions", "Chocobo World event-screen crash") reaches this
           point after ~1.3 billion instructions of otherwise-correct real gameplay.
           Do not assume this state is harmless just because it has not happened yet.
           Stop stepping the CPU once this trips, freezing on the last good frame.
           Do not silently continue feeding the CPU garbage forever.
           Before this fix, that silent continuation looked to a player like an
           unexplained hang or crash with zero diagnostic information. */
        /* Tools > Date/Time Override.
           This setting lives in emulated RAM that the BIOS's own system menus
           also write, so holding a value means re-applying it rather than
           setting it once. Re-applying here, every frame, also covers reset,
           startup and state load with no extra hooks: there is no event to
           miss, because the next frame corrects whatever changed.

           This is a handful of byte stores against the ~26,500 instructions a
           frame already emulates.

           Guarded on psemu_settings_offsets_known so an untraced BIOS revision
           is left alone entirely, matching the greyed-out menu items.

           Also guarded on psemu_app_running, because "re-apply every frame" is
           only correct while the BIOS shell owns that RAM. Once the BIOS
           dispatches an app off the card, those two bytes belong to the app,
           and continuing to stamp them 32 times a second corrupts it: with
           testdata/YGO_jap.mcr this was enough to make the app reject the PS1
           save on the card and drop to its "ODD DATA" screen, which is how the
           bug was found. The override resumes on its own once the app hands
           control back. */
        if (psemu_settings_offsets_known(ps) && settings.datetime_override == DATETIME_OVERRIDE_OS &&
            !psemu_app_running(ps)) {
            SYSTEMTIME now;
            GetLocalTime(&now);
            /* SYSTEMTIME's wDayOfWeek is 0=Sunday; the RTC's field is
               1=Sunday. psemu_set_datetime declines while the BIOS is
               mid-way through programming the clock, and the next frame
               simply tries again. */
            psemu_set_datetime(
                ps, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, (int)now.wDayOfWeek + 1);
        }

        if (!psemu_cpu_faulted(ps)) {
            psemu_run(ps, 33000);
        } else if (!cpu_faulted_reported) {
            cpu_faulted_reported = 1;
            fprintf(
                stderr, "psemu: CPU hit an unrecognized opcode and has stopped - this is a real emulator bug, "
                        "not something you did. The game is frozen on its last good frame; please report this "
                        "along with what you were doing right before it happened.\n");
            write_diagnostic_report(ps, "cpu fault (unrecognized opcode)", frame, bios_path, app_path);
        }

        /* Drains this frame's new TX edges onto the pipe.
           It also pushes in every RX edge that arrived from the other instance since the last frame.
           Those edges arrive in time for the next frame's psemu_run to process them through ir_tick.
           This is the same one-frame granularity, about 31ms, that every other input path already accepts.
           Buttons are one example. See ir_link.h.
           ir_link's own status can change here with no menu action: a peer connects, or the link fails.
           The title therefore refreshes whenever the status text changes. */
        {
            const char *status_before = ir_link_status_text(&ir_link);
            char status_before_copy[sizeof(((ir_link_t *)0)->status)];
            snprintf(status_before_copy, sizeof(status_before_copy), "%s", status_before);
            ir_link_pump(&ir_link, ps);
            if (strcmp(status_before_copy, ir_link_status_text(&ir_link)) != 0) {
                ir_link_refresh_title(&menu_ctx);
            }
        }

        if (audio_dev != 0) {
            uint32_t n = psemu_get_audio_samples(ps, audio_buf, sizeof(audio_buf) / sizeof(audio_buf[0]));
            if (n > 0) {
                /* Tools > Sound > Volume, read fresh each frame so a change takes effect
                   on the next one. The core is always drained regardless, and
                   silence is still queued at 0%: skipping the queue instead
                   would leave whatever is already buffered playing on, and
                   would drift this instance's audio clock against its frames.
                   The speaker filter runs first so the percentage stays a plain
                   loudness control over whatever the speaker produced, rather
                   than changing how hard the filter's makeup gain drives into
                   its own clamp. */
                apply_speaker_filter(&speaker_filter, audio_buf, n, settings.speaker_sim);
                apply_master_volume(audio_buf, n, clamp_master_volume(settings.master_volume));
                SDL_QueueAudio(audio_dev, audio_buf, n * sizeof(audio_buf[0]));
            }
        }

        render_framebuffer(ps, pixels, pixel_rgba, bg_rgba, show_shadows, shadow_rgba);
        SDL_UpdateTexture(texture, NULL, pixels, PSEMU_LCD_WIDTH * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_Delay(31); /* ~32Hz, matching the real LCD refresh */
        frame++;
    }

    ir_link_disconnect(&ir_link);

    if (audio_dev != 0) {
        SDL_CloseAudioDevice(audio_dev);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    /* No settings save here.
       bios_path and hardware_id are each already persisted immediately at the
       point they last changed (see the settings-file comment above).
       This app never writes show_console at all. See the same comment.
       Nothing is left to flush on exit. */
    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
