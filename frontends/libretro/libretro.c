#include <stdio.h>
#include <string.h>

#include "libretro.h"
#include "psemu/psemu.h"

static psemu_t *g_ps = NULL;
static uint32_t g_framebuffer[PSEMU_LCD_WIDTH * PSEMU_LCD_HEIGHT];

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

void retro_init(void) {
    g_ps = psemu_create();
}

void retro_deinit(void) {
    psemu_destroy(g_ps);
    g_ps = NULL;
}

unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "PokketStation";
    info->library_version = "0.1";
    info->valid_extensions = "pss|mcs";
    info->need_fullpath = false;
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->geometry.base_width = PSEMU_LCD_WIDTH;
    info->geometry.base_height = PSEMU_LCD_HEIGHT;
    info->geometry.max_width = PSEMU_LCD_WIDTH;
    info->geometry.max_height = PSEMU_LCD_HEIGHT;
    info->geometry.aspect_ratio = 1.0f;
    info->timing.fps = 32.0;
    info->timing.sample_rate = (double)PSEMU_AUDIO_SAMPLE_RATE_HZ;
}

void retro_reset(void) {
    psemu_reset(g_ps);
}

static void update_input(void) {
    if (!input_poll_cb || !input_state_cb) {
        return;
    }
    input_poll_cb();
    uint32_t buttons = 0;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP)) buttons |= PSEMU_BUTTON_UP;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN)) buttons |= PSEMU_BUTTON_DOWN;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT)) buttons |= PSEMU_BUTTON_LEFT;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) buttons |= PSEMU_BUTTON_RIGHT;
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A)) buttons |= PSEMU_BUTTON_FIRE;
    psemu_set_buttons(g_ps, buttons);
}

static void convert_framebuffer(void) {
    const uint8_t *fb = psemu_get_framebuffer(g_ps);
    for (int row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
            int bit_index = col % 8;
            int on = (fb[byte_index] >> bit_index) & 1;
            g_framebuffer[row * PSEMU_LCD_WIDTH + col] = on ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
}

static void submit_audio(void) {
    if (!audio_batch_cb) {
        return;
    }
    int16_t mono[512];
    int16_t stereo[512 * 2];
    uint32_t n;
    while ((n = psemu_get_audio_samples(g_ps, mono, sizeof(mono) / sizeof(mono[0]))) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            stereo[i * 2 + 0] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        audio_batch_cb(stereo, n);
    }
}

void retro_run(void) {
    update_input();
    psemu_run(g_ps, 33000); /* not yet cycle-accurate, see docs/hardware-notes.md */
    convert_framebuffer();
    video_cb(g_framebuffer, PSEMU_LCD_WIDTH, PSEMU_LCD_HEIGHT, PSEMU_LCD_WIDTH * sizeof(uint32_t));
    submit_audio();
}

/* Real PocketStation hardware needs its 16KB BIOS ROM dumped from a unit
   you own; RetroArch's usual convention is a system file, not a bundled
   asset, since the BIOS is copyrighted Sony firmware. */
static bool load_bios(void) {
    const char *system_dir = NULL;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) || !system_dir) {
        return false;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/pocketstation.bin", system_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t bios[PSEMU_BIOS_SIZE];
    size_t bytes_read = fread(bios, 1, sizeof(bios), f);
    fclose(f);
    if (bytes_read != sizeof(bios)) {
        return false;
    }
    return psemu_load_bios(g_ps, bios, sizeof(bios)) == PSEMU_OK;
}

bool retro_load_game(const struct retro_game_info *game) {
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        return false;
    }
    if (!game || !game->data) {
        return false;
    }
    if (!load_bios()) {
        return false;
    }
    /* Try a bare Title Sector (.pss) first, then a single-save .mcs
       (directory frame + data blocks) - content-sniffed rather than gated
       on the file's extension, same as the rest of this frontend. */
    if (psemu_load_app(g_ps, (const uint8_t *)game->data, game->size) != PSEMU_OK &&
        psemu_load_mcs(g_ps, (const uint8_t *)game->data, game->size) != PSEMU_OK) {
        return false;
    }
    psemu_reset(g_ps);
    return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) {
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

void retro_unload_game(void) {
}

unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

size_t retro_serialize_size(void) {
    return psemu_state_size(g_ps);
}

bool retro_serialize(void *data, size_t size) {
    return psemu_save_state(g_ps, data, size) == PSEMU_OK;
}

bool retro_unserialize(const void *data, size_t size) {
    return psemu_load_state(g_ps, data, size) == PSEMU_OK;
}

void retro_cheat_reset(void) {
}

void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    (void)index;
    (void)enabled;
    (void)code;
}

void *retro_get_memory_data(unsigned id) {
    (void)id;
    return NULL;
}

size_t retro_get_memory_size(unsigned id) {
    (void)id;
    return 0;
}
