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

/* The identity of the card immediately after the load, and before the frontend can write its own
   save data over flash. See check_savedata_identity. */
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
    /* This interface is optional in the libretro specification. A frontend does not have to supply
       it, thus each use of the interface has a guard. */
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
    info->library_name = "pokketstation";
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

/* Approximately 6 seconds, at the 32 frames each second of this core. */
#define IDENTITY_WARNING_FRAMES (32 * 6)

/* A frontend copies save data over flash after retro_load_game returns, and before the first
   retro_run. The frontend does no validation: the save file with the name of this content always
   wins. This behavior is usually correct, because that file is the same card at a later time. But
   when the behavior is incorrect, the frontend discards the bytes of the content and gives no error.
   This condition does occur. The save data of this core IS a card image (see
   retro_get_memory_data), thus the documentation tells users to open it in external memory-card
   tools. If a user edits a card in that way, and then loads the card here while an old save file
   from an earlier session is still present, the edit is lost with no message.

   psemu_content_identity_hash gives an answer that is near to this question. The hash covers the
   saves on a card, and it does not include the bytes that an app writes when it saves. Thus a usual
   session still gives the same hash. Examples of a usual session are an app that writes its own
   progress, and an app that edits the PS1 save next to it.

   The hash is not exact. Know this difference before you report it as a fault: the hash also changes
   when the set of files on the card changes. Thus an app that MAKES or DELETES a save file causes
   this warning at the next load, even for the correct card. This condition is rare for PocketStation
   apps, which stay in their own blocks and write in them. The result is one incorrect warning, and
   no data loss. Thus this is the correct compromise against a failure to find the real condition.

   This code hashes flash on both sides, before and after. It does not compare the loaded file against
   flash. A .mcs or .pss file and the card that this emulator synthesizes around it hash in different
   domains: one is a file, and the other is a card. Thus a comparison of those two has no meaning. A
   comparison of flash before against flash after uses the same domain, for all three content kinds.

   The save data still wins. To refuse the save data can discard the real progress of a session
   because of an incorrect warning, which is a much worse failure. This code only makes the
   replacement visible. */
static void check_savedata_identity(void) {
    struct retro_message msg;
    if (psemu_content_identity_hash(psemu_flash_data(g_ps), PSEMU_FLASH_SIZE) == g_loaded_identity) {
        return;
    }
    if (log_cb) {
        log_cb(RETRO_LOG_WARN,
               "[pokketstation] Save data holds a different memory card than the loaded content, and "
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
    psemu_run(g_ps, 33000); /* the cycle budget at PSEMU_ASSUMED_CPU_HZ. See docs/hardware-notes.md. */
    convert_framebuffer();
    video_cb(g_framebuffer, PSEMU_LCD_WIDTH, PSEMU_LCD_HEIGHT, PSEMU_LCD_WIDTH * sizeof(uint32_t));
    submit_audio();
}

/* The BIOS is copyrighted Sony firmware. This project does not supply it.
   Make a dump of the 16KB BIOS ROM from real hardware that you own.
   A libretro frontend usually loads such a file from its system directory. */
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
    /* This emulator examines the file content. It does not use the file extension.
       See the comment on psemu_load_content for the exact order of priority. */
    if (psemu_load_content(g_ps, (const uint8_t *)game->data, game->size) != PSEMU_OK) {
        return false;
    }
    psemu_reset(g_ps);
    /* Record the hash now, while flash holds only the data from the content. */
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

/* SAVE_RAM is the full 128KB FLASH2 image. This region is what keeps the progress of an app. Before
   this code, nothing continued after a session except a manual save state, and the saves of a session
   were lost at exit.
   Flash is the correct region, and a smaller region is not. The output of a PocketStation app is
   frequently an edit to the PS1 save of the game in a different block of the same card, and not the
   state of the app itself (see docs/app-notes.md). Thus a smaller region discards the exact edits
   that users need.

   THE SAVE FILE THAT THIS PRODUCES IS A .mcr FILE, BYTE FOR BYTE. This behavior is deliberate, and it
   depends on this region staying a flat PSEMU_FLASH_SIZE region. See the contract of
   psemu_flash_data, and the saves section of docs/libretro_readme.md. That section tells users to
   open the save file directly in external memory-card tools.
   All three content kinds operate this way, because a loaded .mcs or .pss file runs inside a full
   card that this emulator synthesizes around it.

   SYSTEM_RAM is work RAM. This core supplies it for the cheat-search and memory-viewer functions of a
   host. A frontend does not keep this region, which is correct: it is live state, and not save
   data. */
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
