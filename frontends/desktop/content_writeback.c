#include "content_writeback.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

/* Where a loaded app's own body sits in flash: physical block 1, right after the directory this
   emulator synthesizes around it (see psemu_load_app). psemu_save_app_image copies from there. */
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

/* Rebuilds the file's bytes from flash, in the shape of whatever kind was loaded. Returns the number of
   bytes to write, or 0 if the kind cannot be written back. */
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
        /* The frame verbatim, then the body as the app left it. See the header on why the loaded
           frame is still correct. */
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
    /* Reuses `current` as the staging buffer. A .mcs is its frame plus a body that can fill 15 of the
       card's 16 blocks, so it still fits inside one card's worth of bytes. */
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
    /* The staging buffer held the file's bytes, not flash's, so re-read flash for the new baseline
       rather than assuming the two match - they do not for a .mcs or .pss. */
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
    /* Only the region the file can represent. See the header on why a wider comparison would rewrite a
       .mcs on a loop over bytes it cannot store. */
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
