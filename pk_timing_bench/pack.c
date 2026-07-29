/* pack.c - wraps the assembled+linked 8KB PocketStation app body (produced
 * by the Makefile via arm-none-eabi-as/ld/objcopy) in a real PS1
 * single-save directory frame, producing the final pk_timing_bench.mcs.
 * See README.md for what this app does and docs/app-notes.md for the
 * container format. No Python, no scripting language - just a C compiler,
 * matching the rest of this project's toolchain. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BODY_SIZE 0x2000u

static uint8_t directory_frame_xor(const uint8_t *frame) {
    uint8_t x = 0;
    int i;
    for (i = 0; i < 0x7F; i++) {
        x ^= frame[i];
    }
    return x;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <body.bin> <out.mcs>\n", argv[0]);
        return 1;
    }

    FILE *bf = fopen(argv[1], "rb");
    if (!bf) {
        perror("fopen body");
        return 1;
    }
    static uint8_t body[BODY_SIZE];
    size_t n = fread(body, 1, BODY_SIZE, bf);
    fclose(bf);
    if (n != BODY_SIZE) {
        fprintf(stderr, "%s is %zu bytes, expected exactly %u\n", argv[1], n, BODY_SIZE);
        return 1;
    }

    uint8_t frame[0x80];
    memset(frame, 0, sizeof(frame));
    frame[0x00] = 0x51; /* solo block */
    frame[0x04] = (uint8_t)(BODY_SIZE & 0xFFu);
    frame[0x05] = (uint8_t)((BODY_SIZE >> 8) & 0xFFu);
    frame[0x06] = (uint8_t)((BODY_SIZE >> 16) & 0xFFu);
    frame[0x07] = (uint8_t)((BODY_SIZE >> 24) & 0xFFu);
    frame[0x08] = 0xFF;
    frame[0x09] = 0xFF; /* end of chain */

    /* Product-code-style identifier (20 bytes @ 0x0A-0x1D), mirroring the
     * real PS1 convention of a maker/region prefix + product code, but
     * made up since there's no real registered code for homebrew -
     * "BAPKTM" (6-byte prefix, echoing real codes like "BASLUS") + the
     * mandatory 'P' flag at byte 0x10 (see docs/app-notes.md's
     * "App-selection and dispatch" section for why that byte is
     * load-bearing) + a readable suffix, filling the field exactly. */
    memcpy(&frame[0x0A], "BAPKTM"
                          "P"
                          "TIMEBENCH0001",
           20);

    frame[0x7F] = directory_frame_xor(frame);

    FILE *of = fopen(argv[2], "wb");
    if (!of) {
        perror("fopen out");
        return 1;
    }
    fwrite(frame, 1, sizeof(frame), of);
    fwrite(body, 1, BODY_SIZE, of);
    fclose(of);

    printf("wrote %s (%u bytes)\n", argv[2], (unsigned)(sizeof(frame) + BODY_SIZE));
    return 0;
}
