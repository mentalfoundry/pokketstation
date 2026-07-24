#include <SDL.h>
#include <SDL_syswm.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#ifdef _MSC_VER
/* SysLink (Help > About's clickable repo link) only exists in ComCtl32
   v6+; without this, the OS loader binds the old v5.82 system DLL (no
   manifest = no side-by-side version selection) and CreateDialog would
   silently fail to create that one control. */
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

#define SCALE 8

/* Shown in Help > About - bump this by hand to match whatever's actually
   being released (e.g. the latest git tag) each time one goes out. Not
   wired up to git/CMake automatically on purpose - a plain string here is
   the one spot to touch, rather than something that silently drifts if
   the build environment doesn't have git available (e.g. building from a
   source zip instead of a clone). */
#define POKKETSTATION_VERSION "v1.4.0"

/* Directory the running executable lives in, derived from argv[0] rather
   than an OS-specific "current module path" API - argv[0] is already the
   full path when Explorer double-click-launches an exe, which is the one
   case this is actually needed for (CLI invocations pass explicit paths
   and never hit this). */
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

/* Writes a timestamped diagnostic report (frontend context - reason,
   frame number - followed by psemu_write_crash_report's full CPU/trace
   dump) to disk and points the user at it on stderr. Called both
   automatically on a detected CPU fault and on-demand via a hotkey
   (F12), since not everything worth reporting during manual testing
   trips psemu_cpu_faulted() - "the game looks wrong" or "no sound" are
   just as real as a hard fault, and this session's actual Chocobo World
   crash investigation (see docs/hardware-notes.md) needed exactly this
   kind of state dump, built by hand with one-off tracing, to get
   anywhere. */
static void write_diagnostic_report(
    const psemu_t *ps, const char *reason, unsigned long frame, const char *bios_path, const char *app_path) {
    char path[64];
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    FILE *f;

    strftime(path, sizeof(path), "psemu_report_%Y%m%d_%H%M%S.log", tmv);
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

/* Small preferences file the desktop app owns outright - not a format any
   external tool reads or writes, unlike the hardware ID's own encoding
   (see psemu_parse_hardware_id's comment in psemu.h). Remembers:
     - The last successfully-loaded BIOS path, so a double-click launch
       (no CLI args) doesn't need bios.bin sitting next to the .exe once a
       real BIOS has been picked at least once via File > Open or a CLI
       argument.
     - The PocketStation hardware serial number (F_SN) - the core's own
       default (PSEMU_DEFAULT_HARDWARE_ID) already gives every fresh
       Chocobo World save the best rank, but a homebrew ID-editing tool
       can change it in-session, and there's no other persistent store for
       that (it lives in a flash "header" region outside the ordinary
       128KB card image, so it isn't part of any .mcr/.mcs file). Stored
       as exactly 8 plain hex digits, the same form a real "ID rewriter"
       homebrew itself displays and edits - confirmed via real hardware
       there's no "first digit must be a letter" restriction, e.g. a real
       unit accepts and persists "EEEEEEEE". Empty means "use the
       default".
     - The --console/--no-console choice.
   Saved immediately at the point each of these actually changes (BIOS
   loaded via a CLI arg or File > Open, hardware ID edited via Tools >
   Edit Hardware ID, or an explicit --console/--no-console flag) rather
   than batched up for a single write at exit - a force-kill or crash
   mid-session would otherwise silently lose whatever changed since
   launch. */
#define SETTINGS_CONFIG_NAME "settings.cfg"

typedef struct {
    char bios_path[1024];
    char hardware_id[PSEMU_HARDWARE_ID_STRING_SIZE];
    /* "RRGGBB" hex, or empty for the Classic preset's default (a fresh
       settings.cfg's starting point - see load_settings and the
       DISPLAY_*_CLASSIC constants below). */
    char pixel_color[7];
    char bg_color[7];
    /* "RRGGBB" hex, or empty for DISPLAY_SHADOW_COLOR's default. */
    char shadow_color[7];
    int show_console;
    int show_shadows;
    /* SDL_GetScancodeName()-formatted key names (e.g. "Up", "Z", "Left
       Ctrl"), or empty for that button's hardcoded default - see
       resolve_key_binding. */
    char key_up[32];
    char key_down[32];
    char key_left[32];
    char key_right[32];
    char key_fire[32];
} app_settings_t;

/* Packs 8-bit R/G/B into the same 0xRRGGBBAA layout render_framebuffer
   writes into the pixel buffer (see its own comment on
   SDL_PIXELFORMAT_RGBA8888's byte order) - alpha is always opaque. */
#define RGBA_PACK(r, g, b) \
    ((((uint32_t)(r)) << 24) | (((uint32_t)(g)) << 16) | (((uint32_t)(b)) << 8) | 0xFFu)

#define DISPLAY_PIXEL_LIGHT RGBA_PACK(0x00, 0x00, 0x00)
#define DISPLAY_BG_LIGHT RGBA_PACK(0xFF, 0xFF, 0xFF)
#define DISPLAY_PIXEL_DARK RGBA_PACK(0xFF, 0xFF, 0xFF)
#define DISPLAY_BG_DARK RGBA_PACK(0x00, 0x00, 0x00)
/* Approximates an unlit reflective/transflective LCD like a watch or
   Tamagotchi's - a dark, slightly warm ink color rather than pure black,
   against a muted sage-gray (not white) background. The default color
   scheme for a freshly-initialized settings.cfg (see load_settings). */
#define DISPLAY_PIXEL_CLASSIC RGBA_PACK(0x11, 0x1A, 0x15)
#define DISPLAY_BG_CLASSIC RGBA_PACK(0xBC, 0xC7, 0xB9)

/* Faint "ghosting" real late-90s STN/passive-matrix LCDs (watches,
   Tamagotchis, and the PocketStation itself) show trailing a lit pixel,
   from slow crystal response rather than a real drop shadow - always this
   fixed color regardless of the active color scheme, per View > Sprite
   Shadows. */
#define DISPLAY_SHADOW_COLOR RGBA_PACK(0x8E, 0x9B, 0x8E)

/* Inverse of RGBA_PACK's top 3 bytes, formatted the way settings.cfg
   stores colors ("RRGGBB", see save_settings). */
static void format_rgba_hex(uint32_t rgba, char *out, size_t out_size) {
    snprintf(out, out_size, "%02X%02X%02X", (unsigned)(rgba >> 24) & 0xFFu, (unsigned)(rgba >> 16) & 0xFFu,
        (unsigned)(rgba >> 8) & 0xFFu);
}

/* Returns nonzero if `path` already existed (and was read from), 0 if it
   didn't - callers use this to tell "first run, nothing to read yet" from
   "an existing file just happened to not set every field". */
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
            } else if (strncmp(line, "show_console=", 13) == 0) {
                settings->show_console = atoi(line + 13) != 0;
            } else if (strncmp(line, "show_shadows=", 13) == 0) {
                settings->show_shadows = atoi(line + 13) != 0;
            }
        }
        fclose(f);
    }
    /* No settings.cfg yet, or an existing one written before one of these
       fields existed - fill in the real default for whichever is still
       blank (PSEMU_DEFAULT_HARDWARE_ID, the Classic color scheme, the
       default shadow color, and the original hardcoded key bindings)
       rather than leaving it blank, so the file always shows what's
       actually in effect instead of an implicit fallback nothing on disk
       hints at. */
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

/* Parses exactly 6 hex digits ("RRGGBB") and nothing else - same
   deliberately-strict, no-alternate-format spirit as
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

/* One entry of the live key -> PocketStation-button mapping the main loop
   polls every frame (see button_scancodes in main) - display_name is only
   used for remap prompts/labels, never persisted itself (the scancode's
   own SDL_GetScancodeName is what's written to settings.cfg). */
typedef struct {
    SDL_Scancode scancode;
    uint32_t bit;
    const char *display_name;
} button_binding_t;

/* `saved_name` is a settings.cfg key_* field - parsed via
   SDL_GetScancodeFromName, falling back to `fallback` if it's empty or
   doesn't name a real key (e.g. hand-edited to garbage). */
static SDL_Scancode resolve_key_binding(const char *saved_name, SDL_Scancode fallback) {
    if (saved_name[0] != '\0') {
        SDL_Scancode sc = SDL_GetScancodeFromName(saved_name);
        if (sc != SDL_SCANCODE_UNKNOWN) {
            return sc;
        }
    }
    return fallback;
}

/* Inverse of resolve_key_binding - bindings must be exactly 5 entries in
   fixed Up/Down/Left/Right/Fire order (see button_scancodes in main). */
static void save_key_bindings(app_settings_t *settings, const button_binding_t bindings[5]) {
    snprintf(settings->key_up, sizeof(settings->key_up), "%s", SDL_GetScancodeName(bindings[0].scancode));
    snprintf(settings->key_down, sizeof(settings->key_down), "%s", SDL_GetScancodeName(bindings[1].scancode));
    snprintf(settings->key_left, sizeof(settings->key_left), "%s", SDL_GetScancodeName(bindings[2].scancode));
    snprintf(settings->key_right, sizeof(settings->key_right), "%s", SDL_GetScancodeName(bindings[3].scancode));
    snprintf(settings->key_fire, sizeof(settings->key_fire), "%s", SDL_GetScancodeName(bindings[4].scancode));
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

/* State the menu bar's WM_COMMAND handlers need to reach, bundled since
   SDL_SetWindowsMessageHook only takes a single void *userdata. */
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
    button_binding_t *button_scancodes; /* fixed 5-element Up/Down/Left/Right/Fire array, see main */
    app_settings_t *settings;
    const char *settings_path;
} menu_context_t;

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
    *ctx->cpu_faulted_reported = 0;
}

/* lParam payload for hwid_dialog_proc, passed in via DialogBoxParamA and
   retrieved with GetWindowLongPtrA(GWLP_USERDATA) - parsed_id is only
   filled in (and IDOK only allowed to close the dialog) once the edit
   control's text has actually passed psemu_parse_hardware_id. */
typedef struct {
    char text[PSEMU_HARDWARE_ID_STRING_SIZE];
    uint32_t parsed_id;
} hwid_dialog_data_t;

static INT_PTR CALLBACK hwid_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, (LONG_PTR)lparam);
        SetDlgItemTextA(hdlg, IDC_HWID_EDIT, ((hwid_dialog_data_t *)lparam)->text);
        /* PSEMU_HARDWARE_ID_STRING_SIZE includes the '\0' - the canonical
           form is always exactly 8 hex digits, so cap typed input there
           instead of letting psemu_parse_hardware_id reject it later. */
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

/* Resizes the window so its *client* area (where the framebuffer actually
   renders) becomes exactly PSEMU_LCD_{WIDTH,HEIGHT} * SCALE * multiplier,
   regardless of however much chrome (menu bar, title bar, borders) the
   window currently has - same before/after-GetClientRect technique used
   to compensate for the menu bar's height at startup. */
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

/* Fills the IDC_PIXEL_HEX/IDC_BG_HEX edit control at `edit_id` with
   whatever ChooseColorA returns, seeded from that field's current text
   (falling back to black if it isn't valid hex yet) - lets a "Choose..."
   click and hand-typing the hex code freely mix, either one just
   overwrites the same field. */
static void choose_color_into_hex_field(HWND hdlg, int edit_id) {
    /* CHOOSECOLOR requires a caller-owned 16-entry custom-color scratch
       array; static so the user's custom-palette additions survive
       between picks, both within one dialog session and across separate
       menu invocations, rather than resetting every time. */
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

/* lParam payload for custom_colors_dialog_proc - same pattern as
   hwid_dialog_data_t: parsed_*_rgba is only filled in (and IDOK only
   allowed to close the dialog) once both hex fields have actually passed
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

/* lParam payload for shadow_color_dialog_proc - same pattern as
   custom_colors_dialog_data_t, just one color instead of two. */
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
            /* Only resets the field's displayed text, not the live
               setting - OK still has to be clicked to actually apply (and
               persist) it, same as any other edit in this dialog, so
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

/* No buttons, nothing to wire up beyond letting the default dialog
   handling run - IDD_CAPTURE_PROMPT is purely a static text display. */
static INT_PTR CALLBACK capture_prompt_dialog_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam) {
    (void)hdlg;
    (void)wparam;
    (void)lparam;
    return msg == WM_INITDIALOG ? TRUE : FALSE;
}

/* Blocks - draining SDL's own event queue directly, the same way
   DialogBoxParamA blocks the caller with its own message loop - until the
   user presses a key (returned) or Esc/closes the window (returns
   SDL_SCANCODE_UNKNOWN, treated as "cancelled"). This has to go through
   SDL_PollEvent rather than a native dialog/message loop: capturing a raw
   Win32 WM_KEYDOWN and converting it to an SDL_Scancode by hand would mean
   reimplementing SDL's own (nontrivial, per-platform) scancode table,
   which is exactly what button_scancodes[].scancode and
   SDL_GetKeyboardState both already rely on SDL to get right - safer to
   just ask SDL directly, the same way normal gameplay input already does.
   A real SDL_QUIT during this wait can't be cleanly bubled back out
   through the nested call stack that led here (WM_COMMAND handler ->
   this), so it's treated as a cancel too; the user's next close attempt
   after that proceeds normally since this only ever blocks for a single
   keypress. */
static SDL_Scancode capture_next_key(HWND hwnd, const char *button_name) {
    char message[160];
    HWND prompt;
    RECT owner_rect, prompt_rect;
    int have_result = 0;
    SDL_Scancode result = SDL_SCANCODE_UNKNOWN;

    /* CreateDialogParamA (modeless), not DialogBoxParamA - a modal dialog
       would block this function from ever reaching the SDL_PollEvent loop
       below, the same problem MessageBoxA had. Shown via
       SW_SHOWNOACTIVATE so it never takes activation/keyboard focus away
       from `hwnd` in the first place - that's what was previously causing
       the pressed key to get eaten by Windows' own menu-mnemonic handling
       (heard as the system beep) instead of ever reaching SDL as a real
       SDL_KEYDOWN. */
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
        for (i = 0; i < 5; i++) {
            SetDlgItemTextA(hdlg, IDC_REMAP_LABEL_BASE + i, SDL_GetScancodeName(bindings[i].scancode));
        }
        return TRUE;
    }
    case WM_COMMAND: {
        int cmd = LOWORD(wparam);
        if (cmd == IDOK || cmd == IDCANCEL) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        if (cmd >= IDC_REMAP_CHANGE_BASE && cmd < IDC_REMAP_CHANGE_BASE + 5) {
            /* 100+ is well clear of any real IDOK/IDCANCEL/control ID -
               prompt_remap_controls uses this to tell "row N's Change...
               was clicked" apart from a plain close. */
            EndDialog(hdlg, 100 + (cmd - IDC_REMAP_CHANGE_BASE));
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

/* Shows the current 5 bindings with a per-row "Change..." button; clicking
   one closes this dialog (see remap_dialog_proc) so the actual keypress
   can be captured via capture_next_key below, outside of any native
   dialog's own keyboard-navigation message loop, then reopens the dialog
   to show the result and allow changing another row - loops until the
   user clicks Close. */
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

/* Installed via SDL_SetWindowsMessageHook - fires synchronously from
   within SDL_PollEvent's own message pump (same thread, no locking
   needed), so it's safe to mutate *ctx and call psemu_* directly here. */
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
            /* SDL_PIXELFORMAT_RGBA8888 packs a 32-bit value as
               (R<<24)|(G<<16)|(B<<8)|A regardless of host endianness -
               0xFF000000 is R=0xFF,G=0,B=0,A=0 (pure red, alpha-
               transparent), not opaque black - pixel_rgba/bg_rgba (see
               RGBA_PACK) already account for this, so callers must not
               pass plain 0xRRGGBB values here. */
            pixels[row * PSEMU_LCD_WIDTH + col] = lcd_bit_on(fb, row, col) ? pixel_rgba : bg_rgba;
        }
    }
    if (!show_shadows) {
        return;
    }
    /* Approximates the faint "ghosting" a real late-90s STN/passive-matrix
       LCD shows trailing a lit pixel (slow crystal response, not a real
       drop shadow) - shadow_rgba (DISPLAY_SHADOW_COLOR by default, but
       user-configurable via View > Sprite Shadows > Shadow Color...) one
       row below each lit pixel, drawn as a second pass so it never
       overwrites an actually-lit pixel (checked against `fb` directly, not
       the just-written output, since two adjacent source pixels being lit
       must never dim each other). */
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
    /* Mutable (not just const char * into argv) so File > Load BIOS/Open
       App can overwrite the current path in place after a successful reload -
       used for the F12/crash diagnostic report's "bios:"/"app:" lines. */
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
        /* Registers SysLink (Help > About's clickable repo link) - a no-op
           for every other dialog in this file, so doing this once up
           front is simplest rather than threading it through each
           DialogBoxParamA call site individually. */
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LINK_CLASS;
        InitCommonControlsEx(&icc);
    }

    /* Computed regardless of how paths/flags were given below - the
       settings file always lives next to the executable, not next to
       whatever content path the user passed on the command line. */
    get_exe_dir(argv[0], exe_dir, sizeof(exe_dir));
    join_path(settings_config_path, sizeof(settings_config_path), exe_dir, SETTINGS_CONFIG_NAME);
    if (!load_settings(&settings, settings_config_path)) {
        /* First run - settings.cfg didn't exist yet, so write it out right
           away with the defaults load_settings just filled in (e.g.
           hardware_id) rather than waiting for some other change (BIOS
           load, hardware ID edit, etc.) to trigger the first save. An
           existing file is left exactly as read here even if a field
           happened to be blank in it - only ever rewritten by an actual
           update, same as everything else in settings.cfg. */
        save_settings(&settings, settings_config_path);
    }

    /* --console/--no-console are the only flags; everything else is a
       positional arg. Extra positional args beyond 2 are silently ignored,
       matching this parsing's long-standing behavior of only ever reading
       the first two. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0) {
            saw_console_flag = 1;
        } else if (strcmp(argv[i], "--no-console") == 0) {
            saw_no_console_flag = 1;
        } else if (npositional < 2) {
            positional[npositional++] = argv[i];
        }
    }
    /* An explicit flag always wins for this run; absent either, fall back
       to the persisted preference from settings.cfg (0 if there's no
       settings file yet). Only persist when a flag actually changed it -
       otherwise this run's resolved value already matches what's on disk
       and there's nothing to update. */
    show_console = saw_console_flag ? 1 : (saw_no_console_flag ? 0 : settings.show_console);
    if (saw_console_flag || saw_no_console_flag) {
        settings.show_console = show_console;
        save_settings(&settings, settings_config_path);
    }

    if (show_console) {
        /* Built as a GUI-subsystem executable (see CMakeLists.txt's
           add_executable(... WIN32 ...)) so no console is attached by
           default and every fprintf(stderr, ...) below would otherwise go
           nowhere visible - --console opts back into one for anyone who
           wants to see them (e.g. while testing from a terminal). */
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    if (npositional >= 2) {
        snprintf(bios_path, sizeof(bios_path), "%s", positional[0]);
        snprintf(app_path, sizeof(app_path), "%s", positional[1]);
    } else if (npositional == 0) {
        /* No positional arguments at all means Explorer double-click-
           launched the .exe rather than a terminal invocation - fall back
           first to the BIOS path remembered in settings.cfg from a
           previous run's File > Open (or CLI argument), then to a BIOS
           dump sitting next to the .exe, since there's no command line to
           pass a path on. The memory-card/app path isn't remembered in
           settings.cfg (only the BIOS is, per its comment above), so it
           always falls back to the same next-to-the-.exe convention. */
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

    /* Neither a missing nor an invalid BIOS/app-or-card file is fatal
       anymore - the menu bar's File > Load BIOS.../Open App/Card... lets
       the user browse to one after the window comes up, so launch either
       way and just leave psemu without one loaded (psemu_run no-ops until
       a BIOS is actually loaded - see psemu_run's !ps->has_bios check). */
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

    fprintf(stderr, "psemu: press F12 at any time to write a diagnostic report to a psemu_report_*.log file\n");

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

    /* Colors > Light/Dark/Classic/Custom Colors... all funnel through here
       on next launch (see save_settings above) - load_settings always
       fills in a real value (Classic, on a fresh settings.cfg) for both
       fields, so this Light fallback only matters if settings.cfg's
       content somehow doesn't parse (e.g. hand-edited to something
       invalid). */
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

    /* Persist a BIOS that just loaded successfully right away (whether it
       came from a CLI argument or the settings-remembered/next-to-the-.exe
       default) rather than waiting for exit - see the settings-file
       comment above for why. A failed load leaves settings.bios_path
       untouched. */
    if (bios) {
        snprintf(settings.bios_path, sizeof(settings.bios_path), "%s", bios_path);
        save_settings(&settings, settings_config_path);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Resizable - the 32x32 framebuffer is stretched to fill the entire
       render target on every frame regardless of its size (render_copy's
       NULL dstrect below), so free-form resizing just works without any
       extra handling; View > Native/Double Size are just a shortcut back
       to a known-good size, not the only sizes supported. */
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

    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info)) {
        fprintf(stderr, "SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return 1;
    }
    HWND hwnd = wm_info.info.win.window;

    /* SDL registers its own window class with no icon, so the exe's
       resource icon (see resource.rc) doesn't show up on the title bar or
       Alt-Tab on its own even though it's already the taskbar/Explorer
       icon - load it explicitly at both sizes Windows actually asks for
       and set it on the window directly. */
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
    /* SetMenu shrinks the client area to make room for the menu bar unless
       the window grows to compensate - grow by exactly however much it
       just shrank so the emulator keeps rendering at its native
       SCALE-scaled size instead of getting clipped or letterboxed. */
    RECT client_after;
    GetClientRect(hwnd, &client_after);
    int shrink =
        (client_before.bottom - client_before.top) - (client_after.bottom - client_after.top);
    if (shrink > 0) {
        RECT window_rect;
        GetWindowRect(hwnd, &window_rect);
        SetWindowPos(hwnd, NULL, 0, 0, window_rect.right - window_rect.left,
            (window_rect.bottom - window_rect.top) + shrink, SWP_NOMOVE | SWP_NOZORDER);
    }

    /* Live key -> PocketStation-button mapping the main loop polls every
       frame - fixed Up/Down/Left/Right/Fire order (matches
       IDC_REMAP_LABEL_BASE/IDC_REMAP_CHANGE_BASE and save_key_bindings).
       Not const/static since Tools > Remap Controls... mutates entries in
       place through menu_ctx.button_scancodes, which points at this same
       array. */
    button_binding_t button_scancodes[5] = {
        {resolve_key_binding(settings.key_up, SDL_SCANCODE_UP), PSEMU_BUTTON_UP, "Up"},
        {resolve_key_binding(settings.key_down, SDL_SCANCODE_DOWN), PSEMU_BUTTON_DOWN, "Down"},
        {resolve_key_binding(settings.key_left, SDL_SCANCODE_LEFT), PSEMU_BUTTON_LEFT, "Left"},
        {resolve_key_binding(settings.key_right, SDL_SCANCODE_RIGHT), PSEMU_BUTTON_RIGHT, "Right"},
        {resolve_key_binding(settings.key_fire, SDL_SCANCODE_Z), PSEMU_BUTTON_FIRE, "Fire/Action"},
    };

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
    SDL_SetWindowsMessageHook(handle_windows_message, &menu_ctx);

    uint32_t pixels[PSEMU_LCD_WIDTH * PSEMU_LCD_HEIGHT];
    int16_t audio_buf[1024];
    unsigned long frame = 0;

    /* Minimum number of frames a button reads as pressed once detected,
       stretching a quick real tap to match the duration already
       confirmed (via scripted headless testing) to reliably register
       with the real BIOS. At 32Hz, a real ~40ms tap is only ~1.3 frames -
       if it lands awkwardly between two per-frame SDL_GetKeyboardState
       polls, the emulator could see it for a small fraction of a frame,
       too short for the BIOS's own input handling to count it as a
       completed press. */
#define BUTTON_LATCH_FRAMES 5
    int latch_frames_remaining[sizeof(button_scancodes) / sizeof(button_scancodes[0])] = {0};

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_F12) {
                /* On-demand snapshot for manual testing - press F12 the
                   moment something looks wrong (frozen screen, missing
                   sound, garbled graphics), whether or not the CPU has
                   actually faulted. */
                write_diagnostic_report(ps, "manual (F12)", frame, bios_path, app_path);
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

        /* 33000 cycles at a 32Hz refresh (~1.056MHz effective) - reverted
           from an earlier attempt to match rtc.h's RTC_TICK_CYCLES
           (~4MHz), which turned out to be an unvalidated guess matching
           one uncalibrated constant to another: real-hardware testing
           showed that "fix" made on-screen blinking visibly too fast.
           Real hardware genuinely runs at a variable clock (up to
           ~7.995MHz, see the CPU_FREQ table in core/src/clk.c, and
           docs/hardware-notes.md, "CLK_MODE") - psemu_run does scale its
           overall throughput by the app's currently-programmed CLK_MODE,
           but not true per-instruction cycle costs (still approximate,
           see "Known open questions" in docs/hardware-notes.md), so any
           single fixed per-frame budget is itself still an approximation
           on top of that - 33000/frame is kept here specifically because
           it's the value empirically confirmed to look right, not because
           it's independently derived. See dac.h's PSEMU_ASSUMED_CPU_HZ
           for the matching audio-rate conversion (33000 * 32) - keep both
           in sync if this ever changes. */
        /* If the CPU has run into an opcode this emulator doesn't
           recognize, register/memory state is no longer meaningful - a
           real, confirmed bug found this way (see docs/hardware-notes.md,
           "Known open questions" - "Chocobo World event-screen crash")
           reaches this after ~1.3 billion instructions of otherwise-correct
           real gameplay, so
           this can't be assumed harmless just because it hasn't happened
           yet. Stop stepping the CPU once this trips (freezing on the
           last good frame) instead of silently continuing to feed it
           garbage forever, which previously looked to a player like an
           unexplained hang/crash with zero diagnostic information. */
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

    if (audio_dev != 0) {
        SDL_CloseAudioDevice(audio_dev);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    /* No settings save here - bios_path, hardware_id, and show_console are
       each already persisted immediately at the point they last changed
       (see the settings-file comment above), so there's nothing left to
       flush on exit. */
    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
