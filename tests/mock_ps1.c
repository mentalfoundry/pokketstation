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

mock_ps1_t *mock_ps1_open_from_flash(const char *bios_path,
                                      const uint8_t *flash_data, size_t flash_size) {
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
    if (psemu_load_flash_image(ps, flash_data, flash_size) != PSEMU_OK) {
        psemu_destroy(ps);
        return NULL;
    }
    psemu_reset(ps);
    for (i = 0; i < BOOT_FRAMES; i++) {
        psemu_run(ps, FRAME_CYCLES);
    }
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

mock_ps1_t *mock_ps1_open_from_state(const char *bios_path,
                                      const uint8_t *flash_data, size_t flash_size,
                                      const void *state_data, size_t state_size) {
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
    if (psemu_load_flash_image(ps, flash_data, flash_size) != PSEMU_OK) {
        psemu_destroy(ps);
        return NULL;
    }
    if (psemu_load_state(ps, state_data, state_size) != PSEMU_OK) {
        psemu_destroy(ps);
        return NULL;
    }
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
    /* SELECT releases before the frames run. The kernel finds the released level in its
       end-of-command wait. See mock_ps1_end_command in mock_ps1.h. */
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

int mock_ps1_launch_app(mock_ps1_t *m, unsigned slot) {
    uint8_t cmd59[9];
    uint8_t reply[9];
    unsigned i;

    /* Format per psx-spx: 81 59 00(dummy) dir_hi dir_lo param0..param3.
       Byte 2 is a dummy zero. The kernel replies with the data length and exits early
       when it receives any nonzero value there. */
    cmd59[0] = 0x81u;
    cmd59[1] = 0x59u;
    cmd59[2] = 0x00u;
    cmd59[3] = (uint8_t)((slot >> 8) & 0xFFu);
    cmd59[4] = (uint8_t)(slot & 0xFFu);
    cmd59[5] = 0x00u;
    cmd59[6] = 0x00u;
    cmd59[7] = 0x00u;
    cmd59[8] = 0x00u;
    mock_ps1_exchange(m, cmd59, reply, sizeof(cmd59));
    mock_ps1_end_command(m);

    for (i = 0; i < MOCK_PS1_LAUNCH_MAX_FRAMES; i++) {
        psemu_run(m->ps, FRAME_CYCLES);
        if (psemu_app_running(m->ps)) {
            return 1;
        }
    }
    return 0;
}

size_t mock_ps1_dispatch(mock_ps1_t *m, uint8_t cmd, const uint8_t *data, size_t payload_size,
                         uint8_t *out_reply) {
    uint8_t send[2u + MOCK_PS1_DISPATCH_MAX_PAYLOAD];
    uint8_t reply[2u + MOCK_PS1_DISPATCH_MAX_PAYLOAD];
    size_t cmd_size = 2u + payload_size;
    size_t i;
    size_t n = 0;

    send[0] = MOCK_PS1_SEL_CARD;
    send[1] = cmd;
    for (i = 0; i < payload_size; i++) {
        send[2u + i] = data[i];
    }

    for (i = 0; i < cmd_size; i++) {
        uint8_t out = 0xFFu;
        int ack = psemu_com_transfer(m->ps, send[i], &out, m->timeout_cycles);
        reply[i] = out;
        n++;
        if (!ack) {
            break;
        }
    }
    if (out_reply) {
        memcpy(out_reply, reply, n);
    }
    return n;
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
