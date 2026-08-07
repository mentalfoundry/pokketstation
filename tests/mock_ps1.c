/* SPDX-FileCopyrightText: Copyright (c) 2026 Darien Liu (mentalfoundry)
   SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_ps1.h"

/* THIS MODULE USES THE PUBLIC HEADER ONLY. A PS1 emulator gets the same header and no more, thus a
   private include here would hide a gap in that header.

   The per-frame cycle budget of a frontend, at the 32Hz refresh rate of the LCD. */
#define FRAME_CYCLES (PSEMU_REFERENCE_CLOCK_HZ / 32u)

/* The frames that a boot needs before the kernel gets to its shell. */
#define BOOT_FRAMES 200u

/* The frames that a dock operation can need before the kernel enables communication. */
#define DOCK_FRAMES 60u

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *buf;

    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
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

mock_ps1_t *mock_ps1_open(const char *bios_path, const char *card_path) {
    mock_ps1_t *m;
    psemu_t *ps;
    uint8_t *bios;
    size_t bios_size = 0;
    unsigned i;
    int enabled = 0;

    bios = read_file(bios_path, &bios_size);
    if (!bios) {
        return NULL;
    }
    ps = psemu_create();
    if (!ps) {
        free(bios);
        return NULL;
    }
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        free(bios);
        psemu_destroy(ps);
        return NULL;
    }
    free(bios);
    psemu_reset(ps);

    if (card_path) {
        size_t card_size = 0;
        uint8_t *card = read_file(card_path, &card_size);
        if (!card) {
            psemu_destroy(ps);
            return NULL;
        }
        if (psemu_load_content(ps, card, card_size) != PSEMU_OK) {
            free(card);
            psemu_destroy(ps);
            return NULL;
        }
        free(card);
    }

    for (i = 0; i < BOOT_FRAMES; i++) {
        psemu_run(ps, FRAME_CYCLES);
    }

    /* The kernel enables communication from its IRQ-11 handler. That handler waits before it reads
       the docking level again. The wait skips the switch-bounce period of a real connector. Thus
       this loop runs the machine after the transition. One frame can be too few. */
    psemu_com_set_docked(ps, 1);
    for (i = 0; i < DOCK_FRAMES; i++) {
        psemu_run(ps, FRAME_CYCLES);
        if (psemu_com_is_enabled(ps)) {
            enabled = 1;
            break;
        }
    }
    if (!enabled) {
        psemu_destroy(ps);
        return NULL;
    }

    m = (mock_ps1_t *)malloc(sizeof(mock_ps1_t));
    if (!m) {
        psemu_destroy(ps);
        return NULL;
    }
    m->ps = ps;
    m->timeout_cycles = PSEMU_COM_DEFAULT_TIMEOUT_CYCLES;
    m->settle_frames = 8u;
    return m;
}

void mock_ps1_close(mock_ps1_t *m) {
    if (!m) {
        return;
    }
    psemu_destroy(m->ps);
    free(m);
}

size_t mock_ps1_exchange(mock_ps1_t *m, const uint8_t *send, uint8_t *reply, size_t count) {
    size_t i;

    for (i = 0; i < count; i++) {
        uint8_t out = 0xFFu;
        int ack = psemu_com_transfer(m->ps, send[i], &out, m->timeout_cycles);
        reply[i] = out;
        if (!ack) {
            return i + 1u;
        }
    }
    return count;
}

void mock_ps1_run_frames(mock_ps1_t *m, unsigned frames) {
    unsigned i;

    for (i = 0; i < frames; i++) {
        psemu_run(m->ps, FRAME_CYCLES);
    }
}

void mock_ps1_end_command(mock_ps1_t *m) {
    /* The machine runs before the release, so the kernel can reach its end-of-command wait. See
       mock_ps1_end_command in mock_ps1.h for the reason. */
    psemu_com_set_selected(m->ps, 0);
    mock_ps1_run_frames(m, m->settle_frames);
}

uint8_t mock_ps1_checksum(uint16_t sector, const uint8_t *data) {
    uint8_t checksum = (uint8_t)((sector >> 8) & 0xFFu) ^ (uint8_t)(sector & 0xFFu);
    unsigned i;

    for (i = 0; i < MOCK_PS1_FRAME_SIZE; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

size_t mock_ps1_find_id_pair(const uint8_t *reply, size_t count) {
    size_t i;

    if (count < 2u) {
        return MOCK_PS1_NOT_FOUND;
    }
    for (i = 0; i + 1u < count; i++) {
        if (reply[i] == MOCK_PS1_ID1 && reply[i + 1u] == MOCK_PS1_ID2) {
            return i;
        }
    }
    return MOCK_PS1_NOT_FOUND;
}

size_t mock_ps1_get_id(mock_ps1_t *m, uint8_t *reply) {
    static const uint8_t SEQ[] = {MOCK_PS1_SEL_CARD, 0x53u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u};

    return mock_ps1_exchange(m, SEQ, reply, sizeof(SEQ));
}

size_t mock_ps1_read_sector(mock_ps1_t *m, uint16_t sector, uint8_t *out_data, uint8_t *out_reply) {
    uint8_t send[MOCK_PS1_READ_REPLY_SIZE];
    uint8_t reply[MOCK_PS1_READ_REPLY_SIZE];
    size_t n;
    size_t id;

    memset(send, 0, sizeof(send));
    send[0] = MOCK_PS1_SEL_CARD;
    send[1] = 0x52u;
    send[4] = (uint8_t)((sector >> 8) & 0xFFu);
    send[5] = (uint8_t)(sector & 0xFFu);

    n = mock_ps1_exchange(m, send, reply, sizeof(send));
    if (out_reply) {
        memcpy(out_reply, reply, sizeof(reply));
    }

    /* The reply stream after the identifier pair holds two dummy bytes, the two acknowledge bytes,
       the two bytes that repeat the sector number, and then the data. That distance is fixed against
       the identifier pair, thus the shift of the output register does not move it.
       MOCK_PS1_READ_DATA_OFFSET holds the distance. */
    if (out_data) {
        memset(out_data, 0, MOCK_PS1_FRAME_SIZE);
        id = mock_ps1_find_id_pair(reply, n);
        if (id != MOCK_PS1_NOT_FOUND && id + MOCK_PS1_READ_DATA_OFFSET + MOCK_PS1_FRAME_SIZE <= n) {
            memcpy(out_data, &reply[id + MOCK_PS1_READ_DATA_OFFSET], MOCK_PS1_FRAME_SIZE);
        }
    }
    return n;
}

size_t mock_ps1_write_sector(mock_ps1_t *m, uint16_t sector, const uint8_t *data, int corrupt_checksum,
    uint8_t *out_term, uint8_t *out_reply) {
    uint8_t send[MOCK_PS1_WRITE_REPLY_SIZE];
    uint8_t reply[MOCK_PS1_WRITE_REPLY_SIZE];
    uint8_t checksum = mock_ps1_checksum(sector, data);
    size_t n;

    if (corrupt_checksum) {
        checksum ^= 0xFFu;
    }

    memset(send, 0, sizeof(send));
    send[0] = MOCK_PS1_SEL_CARD;
    send[1] = 0x57u;
    send[4] = (uint8_t)((sector >> 8) & 0xFFu);
    send[5] = (uint8_t)(sector & 0xFFu);
    memcpy(&send[6], data, MOCK_PS1_FRAME_SIZE);
    send[6 + MOCK_PS1_FRAME_SIZE] = checksum;
    /* The bytes after the checksum are the two acknowledge bytes and the terminator. The exact
       position of the terminator depends on the shift of the output register. Thus this function
       sends dummy bytes, and mock_ps1_exchange stops at the missing acknowledge. The reply of that
       last exchange is the terminator. */

    n = mock_ps1_exchange(m, send, reply, sizeof(send));
    if (out_reply) {
        memcpy(out_reply, reply, sizeof(reply));
    }
    if (out_term) {
        *out_term = (n > 0u) ? reply[n - 1u] : 0xFFu;
    }
    return n;
}
