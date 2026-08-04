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
static retro_log_printf_t log_cb;

/* The card's identity as it stood right after the load, before the frontend had any chance to
   overwrite flash with its own save data. See check_savedata_identity. */
static uint32_t g_loaded_identity;
static bool g_identity_check_pending;

void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

void retro_init(void) {
    struct retro_log_callback log;
    g_ps = psemu_create();
    /* Optional, per the libretro spec: a frontend need not provide this, so every use is guarded. */
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
        log_cb = log.log;
    }
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
    info->valid_extensions = "mcr|mcs|pss";
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

/* ~6s at this core's 32fps. */
#define IDENTITY_WARNING_FRAMES (32 * 6)

/* A frontend copies save data over flash after retro_load_game returns and before the first
   retro_run, with no validation of any kind: whatever save file is named after this content wins.
   That is usually right - same card, further along - but it silently discards the content's own bytes
   when it is wrong, and the case that bites is a real one. Because this core's save data IS a card
   image (see retro_get_memory_data), users are told to open it in external memory-card tools; edit a
   card that way, load it here while a stale save file from an older session is still sitting next to
   it, and the edit is gone without a word.

   psemu_content_identity_hash answers close to this question. It covers which saves live on a card and
   deliberately excludes the bytes an app rewrites when it saves, so an ordinary session - an app
   writing its own progress, or editing the PS1 save next to it - still matches afterwards.

   It is not exact, and the gap is worth knowing before someone reports it as a bug: the hash also
   moves when the set of files on the card changes, so an app that CREATES or DELETES a save file will
   trip this on the next load even though the card is the right one. That is rare for PocketStation
   apps, which live in their own blocks and write within them, and the consequence is one spurious
   warning rather than any data loss - so it is the right trade against missing the real case.

   Both sides are hashed as flash, before and after, rather than comparing the loaded file against
   flash. A .mcs/.pss file and the card synthesized around it hash in different domains - one is a
   file, the other a card - so comparing those two would be meaningless. Comparing flash-then against
   flash-now is the same domain for all three content kinds.

   The save data still wins. Refusing it would risk throwing away a real session's progress over a
   false positive, which is the worse failure by far. This only makes the swap visible. */
static void check_savedata_identity(void) {
    struct retro_message msg;
    if (psemu_content_identity_hash(psemu_flash_data(g_ps), PSEMU_FLASH_SIZE) == g_loaded_identity) {
        return;
    }
    if (log_cb) {
        log_cb(RETRO_LOG_WARN,
               "[PokketStation] Save data holds a different memory card than the loaded content, and "
               "has replaced it. If you edited this card outside RetroArch, delete its .srm and "
               "reload.\n");
    }
    msg.msg = "Save data is from a different memory card, and has replaced the loaded content.";
    msg.frames = IDENTITY_WARNING_FRAMES;
    if (environ_cb) {
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
}

void retro_run(void) {
    if (g_identity_check_pending) {
        g_identity_check_pending = false;
        check_savedata_identity();
    }
    update_input();
    psemu_run(g_ps, 33000); /* cycle budget at PSEMU_ASSUMED_CPU_HZ, see docs/hardware-notes.md */
    convert_framebuffer();
    video_cb(g_framebuffer, PSEMU_LCD_WIDTH, PSEMU_LCD_HEIGHT, PSEMU_LCD_WIDTH * sizeof(uint32_t));
    submit_audio();
}

/* The BIOS is copyrighted Sony firmware, not a bundled asset.
   Dump the 16KB BIOS ROM from real hardware you own.
   RetroArch's usual convention is to load this as a system file. */
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
    /* This emulator sniffs file content, instead of checking the file extension.
       See psemu_load_content's own doc comment for the exact priority order. */
    if (psemu_load_content(g_ps, (const uint8_t *)game->data, game->size) != PSEMU_OK) {
        return false;
    }
    psemu_reset(g_ps);
    /* Snapshot now, while flash still holds what the content put there and nothing else. */
    g_loaded_identity = psemu_content_identity_hash(psemu_flash_data(g_ps), PSEMU_FLASH_SIZE);
    g_identity_check_pending = true;
    return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) {
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

void retro_unload_game(void) {
    g_identity_check_pending = false;
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

/* SAVE_RAM is the whole 128KB FLASH2 image, which is what makes an app's progress survive at all -
   before this, nothing persisted except a manual save state, and a session's saves died on exit.
   Flash is the right region rather than any narrower one: a PocketStation app's output is frequently
   an edit to the PS1 game's save in another block of the same card, not its own state (see
   docs/app-notes.md), so anything smaller would drop exactly the edits people care about.

   THE .srm THIS PRODUCES IS A .mcr, byte for byte. That is deliberate and depends on this staying a
   flat PSEMU_FLASH_SIZE region - see psemu_flash_data's contract, and the saves section of
   docs/libretro_readme.md, which tells users to open the .srm directly in external memory-card tools.
   All three content kinds behave this way, because a loaded .mcs/.pss runs inside a full card this
   emulator synthesizes around it.

   SYSTEM_RAM is work RAM, exposed for RetroArch's cheat search and memory viewers. The frontend does
   not persist it, which is correct - it is live state, not save data. */
void *retro_get_memory_data(unsigned id) {
    if (!g_ps) {
        return NULL;
    }
    switch (id) {
    case RETRO_MEMORY_SAVE_RAM:
        return psemu_flash_data(g_ps);
    case RETRO_MEMORY_SYSTEM_RAM:
        return psemu_ram_data(g_ps);
    default:
        return NULL;
    }
}

size_t retro_get_memory_size(unsigned id) {
    switch (id) {
    case RETRO_MEMORY_SAVE_RAM:
        return PSEMU_FLASH_SIZE;
    case RETRO_MEMORY_SYSTEM_RAM:
        return PSEMU_RAM_SIZE;
    default:
        return 0;
    }
}
