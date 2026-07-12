#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "psemu/psemu.h"

#define SCALE 8

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
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios.bin> <app.pss | memory-card.mcr>\n", argv[0]);
        fprintf(
            stderr,
            "  a %d-byte file is loaded as a raw memory card image (with its own directory) -\n"
            "  navigate and launch apps from the real BIOS menu with the keyboard, same as real\n"
            "  hardware. Anything else is loaded as a single Title Sector app (MCX0/MCX1).\n",
            PSEMU_FLASH_SIZE);
        return 1;
    }

    size_t bios_size = 0, app_size = 0;
    uint8_t *bios = read_file(argv[1], &bios_size);
    uint8_t *app = read_file(argv[2], &app_size);
    if (!bios || !app) {
        fprintf(stderr, "failed to read input files\n");
        return 1;
    }

    psemu_t *ps = psemu_create();
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        fprintf(stderr, "invalid BIOS image (expected %d bytes)\n", PSEMU_BIOS_SIZE);
        return 1;
    }
    if (app_size == PSEMU_FLASH_SIZE) {
        if (psemu_load_flash_image(ps, app, app_size) != PSEMU_OK) {
            fprintf(stderr, "failed to load memory card image\n");
            return 1;
        }
    } else if (psemu_load_app(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr, "invalid app image\n");
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
           ~7.995MHz, see the CPU_FREQ table referenced in
           docs/hardware-notes.md), and this emulator doesn't model
           per-instruction cycle counts or CLK_MODE at all, so any single
           fixed rate is an approximation - 33000/frame is kept here
           specifically because it's the value empirically confirmed to
           look right, not because it's independently derived. See
           dac.h's PSEMU_ASSUMED_CPU_HZ for the matching audio-rate
           conversion (33000 * 32) - keep both in sync if this ever
           changes. */
        psemu_run(ps, 33000);

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
    }

    if (audio_dev != 0) {
        SDL_CloseAudioDevice(audio_dev);
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
