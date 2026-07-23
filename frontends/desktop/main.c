#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "psemu/psemu.h"

#define SCALE 8

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

/* Hardware ID (F_SN, see psemu.h) persistence: the core's own default
   (0x410000D3, "410000D3" in hex form) already gives every fresh Chocobo
   World save the best rank, but a homebrew ID-editing tool can change it
   in-session - without saving that back somewhere, every relaunch would
   silently lose the edit and revert to the default. There's no other persistent store for this (it lives in a
   flash "header" region outside the ordinary 128KB card image, so it
   isn't part of any .mcr/.mcs file), so the desktop frontend keeps its
   own small config file next to the executable. Stored as exactly 8
   plain hex digits - the same form a real "ID rewriter" homebrew itself
   displays and edits (see psemu_parse_hardware_id's comment in psemu.h -
   confirmed via real hardware there's no "first digit must be a letter"
   restriction, e.g. a real unit accepts and persists "EEEEEEEE") - and
   nothing else: what's in the file is exactly the raw value, with no
   alternate/translated format silently also accepted. A real unit's
   printed "sticker" ID (one letter + 8 decimal digits, e.g. "A02374684")
   is a different, less-general encoding of the same underlying value;
   converting one to the other, if ever wanted, belongs as a small
   standalone desktop-app feature, not hidden inside this file's format. */
#define HARDWARE_ID_CONFIG_NAME "hardware_id.cfg"

static void load_hardware_id_config(psemu_t *ps, const char *path) {
    FILE *f = fopen(path, "r");
    char line[64] = {0};
    uint32_t id;
    if (!f) {
        return;
    }
    /* psemu_parse_hardware_id expects exactly 8 hex digits and nothing
       else - strip fgets' trailing newline (and a preceding \r, in case
       the file was saved with Windows line endings) before parsing.
       Leaves `line` as an empty string (rejected by
       psemu_parse_hardware_id below) if fgets fails, e.g. an empty
       file. */
    if (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
    }
    if (psemu_parse_hardware_id(line, &id)) {
        psemu_set_hardware_id(ps, id);
    } else {
        fprintf(
            stderr,
            "psemu: couldn't parse hardware ID from %s (expected exactly 8 hex digits, e.g. \"EEEEEEEE\") - "
            "ignoring, using the default\n",
            path);
    }
    fclose(f);
}

static void save_hardware_id_config(const psemu_t *ps, const char *path) {
    char buf[PSEMU_HARDWARE_ID_STRING_SIZE];
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "psemu: failed to persist hardware ID to %s\n", path);
        return;
    }
    psemu_format_hardware_id(psemu_get_hardware_id(ps), buf, sizeof(buf));
    fprintf(f, "%s\n", buf);
    fclose(f);
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

static void render_framebuffer(const psemu_t *ps, uint32_t *pixels) {
    const uint8_t *fb = psemu_get_framebuffer(ps);
    for (int row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
            int bit_index = col % 8;
            int on = (fb[byte_index] >> bit_index) & 1;
            /* SDL_PIXELFORMAT_RGBA8888 packs a 32-bit value as
               (R<<24)|(G<<16)|(B<<8)|A regardless of host endianness -
               0xFF000000 is R=0xFF,G=0,B=0,A=0 (pure red, alpha-
               transparent), not opaque black. SDL_RenderCopy's default
               blend mode ignores the source alpha and blits RGB as-is,
               so "on" pixels rendered solid red instead of black. */
            pixels[row * PSEMU_LCD_WIDTH + col] = on ? 0x000000FFu : 0xFFFFFFFFu;
        }
    }
}

int main(int argc, char **argv) {
    char default_bios_path[1024];
    char default_app_path[1024];
    char exe_dir[900];
    char hwid_config_path[1024];
    const char *bios_path;
    const char *app_path;
    int using_defaults = 0;

    /* Computed regardless of how bios/app paths were given below - the
       hardware-ID config file (see load_hardware_id_config) always lives
       next to the executable, not next to whatever content path the user
       passed on the command line. */
    get_exe_dir(argv[0], exe_dir, sizeof(exe_dir));
    join_path(hwid_config_path, sizeof(hwid_config_path), exe_dir, HARDWARE_ID_CONFIG_NAME);

    if (argc >= 3) {
        bios_path = argv[1];
        app_path = argv[2];
    } else if (argc == 1) {
        /* No arguments at all means Explorer double-click-launched the
           .exe rather than a terminal invocation - fall back to looking
           for a BIOS dump and memory-card image sitting next to it,
           since there's no command line to pass paths on. */
        join_path(default_bios_path, sizeof(default_bios_path), exe_dir, "bios.bin");
        join_path(default_app_path, sizeof(default_app_path), exe_dir, "memcard.mcr");
        bios_path = default_bios_path;
        app_path = default_app_path;
        using_defaults = 1;
    } else {
        fprintf(stderr, "usage: %s <bios.bin> <app.pss | app.mcs | memory-card.mcr>\n", argv[0]);
        fprintf(
            stderr,
            "  a %d-byte file is loaded as a raw memory card image (with its own directory) -\n"
            "  navigate and launch apps from the real BIOS menu with the keyboard, same as real\n"
            "  hardware. Anything else is loaded as a single Title Sector app (MCX0/MCX1), tried\n"
            "  either bare (.pss) or wrapped in a single-save directory frame (.mcs).\n",
            PSEMU_FLASH_SIZE);
        return 1;
    }

    size_t bios_size = 0, app_size = 0;
    uint8_t *bios = read_file(bios_path, &bios_size);
    uint8_t *app = read_file(app_path, &app_size);
    if (!bios || !app) {
        if (using_defaults) {
            fprintf(
                stderr,
                "psemu: couldn't find a BIOS dump and/or memory-card image next to the .exe:\n"
                "  %s%s\n"
                "  %s%s\n"
                "Place a BIOS dump named \"bios.bin\" and a memory-card image named \"memcard.mcr\"\n"
                "next to pokketstation_desktop.exe, or run it from a terminal with explicit paths:\n"
                "  %s <bios.bin> <app-or-card-file>\n",
                default_bios_path, bios ? " (ok)" : " (missing)", default_app_path, app ? " (ok)" : " (missing)",
                argv[0]);
            fprintf(stderr, "press Enter to exit...\n");
            free(bios);
            free(app);
            getchar();
        } else {
            fprintf(stderr, "failed to read input files\n");
            free(bios);
            free(app);
        }
        return 1;
    }

    fprintf(stderr, "psemu: press F12 at any time to write a diagnostic report to a psemu_report_*.log file\n");

    psemu_t *ps = psemu_create();
    load_hardware_id_config(ps, hwid_config_path);
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        fprintf(stderr, "invalid BIOS image (expected %d bytes)\n", PSEMU_BIOS_SIZE);
        return 1;
    }
    if (psemu_load_content(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr, "invalid app or memory-card image\n");
        return 1;
    }
    psemu_reset(ps);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("PokketStation", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        PSEMU_LCD_WIDTH * SCALE, PSEMU_LCD_HEIGHT * SCALE, 0);
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

    uint32_t pixels[PSEMU_LCD_WIDTH * PSEMU_LCD_HEIGHT];
    int16_t audio_buf[1024];
    int running = 1;
    int cpu_faulted_reported = 0;
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
    static const struct {
        SDL_Scancode scancode;
        uint32_t bit;
    } button_scancodes[] = {
        {SDL_SCANCODE_UP, PSEMU_BUTTON_UP},
        {SDL_SCANCODE_DOWN, PSEMU_BUTTON_DOWN},
        {SDL_SCANCODE_LEFT, PSEMU_BUTTON_LEFT},
        {SDL_SCANCODE_RIGHT, PSEMU_BUTTON_RIGHT},
        {SDL_SCANCODE_Z, PSEMU_BUTTON_FIRE},
    };
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

        render_framebuffer(ps, pixels);
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

    /* Persists any in-session hardware-ID edit (e.g. via a homebrew ID
       editor) for next launch - see load_hardware_id_config's comment. */
    save_hardware_id_config(ps, hwid_config_path);

    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
