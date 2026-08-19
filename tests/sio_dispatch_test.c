/* SPDX-FileCopyrightText: Copyright (c) 2026 Darien Liu (mentalfoundry)
   SPDX-License-Identifier: MIT */

/* See the comment at the top of cpu_test.c for why NDEBUG must stay clear. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_ps1.h"

/* SIO dispatch command (0x5B/0x5C) end-of-command protocol tests.

   The kernel FIQ handler receives each byte of a dispatch command while SELECT is asserted.
   After all payload bytes it calls the app dispatch entry twice. Phase-1 (opcode 0x0A) returns
   the app receive buffer. Phase-2 (opcode 0x12 on success, 0x16 on error) delivers the data.
   The FIQ then enters a SELECT-drop wait with no timeout, polling COM_STAT1 bit 1. When that
   bit is 1, the FIQ runs its dispatch processor and returns to the app.

   SELECT drops via mock_ps1_end_command after mock_ps1_dispatch returns. The FIQ exits its
   wait, the app continues from where the FIQ interrupted it, and when phase-2 succeeded the
   app writes flash.

   Flash change from the BIOS phase-2 handler is the observable for each test. That write
   happens inside mock_ps1_dispatch, before mock_ps1_end_command runs the settle frames.
   The test checks flash before calling mock_ps1_end_command.

   THESE TESTS NEED A BIOS DUMP AND A DISPATCH APP. The repository excludes each dump.
   This suite skips itself with exit code 77 when it finds no dump or no app. Set
   PSEMU_TEST_BIOS and PSEMU_TEST_DISPATCH_APP to supply alternate paths.

   THE RAM ADDRESSES BELOW ARE SPECIFIC TO THE APP UNDER TEST. A different app uses different
   addresses. Verify each address against the target app before use. The addresses come from a
   PC trace of the app while the debug build ran a 0x5C command. See docs/hardware-notes.md. */

#define SKIP_EXIT_CODE 77

/* The RAM byte that the kernel sets while a dispatch command is active.
   The FIQ cleanup path sets this byte to 0. The app function exits its wait when it reads 0.
   Address within the 0x800-byte work RAM that psemu_ram_data gives. */
#define CMD_ACTIVE_FLAG  0x30Eu

/* The outer-loop exit flag that the app checks before it commits a flash write.
   The FIQ cleanup path sets this byte to 0. */
#define OUTER_LOOP_FLAG  0x31Cu

/* The directory slot where psemu_load_mcs places an app. */
#define APP_SLOT 1u

/* Settle frames after a dispatch command. The app needs time to exit its counter wait and
   to run its flash write after the FIQ cleanup path fires. */
#define DISPATCH_SETTLE_FRAMES 60u

/* The command byte for the write dispatch function. */
#define CMD_WRITE 0x5Cu

/* Function number 0x01 in the dispatch table: writes the character save to flash. */
#define FN_WRITE 0x01u

/* Payload layout for the fn#1 write command:
   byte 0 : fn# (FN_WRITE)
   byte 1 : discarded
   bytes 2-5 : destination address, little-endian
   byte 6 : byte count N
   byte 7 : dummy info byte (the kernel sends N back to the PS1 during this exchange)
   bytes 8..8+N-1 : N data bytes that the kernel copies to the destination
   Total payload = 8 + N bytes. For N=1: FN_WRITE_PAYLOAD_SIZE = 9.
   Total bytes on the wire = 2 (device + command) + FN_WRITE_PAYLOAD_SIZE = 11.
   The kernel ACKs all 11 bytes on a correct transfer. */
#define FN_WRITE_N            1u
#define FN_WRITE_PAYLOAD_SIZE (8u + FN_WRITE_N)

/* Physical flash (FLASH2) address that the kernel fn#1 handler writes to.
   0x08000000 is FLASH2 base. Offset 0x800 sits in the gap between the directory frames
   (0x000-0x7FF) and the app data (0x2000+). flash_load_app zeros that range.
   Any write there is visible as a flash change without touching directory or app code. */
#define FN_WRITE_DEST 0x08000800u

/* Data byte written to FN_WRITE_DEST. Must differ from the value at that offset before the
   write, which is 0x00 after flash_load_app. */
#define FN_WRITE_DATA 0x5Au

static const char *bios_path(void) {
    const char *env = getenv("PSEMU_TEST_BIOS");

    if (env && env[0]) {
        return env;
    }
    return PSEMU_TESTDATA_DIR "/J110.bin";
}

static const char *app_path(void) {
    const char *env = getenv("PSEMU_TEST_DISPATCH_APP");

    if (env && env[0]) {
        return env;
    }
    return PSEMU_TESTDATA_DIR "/choco.mcs";
}

/* Opens a fresh machine with the BIOS and app loaded, docks, and starts the app with
   command 0x59. Returns NULL if the BIOS or app is absent, or if the app does not start. */
static mock_ps1_t *open_with_app(const char *bios, const char *app) {
    mock_ps1_t *m = mock_ps1_open(bios, app);

    if (!m) {
        return NULL;
    }
    if (!mock_ps1_launch_app(m, APP_SLOT)) {
        printf("  app did not start at slot %u\n", APP_SLOT);
        mock_ps1_close(m);
        return NULL;
    }
    return m;
}

/* Sends a 0x5C dispatch command where SELECT drops before the PS1 sends the last byte.
   Bytes 0 to cmd_size-2 use psemu_com_transfer. SELECT then drops without the last byte.
   The FIQ byte-receive poll (0x04001570) finds rx_done=0 and sel_drop_latch=1 and returns
   an error. Phase-2 receives the error opcode. No data copies to flash. */
static size_t dispatch_early_select_drop(mock_ps1_t *m, uint8_t cmd, const uint8_t *data,
                                         size_t payload_size, uint8_t *out_reply) {
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
    for (i = 0; i < cmd_size - 1u; i++) {
        uint8_t out = 0xFFu;
        int ack = psemu_com_transfer(m->ps, send[i], &out, m->timeout_cycles);
        reply[i] = out;
        n++;
        if (!ack) {
            break;
        }
    }
    /* Drop SELECT without sending the last byte. The FIQ byte-receive poll checks rx_done
       before sel_drop_latch in each iteration. With rx_done=0 and sel_drop_latch=1, the poll
       finds rx_done not set, then finds sel_drop_latch set, and returns an error. Phase-2
       receives the error opcode and copies nothing to the destination. The error path runs
       during the settle frames of the subsequent mock_ps1_end_command call. */
    psemu_com_set_selected(m->ps, 0);
    if (out_reply) {
        memcpy(out_reply, reply, n);
    }
    return n;
}

/* Builds a FN_WRITE_PAYLOAD_SIZE-byte payload for a 0x5C function-0x01 (write) command.
   The payload directs the kernel to copy FN_WRITE_N bytes to FN_WRITE_DEST in flash. */
static void make_write_payload(uint8_t *data) {
    memset(data, 0, FN_WRITE_PAYLOAD_SIZE);
    data[0] = FN_WRITE;
    /* destination address, little-endian */
    data[2] = (uint8_t)( FN_WRITE_DEST        & 0xFFu);
    data[3] = (uint8_t)((FN_WRITE_DEST >>  8) & 0xFFu);
    data[4] = (uint8_t)((FN_WRITE_DEST >> 16) & 0xFFu);
    data[5] = (uint8_t)((FN_WRITE_DEST >> 24) & 0xFFu);
    data[6] = FN_WRITE_N;    /* byte count */
    /* data[7] = 0x00 (dummy info byte) */
    data[8] = FN_WRITE_DATA; /* first and only data byte */
}

/* Counts the number of bytes that differ between two flash images. */
static unsigned count_flash_diff(const uint8_t *before, const uint8_t *after) {
    unsigned changed = 0u;
    unsigned i;

    for (i = 0u; i < PSEMU_FLASH_SIZE; i++) {
        if (after[i] != before[i]) {
            changed++;
        }
    }
    return changed;
}

/* After a SELECT-drop dispatch the FIQ runs its cleanup path. The app exits its counter wait
   and commits a flash write. */
static void test_dispatch_with_select_drop_writes_flash(const char *bios, const char *app) {
    static uint8_t flash_before[PSEMU_FLASH_SIZE];
    uint8_t payload[FN_WRITE_PAYLOAD_SIZE];
    mock_ps1_t *m = open_with_app(bios, app);
    const uint8_t *ram;
    unsigned changed;
    size_t n;

    if (!m) {
        printf("test_dispatch_with_select_drop_writes_flash SKIP (no app)\n");
        return;
    }
    make_write_payload(payload);

    ram = psemu_ram_data(m->ps);
    printf("  pre-dispatch: app_running=%d cpu_faulted=%d\n",
           psemu_app_running(m->ps), psemu_cpu_faulted(m->ps));
    printf("  pre-dispatch: RAM[0x%03X]=0x%02X RAM[0x%03X]=0x%02X\n",
           CMD_ACTIVE_FLAG, (unsigned)ram[CMD_ACTIVE_FLAG],
           OUTER_LOOP_FLAG, (unsigned)ram[OUTER_LOOP_FLAG]);
    /* The BIOS FIQ cleanup gate: bit 30 of the word at RAM[0xC0] must be set. */
    printf("  pre-dispatch: RAM[0x0C0..0x0C3] = %02X %02X %02X %02X (word=0x%08X, bit30=%d)\n",
           ram[0xC0], ram[0xC1], ram[0xC2], ram[0xC3],
           (unsigned)(ram[0xC0] | (ram[0xC1]<<8) | (ram[0xC2]<<16) | (ram[0xC3]<<24)),
           (int)((ram[0xC0] | (ram[0xC1]<<8) | (ram[0xC2]<<16) | (ram[0xC3]<<24)) >> 30) & 1);
    memcpy(flash_before, psemu_flash_data(m->ps), PSEMU_FLASH_SIZE);

    n = mock_ps1_dispatch(m, CMD_WRITE, payload, FN_WRITE_PAYLOAD_SIZE, NULL);
    printf("  dispatch bytes sent: %u (expected %u)\n", (unsigned)n, 2u + FN_WRITE_PAYLOAD_SIZE);

    /* The kernel fn#1 phase-2 handler copies data to flash inside the last psemu_com_transfer
       call in mock_ps1_dispatch. Check flash here, before end_command runs settle frames and
       the app runs its own post-dispatch code. */
    changed = count_flash_diff(flash_before, psemu_flash_data(m->ps));
    printf("  flash bytes changed after dispatch: %u\n", changed);
    assert(changed > 0u);

    m->settle_frames = DISPATCH_SETTLE_FRAMES;
    mock_ps1_end_command(m);

    mock_ps1_close(m);
    printf("test_dispatch_with_select_drop_writes_flash OK\n");
}

/* Regression guard: SELECT drops before the PS1 sends the last byte. The FIQ byte-receive
   poll finds rx_done=0 and sel_drop_latch=1 and returns an error. Phase-2 receives the
   error opcode. The BIOS copies nothing to flash. */
static void test_dispatch_without_select_drop_does_not_write_flash(const char *bios,
                                                                   const char *app) {
    static uint8_t flash_before[PSEMU_FLASH_SIZE];
    uint8_t payload[FN_WRITE_PAYLOAD_SIZE];
    mock_ps1_t *m = open_with_app(bios, app);
    const uint8_t *ram;
    unsigned changed;

    if (!m) {
        printf("test_dispatch_without_select_drop_does_not_write_flash SKIP (no app)\n");
        return;
    }
    make_write_payload(payload);
    memcpy(flash_before, psemu_flash_data(m->ps), PSEMU_FLASH_SIZE);

    dispatch_early_select_drop(m, CMD_WRITE, payload, FN_WRITE_PAYLOAD_SIZE, NULL);

    /* SELECT dropped before the last byte. The FIQ byte-receive poll has not yet seen the
       last byte (rx_done=0) and will find sel_drop_latch=1 when it next runs. Phase-2 will
       receive the error opcode and will copy nothing to flash. Check flash here, before
       mock_ps1_end_command runs the settle frames. No BIOS write to flash has occurred. */
    changed = count_flash_diff(flash_before, psemu_flash_data(m->ps));
    printf("  flash bytes changed after dispatch (regression, no last byte): %u (expected 0)\n",
           changed);
    assert(changed == 0u);

    /* SELECT is already low from the early drop. */
    m->settle_frames = DISPATCH_SETTLE_FRAMES;
    mock_ps1_end_command(m);

    mock_ps1_close(m);
    printf("test_dispatch_without_select_drop_does_not_write_flash OK\n");
}

int main(void) {
    const char *bios = bios_path();
    const char *app  = app_path();
    mock_ps1_t *probe;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* A quick open with no app first: confirms the BIOS is present and communication works.
       A missing BIOS gives NULL. A missing app gives NULL from mock_ps1_launch_app.
       Either condition skips this suite. */
    probe = mock_ps1_open(bios, NULL);
    if (!probe) {
        printf("sio_dispatch_test: no usable BIOS at %s, skipping\n", bios);
        printf("sio_dispatch_test: set PSEMU_TEST_BIOS to a %d-byte dump to run this suite\n",
               PSEMU_BIOS_SIZE);
        return SKIP_EXIT_CODE;
    }
    mock_ps1_close(probe);

    /* Verify the app file is present before running any test. */
    {
        FILE *f = fopen(app, "rb");
        if (!f) {
            printf("sio_dispatch_test: no app at %s, skipping\n", app);
            printf("sio_dispatch_test: set PSEMU_TEST_DISPATCH_APP to a dispatch app to run "
                   "this suite\n");
            return SKIP_EXIT_CODE;
        }
        fclose(f);
    }

    printf("sio_dispatch_test: BIOS %s, app %s\n", bios, app);

    test_dispatch_with_select_drop_writes_flash(bios, app);
    test_dispatch_without_select_drop_does_not_write_flash(bios, app);

    printf("sio_dispatch_test: all tests OK\n");
    return 0;
}
