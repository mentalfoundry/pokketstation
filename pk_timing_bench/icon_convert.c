/* icon_convert.c converts a 16x16, 24-bit, uncompressed BMP file into the
 * standard PS1 memory-card icon format. That format is a palette of 32 bytes
 * (16 colors, BGR555, little-endian), and then a bitmap of 128 bytes (16x16
 * pixels, 4 bits for each pixel, where the low nibble is the left pixel of
 * each byte).
 *
 * This icon is separate from the PocketStation browse-screen icon at Title
 * Sector body offset 0x100 (see icon.s). This icon is at body offset 0x60
 * (the palette) and 0x80 (the bitmap). A real PS1 console memory card
 * manager, or a memory-card tool on a computer, shows this icon during a
 * browse operation. This project found the format by a comparison of the
 * output of a real save-icon tool against its own build. See README.md.
 *
 * This is a host-side build tool only. It executes on the machine that builds
 * the project, and not on the PocketStation. pack.c already needs a C
 * compiler, thus this tool adds no new dependency.
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

    /* A BMP file stores its rows from the bottom to the top, in BGR byte
     * order. Each row has padding to a 4-byte boundary. Here, 16 pixels
     * multiplied by 3 bytes gives 48 bytes for each row, which is already a
     * multiple of 4. Thus there is no padding here. But this code calculates
     * the padding correctly, for a later use with a different width. */
    int row_bytes = ((width * 3 + 3) / 4) * 4;
    uint8_t *raw = (uint8_t *)malloc((size_t)row_bytes * (size_t)height);
    if (!raw || fread(raw, 1, (size_t)row_bytes * (size_t)height, f) != (size_t)row_bytes * (size_t)height) {
        fprintf(stderr, "%s: truncated pixel data\n", argv[1]);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* top[row][col], where row 0 is the top of the image. A BMP file stores
     * the rows from the bottom, thus this code reverses the order. The PS1
     * icon bitmap format stores the rows from the top. A decode of a real
     * reference icon confirms this. See README.md. */
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

    /* Build a palette of 16 colors or less. Index 0 always gets the color of
     * the top-left pixel. Most icon converters use that pixel as the
     * "background" sample point. This agrees with a real reference icon,
     * where index 0 was black and was the background color. This code then
     * adds each other color in the order that it finds them. */
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
    /* The unused palette entries, from num_colors to 15, stay at zero. This
     * agrees with the unused entries of a real reference icon. */

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
