/* icon_convert.c - converts a 16x16, 24-bit, uncompressed BMP into the
 * standard PS1 memory-card icon format: a 32-byte palette (16 colors,
 * BGR555, little-endian) followed by a 128-byte bitmap (16x16 pixels,
 * 4 bits/pixel, low nibble = left pixel of each byte's pair).
 *
 * This is a completely separate icon from the PocketStation-specific
 * browse-screen icon at Title Sector body offset 0x100 (see icon.s) - this
 * one lives at body offset 0x60 (palette) / 0x80 (bitmap) and is what a
 * real PS1 console's own memory card manager, or PC-side memory-card
 * management tools, render when browsing a card. Found by diffing a real
 * save-icon-embedding tool's output against this project's own build -
 * see README.md.
 *
 * Host-side build tool only (runs on the machine building the project, not
 * on the PocketStation) - a C compiler is already required for pack.c, so
 * this adds no new dependency.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ICON_W 16
#define ICON_H 16
#define MAX_COLORS 16

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static uint16_t rgb_to_bgr555(rgb_t c) {
    uint16_t r5 = c.r >> 3, g5 = c.g >> 3, b5 = c.b >> 3;
    return (uint16_t)((b5 << 10) | (g5 << 5) | r5);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <in.bmp> <out.bin>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror("fopen bmp");
        return 1;
    }
    uint8_t file_header[14];
    uint8_t dib_header[40];
    if (fread(file_header, 1, sizeof(file_header), f) != sizeof(file_header) ||
        fread(dib_header, 1, sizeof(dib_header), f) != sizeof(dib_header)) {
        fprintf(stderr, "%s: truncated BMP header\n", argv[1]);
        fclose(f);
        return 1;
    }
    if (file_header[0] != 'B' || file_header[1] != 'M') {
        fprintf(stderr, "%s: not a BMP file\n", argv[1]);
        fclose(f);
        return 1;
    }
    uint32_t pixel_offset = (uint32_t)file_header[10] | ((uint32_t)file_header[11] << 8) |
                             ((uint32_t)file_header[12] << 16) | ((uint32_t)file_header[13] << 24);
    uint32_t dib_size = (uint32_t)dib_header[0] | ((uint32_t)dib_header[1] << 8) |
                         ((uint32_t)dib_header[2] << 16) | ((uint32_t)dib_header[3] << 24);
    int32_t width = (int32_t)((uint32_t)dib_header[4] | ((uint32_t)dib_header[5] << 8) |
                               ((uint32_t)dib_header[6] << 16) | ((uint32_t)dib_header[7] << 24));
    int32_t height = (int32_t)((uint32_t)dib_header[8] | ((uint32_t)dib_header[9] << 8) |
                                ((uint32_t)dib_header[10] << 16) | ((uint32_t)dib_header[11] << 24));
    uint16_t bpp = (uint16_t)(dib_header[14] | (dib_header[15] << 8));
    uint32_t compression = (uint32_t)dib_header[16] | ((uint32_t)dib_header[17] << 8) |
                            ((uint32_t)dib_header[18] << 16) | ((uint32_t)dib_header[19] << 24);

    if (dib_size < 40 || width != ICON_W || height != ICON_H || bpp != 24 || compression != 0) {
        fprintf(
            stderr,
            "%s: expected a %dx%d, 24-bit, uncompressed BMP (got %dx%d, %u-bit, compression=%u) - "
            "re-export the icon in that exact format\n",
            argv[1], ICON_W, ICON_H, width, height, (unsigned)bpp, (unsigned)compression);
        fclose(f);
        return 1;
    }

    if (fseek(f, (long)pixel_offset, SEEK_SET) != 0) {
        perror("fseek to pixel data");
        fclose(f);
        return 1;
    }

    /* BMP rows are stored bottom-up, BGR byte order, each row padded to a
     * 4-byte boundary. 16 pixels * 3 bytes = 48 bytes/row, already a
     * multiple of 4, so no padding to skip here - but compute it properly
     * anyway in case this is ever reused for a different width. */
    int row_bytes = ((width * 3 + 3) / 4) * 4;
    uint8_t *raw = (uint8_t *)malloc((size_t)row_bytes * (size_t)height);
    if (!raw || fread(raw, 1, (size_t)row_bytes * (size_t)height, f) != (size_t)row_bytes * (size_t)height) {
        fprintf(stderr, "%s: truncated pixel data\n", argv[1]);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* top[row][col], row 0 = top of the image (BMP storage is bottom-up,
     * so this flips it - the PS1 icon bitmap format is top-down, confirmed
     * by decoding a real embedded reference icon - see README.md). */
    rgb_t top[ICON_H][ICON_W];
    for (int row = 0; row < ICON_H; row++) {
        const uint8_t *src_row = raw + (size_t)row_bytes * (size_t)(ICON_H - 1 - row);
        for (int col = 0; col < ICON_W; col++) {
            uint8_t b = src_row[col * 3 + 0];
            uint8_t g = src_row[col * 3 + 1];
            uint8_t r = src_row[col * 3 + 2];
            top[row][col] = (rgb_t){r, g, b};
        }
    }
    free(raw);

    /* Build a <=16-color palette. Index 0 is forced to the top-left
     * pixel's color (the conventional "background" sample point for most
     * icon converters), matching a real reference icon where index 0 was
     * black and used as the background - other colors are added in
     * first-encountered order after that. */
    rgb_t palette[MAX_COLORS];
    int num_colors = 0;
    uint8_t index_of[ICON_H][ICON_W];

    palette[0] = top[0][0];
    num_colors = 1;

    for (int row = 0; row < ICON_H; row++) {
        for (int col = 0; col < ICON_W; col++) {
            rgb_t c = top[row][col];
            int found = -1;
            for (int i = 0; i < num_colors; i++) {
                if (palette[i].r == c.r && palette[i].g == c.g && palette[i].b == c.b) {
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                if (num_colors >= MAX_COLORS) {
                    fprintf(
                        stderr,
                        "%s: uses more than %d distinct colors - PS1 memory-card icons are 4bpp "
                        "(16 colors max). Re-export with a smaller/flatter color palette.\n",
                        argv[1], MAX_COLORS);
                    return 1;
                }
                found = num_colors;
                palette[num_colors++] = c;
            }
            index_of[row][col] = (uint8_t)found;
        }
    }

    uint8_t out[32 + 128];
    memset(out, 0, sizeof(out));
    for (int i = 0; i < num_colors; i++) {
        uint16_t v = rgb_to_bgr555(palette[i]);
        out[i * 2] = (uint8_t)(v & 0xFF);
        out[i * 2 + 1] = (uint8_t)((v >> 8) & 0xFF);
    }
    /* unused palette entries (num_colors..15) stay zero, matching a real
     * reference icon's own unused entries. */

    for (int row = 0; row < ICON_H; row++) {
        for (int col = 0; col < ICON_W; col += 2) {
            uint8_t lo = index_of[row][col];
            uint8_t hi = index_of[row][col + 1];
            out[32 + row * 8 + col / 2] = (uint8_t)(lo | (hi << 4));
        }
    }

    FILE *of = fopen(argv[2], "wb");
    if (!of) {
        perror("fopen out");
        return 1;
    }
    fwrite(out, 1, sizeof(out), of);
    fclose(of);

    printf("wrote %s (%d bytes, %d colors used)\n", argv[2], (int)sizeof(out), num_colors);
    return 0;
}
