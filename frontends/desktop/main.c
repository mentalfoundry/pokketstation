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
            pixels[row * PSEMU_LCD_WIDTH + col] = on ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios.bin> <app.pss>\n", argv[0]);
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
    if (psemu_load_app(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr, "invalid app image\n");
        return 1;
    }
    psemu_reset(ps);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("PokketStation", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        PSEMU_LCD_WIDTH * SCALE, PSEMU_LCD_HEIGHT * SCALE, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, PSEMU_LCD_WIDTH, PSEMU_LCD_HEIGHT);

    uint32_t pixels[PSEMU_LCD_WIDTH * PSEMU_LCD_HEIGHT];
    int running = 1;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = 0;
            }
        }

        const uint8_t *keys = SDL_GetKeyboardState(NULL);
        uint32_t buttons = 0;
        if (keys[SDL_SCANCODE_UP]) buttons |= PSEMU_BUTTON_UP;
        if (keys[SDL_SCANCODE_DOWN]) buttons |= PSEMU_BUTTON_DOWN;
        if (keys[SDL_SCANCODE_LEFT]) buttons |= PSEMU_BUTTON_LEFT;
        if (keys[SDL_SCANCODE_RIGHT]) buttons |= PSEMU_BUTTON_RIGHT;
        if (keys[SDL_SCANCODE_Z]) buttons |= PSEMU_BUTTON_FIRE;
        psemu_set_buttons(ps, buttons);

        psemu_run(ps, 33000);

        render_framebuffer(ps, pixels);
        SDL_UpdateTexture(texture, NULL, pixels, PSEMU_LCD_WIDTH * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        SDL_Delay(31); /* ~32Hz, matching the real LCD refresh */
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
