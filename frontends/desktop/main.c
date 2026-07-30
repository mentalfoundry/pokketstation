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
#define POKKETSTATION_VERSION "v1.6.0"

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

/* Quick Save/Load State target one file per loaded app/card, named from a
   hash of its content - not its file path - so switching between apps via
   File > Open App/Card... never collides, and each app keeps its own
   most-recent quicksave automatically. Computed fresh on demand rather
   than cached, since the loaded app can change mid-session. */
static void get_quicksave_path(char *out, size_t out_size, const char *exe_dir, const uint8_t *app, size_t app_size) {
    char name[64];
    snprintf(name, sizeof(name), "pokketstation_quicksave_%08x.dat", (unsigned)fnv1a_hash(app, app_size));
    join_path(out, out_size, exe_dir, name);
}

#define QUICKSAVE_MAGIC "PKQS"
#define QUICKSAVE_VERSION 1u

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
     - Whether to show a console window (show_console). There is no in-app
       toggle for this; it is only ever set by hand-editing settings.cfg.
       --console/--no-console override it for a single run, same as any other
       CLI flag, but deliberately do not write back to settings.cfg - a one-off
       flag on the command line is not the same thing as a persisted
       preference, and must not silently overwrite one.

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
    /* Not PocketStation buttons. Trigger reset_emulation/quick_save_state/
       quick_load_state on demand (see button_scancodes in main). */
    char key_reset[32];
    char key_quick_save[32];
    char key_quick_load[32];
} app_settings_t;

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
   This fixed shadow color applies regardless of the active color scheme.
   See View > Sprite Shadows. */
#define DISPLAY_SHADOW_COLOR RGBA_PACK(0x8E, 0x9B, 0x8E)

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
    settings->show_shadows = 0;
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
            } else if (strncmp(line, "show_console=", 13) == 0) {
                settings->show_console = atoi(line + 13) != 0;
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
    fprintf(f, "show_console=%d\n", settings->show_console ? 1 : 0);
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
   plus 4 non-button hotkeys - Create Debug Log, Reset, Quick Save State,
   Quick Load State.
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
   Up/Down/Left/Right/Fire/Create-Debug-Log/Reset/Quick-Save-State/
   Quick-Load-State order (see button_scancodes in main). */
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
    button_binding_t *button_scancodes; /* fixed REMAP_BINDING_COUNT-element Up/Down/Left/Right/Fire/Debug-Log/Reset/Quick-Save/Quick-Load array, see main */
    app_settings_t *settings;
    const char *settings_path;
    const char *exe_dir;
    ir_link_t *ir_link;
} menu_context_t;

/* Shows IR Link's live status as a window-title suffix (e.g. "pokketstation - IR Link: Connected"), or just the
   plain title while idle. Called after every action that can change ir_link's state - host/connect/disconnect
   from the menu, and once per frame in main's loop, since a hosting/connecting link's status can also change on
   its own (a peer connects, or the link errors out) with no menu click involved. */
static void ir_link_refresh_title(menu_context_t *ctx) {
    char title[192];
    if (ir_link_is_active(ctx->ir_link)) {
        snprintf(title, sizeof(title), "pokketstation - IR Link: %s", ir_link_status_text(ctx->ir_link));
    } else {
        snprintf(title, sizeof(title), "pokketstation");
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

/* Restarts the currently loaded BIOS and app/card from a clean state,
   without reloading either file - the same reset psemu_reset already
   performs after a fresh load (see its comment in psemu.c), triggered
   on demand instead of only after a file dialog succeeds. */
static void reset_emulation(menu_context_t *ctx) {
    psemu_reset(ctx->ps);
    drop_ir_link_if_active(ctx);
    *ctx->cpu_faulted_reported = 0;
}

static void quick_save_state(menu_context_t *ctx) {
    char path[1024];
    get_quicksave_path(path, sizeof(path), ctx->exe_dir, *ctx->app, *ctx->app_size);

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
        MessageBoxA(ctx->hwnd, "Couldn't write the quicksave file.", "pokketstation", MB_ICONERROR);
    }
    if (f) {
        fclose(f);
    }
    free(buf);
}

static void quick_load_state(menu_context_t *ctx) {
    char path[1024];
    get_quicksave_path(path, sizeof(path), ctx->exe_dir, *ctx->app, *ctx->app_size);

    FILE *f = fopen(path, "rb");
    if (!f) {
        MessageBoxA(ctx->hwnd, "No quicksave found for the currently loaded app/card.", "pokketstation",
            MB_ICONWARNING);
        return;
    }

    quicksave_header_t header;
    if (fread(&header, 1, sizeof(header), f) != sizeof(header) || memcmp(header.magic, QUICKSAVE_MAGIC, 4) != 0 ||
        header.version != QUICKSAVE_VERSION) {
        MessageBoxA(ctx->hwnd, "Not a valid quicksave file.", "pokketstation", MB_ICONERROR);
        fclose(f);
        return;
    }

    uint32_t current_hash = fnv1a_hash(*ctx->app, *ctx->app_size);
    if (header.app_size != (uint32_t)*ctx->app_size || header.app_hash != current_hash) {
        MessageBoxA(ctx->hwnd, "This quicksave doesn't match the currently loaded app/card.", "pokketstation",
            MB_ICONERROR);
        fclose(f);
        return;
    }

    size_t state_size = psemu_state_size(ctx->ps);
    uint8_t *buf = (uint8_t *)malloc(state_size);
    if (!buf || fread(buf, 1, state_size, f) != state_size || psemu_load_state(ctx->ps, buf, state_size) != PSEMU_OK) {
        MessageBoxA(ctx->hwnd, "Couldn't load the quicksave (wrong build/version?).", "pokketstation", MB_ICONERROR);
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

static void apply_display_colors(menu_context_t *ctx, uint32_t pixel_rgba, uint32_t bg_rgba) {
    *ctx->pixel_rgba = pixel_rgba;
    *ctx->bg_rgba = bg_rgba;
    format_rgba_hex(pixel_rgba, ctx->settings->pixel_color, sizeof(ctx->settings->pixel_color));
    format_rgba_hex(bg_rgba, ctx->settings->bg_color, sizeof(ctx->settings->bg_color));
    save_settings(ctx->settings, ctx->settings_path);
}

static void set_sprite_shadows(menu_context_t *ctx, int enabled) {
    *ctx->show_shadows = enabled;
    ctx->settings->show_shadows = enabled;
    save_settings(ctx->settings, ctx->settings_path);
}

/* Fills the IDC_PIXEL_HEX/IDC_BG_HEX edit control at `edit_id` with whatever
   ChooseColorA returns.
   This seeds ChooseColorA from that field's current text, falling back to
   black if the text is not valid hex yet.
   A "Choose..." click and hand-typing the hex code can freely mix: either
   one just overwrites the same field. */
static void choose_color_into_hex_field(HWND hdlg, int edit_id) {
    /* CHOOSECOLOR requires a caller-owned 16-entry custom-color scratch array.
       This array is static so the user's custom-palette additions survive
       between picks, both within one dialog session and across separate menu
       invocations, instead of resetting every time. */
    static COLORREF custom_colors[16] = {0};
    char text[7];
    uint8_t r, g, b;
    CHOOSECOLORA cc;

    GetDlgItemTextA(hdlg, edit_id, text, sizeof(text));
    memset(&cc, 0, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = hdlg;
    cc.lpCustColors = custom_colors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    cc.rgbResult = parse_hex_rgb(text, &r, &g, &b) ? RGB(r, g, b) : RGB(0, 0, 0);
    if (ChooseColorA(&cc)) {
        char new_hex[7];
        snprintf(new_hex, sizeof(new_hex), "%02X%02X%02X", GetRValue(cc.rgbResult), GetGValue(cc.rgbResult),
            GetBValue(cc.rgbResult));
        SetDlgItemTextA(hdlg, edit_id, new_hex);
    }
}

/* lParam payload for custom_colors_dialog_proc.
   Same pattern as hwid_dialog_data_t: parsed_*_rgba is filled in, and IDOK
   is allowed to close the dialog, only after both hex fields pass
   parse_hex_rgb. */
typedef struct {
    char pixel_hex[7];
    char bg_hex[7];
    uint32_t parsed_pixel_rgba;
    uint32_t parsed_bg_rgba;
} custom_colors_dialog_data_t;

static INT_PTR CALLBACK custom_colors_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG: {
        custom_colors_dialog_data_t *data = (custom_colors_dialog_data_t *)lparam;
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)data);
        SetDlgItemTextA(hdlg, IDC_PIXEL_HEX, data->pixel_hex);
        SetDlgItemTextA(hdlg, IDC_BG_HEX, data->bg_hex);
        SendDlgItemMessageA(hdlg, IDC_PIXEL_HEX, EM_SETLIMITTEXT, 6, 0);
        SendDlgItemMessageA(hdlg, IDC_BG_HEX, EM_SETLIMITTEXT, 6, 0);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_PIXEL_CHOOSE:
            choose_color_into_hex_field(hdlg, IDC_PIXEL_HEX);
            return TRUE;
        case IDC_BG_CHOOSE:
            choose_color_into_hex_field(hdlg, IDC_BG_HEX);
            return TRUE;
        case IDOK: {
            custom_colors_dialog_data_t *data = (custom_colors_dialog_data_t *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
            char pixel_text[7], bg_text[7];
            uint8_t px_r, px_g, px_b, bg_r, bg_g, bg_b;
            GetDlgItemTextA(hdlg, IDC_PIXEL_HEX, pixel_text, sizeof(pixel_text));
            GetDlgItemTextA(hdlg, IDC_BG_HEX, bg_text, sizeof(bg_text));
            if (!parse_hex_rgb(pixel_text, &px_r, &px_g, &px_b) || !parse_hex_rgb(bg_text, &bg_r, &bg_g, &bg_b)) {
                MessageBoxA(hdlg, "Both colors need exactly 6 hex digits (0-9, A-F), e.g. \"1A2B3C\".",
                    "pokketstation", MB_ICONERROR);
                return TRUE;
            }
            data->parsed_pixel_rgba = RGBA_PACK(px_r, px_g, px_b);
            data->parsed_bg_rgba = RGBA_PACK(bg_r, bg_g, bg_b);
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void prompt_custom_colors(menu_context_t *ctx) {
    custom_colors_dialog_data_t data;
    format_rgba_hex(*ctx->pixel_rgba, data.pixel_hex, sizeof(data.pixel_hex));
    format_rgba_hex(*ctx->bg_rgba, data.bg_hex, sizeof(data.bg_hex));
    if (DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_CUSTOM_COLORS), ctx->hwnd,
            custom_colors_dialog_proc, (LPARAM)&data) == IDOK) {
        apply_display_colors(ctx, data.parsed_pixel_rgba, data.parsed_bg_rgba);
    }
}

/* lParam payload for shadow_color_dialog_proc.
   Same pattern as custom_colors_dialog_data_t, but with one color instead of two. */
typedef struct {
    char shadow_hex[7];
    uint32_t parsed_shadow_rgba;
} shadow_color_dialog_data_t;

static INT_PTR CALLBACK shadow_color_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG: {
        shadow_color_dialog_data_t *data = (shadow_color_dialog_data_t *)lparam;
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)data);
        SetDlgItemTextA(hdlg, IDC_SHADOW_HEX, data->shadow_hex);
        SendDlgItemMessageA(hdlg, IDC_SHADOW_HEX, EM_SETLIMITTEXT, 6, 0);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_SHADOW_CHOOSE:
            choose_color_into_hex_field(hdlg, IDC_SHADOW_HEX);
            return TRUE;
        case IDC_SHADOW_RESET: {
            /* This resets only the field's displayed text, not the live setting.
               OK still must be clicked to apply and persist it, the same as any other
               edit in this dialog.
               Reset-then-Cancel is a no-op. */
            char default_hex[7];
            format_rgba_hex(DISPLAY_SHADOW_COLOR, default_hex, sizeof(default_hex));
            SetDlgItemTextA(hdlg, IDC_SHADOW_HEX, default_hex);
            return TRUE;
        }
        case IDOK: {
            shadow_color_dialog_data_t *data = (shadow_color_dialog_data_t *)GetWindowLongPtrA(hdlg, GWLP_USERDATA);
            char text[7];
            uint8_t r, g, b;
            GetDlgItemTextA(hdlg, IDC_SHADOW_HEX, text, sizeof(text));
            if (!parse_hex_rgb(text, &r, &g, &b)) {
                MessageBoxA(hdlg, "Expected exactly 6 hex digits (0-9, A-F), e.g. \"1A2B3C\".", "pokketstation",
                    MB_ICONERROR);
                return TRUE;
            }
            data->parsed_shadow_rgba = RGBA_PACK(r, g, b);
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void prompt_shadow_color(menu_context_t *ctx) {
    shadow_color_dialog_data_t data;
    format_rgba_hex(*ctx->shadow_rgba, data.shadow_hex, sizeof(data.shadow_hex));
    if (DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_SHADOW_COLOR), ctx->hwnd,
            shadow_color_dialog_proc, (LPARAM)&data) == IDOK) {
        *ctx->shadow_rgba = data.parsed_shadow_rgba;
        format_rgba_hex(data.parsed_shadow_rgba, ctx->settings->shadow_color, sizeof(ctx->settings->shadow_color));
        save_settings(ctx->settings, ctx->settings_path);
    }
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

/* IR Link > Host Session/Connect/Disconnect. A fixed well-known pipe name (IR_LINK_DEFAULT_PIPE_NAME) is used
   for v1 instead of a dialog prompting for one - simplest thing that works for two instances on one machine.
   See ir_link.h for the actual transport. */
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
    case ID_FILE_QUICK_SAVE:
        quick_save_state(ctx);
        break;
    case ID_FILE_QUICK_LOAD:
        quick_load_state(ctx);
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
    case ID_COLORS_CUSTOM:
        prompt_custom_colors(ctx);
        break;
    case ID_VIEW_SHADOWS_ENABLE:
        set_sprite_shadows(ctx, 1);
        break;
    case ID_VIEW_SHADOWS_DISABLE:
        set_sprite_shadows(ctx, 0);
        break;
    case ID_VIEW_SHADOW_COLOR:
        prompt_shadow_color(ctx);
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
       This draws shadow_rgba (DISPLAY_SHADOW_COLOR by default, user-configurable
       via View > Sprite Shadows > Shadow Color...) one row below each lit pixel.
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

    /* Colors > Light/Dark/Classic/Custom Colors... all funnel through here on
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
       The trailing 4 entries (Create Debug Log, Reset, Quick Save State,
       Quick Load State) are not real PocketStation buttons. Their bit is 0,
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
        {resolve_key_binding(settings.key_quick_save, SDL_SCANCODE_F5), 0, "Quick Save State"},
        {resolve_key_binding(settings.key_quick_load, SDL_SCANCODE_F9), 0, "Quick Load State"},
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
                /* Remappable via Tools > Remap Controls..., default F5. See button_scancodes above. */
                quick_save_state(&menu_ctx);
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == button_scancodes[8].scancode) {
                /* Remappable via Tools > Remap Controls..., default F9. See button_scancodes above. */
                quick_load_state(&menu_ctx);
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

        /* Drains this frame's freshly-produced TX edges onto the pipe, and pushes any RX edges that arrived
           from the other instance since last frame - in time for next frame's psemu_run to process them via
           ir_tick. Same one-frame (~31ms) granularity every other input path (buttons) already accepts. See
           ir_link.h. ir_link's own status can change here with no menu action involved (a peer connects, or
           the link errors out), so the title is refreshed whenever the status text actually changes. */
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
       show_console is never written by this app at all - see the same comment.
       Nothing is left to flush on exit. */
    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
