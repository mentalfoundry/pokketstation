#include "content_writeback.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

/* The position of the body of a loaded app in flash: physical block 1, immediately after the
   directory that this emulator synthesizes around the app (see psemu_load_app).
   psemu_save_app_image copies from that position. */
#define APP_BODY_OFFSET 8192u

void content_writeback_arm(
    content_writeback_t *cw, psemu_t *ps, const char *path, const uint8_t *data, size_t size) {
    cw->dirty = 0;
    cw->kind = psemu_identify_content(data, size);
    switch (cw->kind) {
    case PSEMU_CONTENT_CARD:
        cw->region_offset = 0;
        cw->region_size = PSEMU_FLASH_SIZE;
        break;
    case PSEMU_CONTENT_MCS:
        memcpy(cw->mcs_frame, data, sizeof(cw->mcs_frame));
        cw->region_offset = APP_BODY_OFFSET;
        cw->region_size = size - CONTENT_WRITEBACK_MCS_FRAME_SIZE;
        break;
    case PSEMU_CONTENT_APP:
        cw->region_offset = APP_BODY_OFFSET;
        cw->region_size = size;
        break;
    default:
        cw->enabled = 0;
        cw->path[0] = '\0';
        return;
    }
    cw->enabled = 1;
    snprintf(cw->path, sizeof(cw->path), "%s", path);
    psemu_save_flash_image(ps, cw->baseline, sizeof(cw->baseline));
}

void content_writeback_resync(content_writeback_t *cw, psemu_t *ps) {
    if (!cw->enabled) {
        return;
    }
    psemu_save_flash_image(ps, cw->baseline, sizeof(cw->baseline));
    cw->dirty = 0;
}

/* Builds the bytes of the file again from flash, in the shape of the content kind that was loaded.
   Returns the number of bytes to write, or 0 if this code cannot write that kind. */
static size_t build_file_image(content_writeback_t *cw, psemu_t *ps, uint8_t *out, size_t out_cap) {
    switch (cw->kind) {
    case PSEMU_CONTENT_CARD:
        if (out_cap < PSEMU_FLASH_SIZE || psemu_save_flash_image(ps, out, out_cap) != PSEMU_OK) {
            return 0;
        }
        return PSEMU_FLASH_SIZE;
    case PSEMU_CONTENT_MCS:
        if (out_cap < CONTENT_WRITEBACK_MCS_FRAME_SIZE + cw->region_size) {
            return 0;
        }
        /* The unchanged frame, and then the body in the condition that the app left it. See the
           header for the reason that the loaded frame is still correct. */
        memcpy(out, cw->mcs_frame, CONTENT_WRITEBACK_MCS_FRAME_SIZE);
        if (psemu_save_app_image(ps, out + CONTENT_WRITEBACK_MCS_FRAME_SIZE, cw->region_size) != PSEMU_OK) {
            return 0;
        }
        return CONTENT_WRITEBACK_MCS_FRAME_SIZE + cw->region_size;
    case PSEMU_CONTENT_APP:
        if (out_cap < cw->region_size || psemu_save_app_image(ps, out, cw->region_size) != PSEMU_OK) {
            return 0;
        }
        return cw->region_size;
    default:
        return 0;
    }
}

int content_writeback_commit(content_writeback_t *cw, psemu_t *ps) {
    char tmp_path[1088];
    char bak_path[1088];
    uint8_t *image;
    size_t image_size;
    FILE *f;
    if (!cw->enabled || !cw->dirty) {
        return 0;
    }
    /* This code uses `current` as the staging buffer. A .mcs file is its frame and a body. The body
       can fill 15 of the 16 blocks of the card. Thus the file always fits in the bytes of one
       card. */
    image = cw->current;
    image_size = build_file_image(cw, ps, image, sizeof(cw->current));
    if (image_size == 0) {
        fprintf(stderr, "psemu: couldn't rebuild %s from flash - it is unchanged on disk.\n", cw->path);
        cw->dirty = 0;
        return 0;
    }

    /* Only if there is not one already: see the header's note on why a backup is taken exactly once. */
    snprintf(bak_path, sizeof(bak_path), "%s.bak", cw->path);
    if (GetFileAttributesA(bak_path) == INVALID_FILE_ATTRIBUTES) {
        CopyFileA(cw->path, bak_path, TRUE);
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cw->path);
    f = fopen(tmp_path, "wb");
    if (!f || fwrite(image, 1, image_size, f) != image_size) {
        if (f) {
            fclose(f);
        }
        fprintf(stderr, "psemu: couldn't write the updated file to %s - %s is unchanged.\n", tmp_path, cw->path);
        return 0;
    }
    fclose(f);
    if (!MoveFileExA(tmp_path, cw->path, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "psemu: couldn't replace %s with the updated file - it is unchanged.\n", cw->path);
        return 0;
    }
    /* The staging buffer held the bytes of the file, and not the bytes of flash. Thus this code
       reads flash again for the new baseline. It does not assume that the two are the same. They
       are not the same for a .mcs or .pss file. */
    psemu_save_flash_image(ps, cw->baseline, sizeof(cw->baseline));
    cw->dirty = 0;
    fprintf(stderr, "psemu: wrote %zu bytes back to %s\n", image_size, cw->path);
    return 1;
}

void content_writeback_poll(content_writeback_t *cw, psemu_t *ps, unsigned long frame) {
    if (!cw->enabled) {
        return;
    }
    psemu_save_flash_image(ps, cw->current, sizeof(cw->current));
    /* Only the region that the file can hold. See the header for the reason that a larger comparison
       writes a .mcs file again in a loop, over bytes that the file cannot store. */
    if (memcmp(cw->current + cw->region_offset, cw->baseline + cw->region_offset, cw->region_size) != 0) {
        if (!cw->dirty) {
            cw->dirty = 1;
            cw->dirty_since_frame = frame;
        } else if (frame - cw->dirty_since_frame >= CONTENT_WRITEBACK_SETTLE_FRAMES) {
            content_writeback_commit(cw, ps);
        }
        return;
    }
    /* Back to identical: an app that scribbled and undid it leaves nothing to write. */
    cw->dirty = 0;
}
