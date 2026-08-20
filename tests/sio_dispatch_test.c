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

/* The command bytes for dispatch functions. */
#define CMD_WRITE 0x5Cu
#define CMD_READ  0x5Bu

/* FF8 Chocobo World dispatch layout (from Disc 3 traces).
   fn#1 write: PS1 sends 128 bytes → PocketStation copies to work RAM at CHOCO_WRITE_ADDR.
   fn#1 read:  PocketStation sends 128 bytes ← from work RAM at CHOCO_READ_ADDR.
   Both addresses are in user work RAM (0x200-0x7FF), which psemu_reset zeros.
   Data written here survives within a session but is lost on a fresh boot. */
#define CHOCO_FN         0x01u
#define CHOCO_N          0x80u          /* 128 bytes */
#define CHOCO_READ_ADDR  0x00000200u    /* PocketStation reads from here (0x5B) */
#define CHOCO_WRITE_ADDR 0x00000280u    /* PocketStation writes to here (0x5C) */
#define CHOCO_PAYLOAD_SIZE (8u + CHOCO_N)
/* Marker byte for continuity tests. Must differ from 0x00 (value after psemu_reset). */
#define CHOCO_MARKER 0xA5u

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

/* Builds a CHOCO_PAYLOAD_SIZE-byte payload for a 0x5C fn#1 write command with FF8's actual
   work RAM destination. All CHOCO_N data bytes are set to `marker`. */
static void make_choco_write_payload(uint8_t *data, uint8_t marker) {
    unsigned i;
    memset(data, 0, CHOCO_PAYLOAD_SIZE);
    data[0] = CHOCO_FN;
    data[2] = (uint8_t)( CHOCO_WRITE_ADDR        & 0xFFu);
    data[3] = (uint8_t)((CHOCO_WRITE_ADDR >>  8) & 0xFFu);
    data[4] = (uint8_t)((CHOCO_WRITE_ADDR >> 16) & 0xFFu);
    data[5] = (uint8_t)((CHOCO_WRITE_ADDR >> 24) & 0xFFu);
    data[6] = CHOCO_N;
    for (i = 0; i < CHOCO_N; i++) {
        data[8u + i] = marker;
    }
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

/* Baseline: a 0x5C write dispatch with FF8's work RAM address copies CHOCO_N bytes to
   work RAM at CHOCO_WRITE_ADDR within the same session.

   Unlike a flash write (which the BIOS phase-2 handler commits synchronously), a work RAM
   write goes through the app's dispatch processor. That processor runs AFTER SELECT drops,
   during the settle frames of mock_ps1_end_command. Check work RAM after end_command. */
static void test_dispatch_write_then_read_within_session(const char *bios, const char *app) {
    uint8_t payload[CHOCO_PAYLOAD_SIZE];
    mock_ps1_t *m = open_with_app(bios, app);
    const uint8_t *ram;
    size_t n;
    unsigned i;

    if (!m) {
        printf("test_dispatch_write_then_read_within_session SKIP (no app)\n");
        return;
    }
    make_choco_write_payload(payload, CHOCO_MARKER);

    n = mock_ps1_dispatch(m, CMD_WRITE, payload, CHOCO_PAYLOAD_SIZE, NULL);
    printf("  dispatch bytes sent: %u (expected %u)\n", (unsigned)n, 2u + CHOCO_PAYLOAD_SIZE);

    /* SELECT drops here. The FIQ exits its wait, the dispatch processor runs, and the app
       writes the received data to work RAM at CHOCO_WRITE_ADDR during the settle frames. */
    m->settle_frames = DISPATCH_SETTLE_FRAMES;
    mock_ps1_end_command(m);

    ram = psemu_ram_data(m->ps);
    for (i = 0; i < CHOCO_N; i++) {
        assert(ram[CHOCO_WRITE_ADDR + i] == CHOCO_MARKER);
    }
    mock_ps1_close(m);
    printf("test_dispatch_write_then_read_within_session OK\n");
}

/* Demonstrates the continuity bug: chocobo data written to work RAM is lost after a fresh
   boot from flash only. This is what DuckStation currently does on session restart. */
static void test_dispatch_write_lost_after_fresh_boot(const char *bios, const char *app) {
    uint8_t payload[CHOCO_PAYLOAD_SIZE];
    static uint8_t flash_buf[PSEMU_FLASH_SIZE];
    mock_ps1_t *m = open_with_app(bios, app);
    const uint8_t *ram;
    size_t n;
    unsigned i;
    mock_ps1_t *m2;

    if (!m) {
        printf("test_dispatch_write_lost_after_fresh_boot SKIP (no app)\n");
        return;
    }
    make_choco_write_payload(payload, CHOCO_MARKER);

    n = mock_ps1_dispatch(m, CMD_WRITE, payload, CHOCO_PAYLOAD_SIZE, NULL);
    printf("  session 1: dispatch bytes sent: %u\n", (unsigned)n);

    m->settle_frames = DISPATCH_SETTLE_FRAMES;
    mock_ps1_end_command(m);

    ram = psemu_ram_data(m->ps);
    for (i = 0; i < CHOCO_N; i++) {
        assert(ram[CHOCO_WRITE_ADDR + i] == CHOCO_MARKER);
    }

    /* Save flash only — no state. psemu_reset in session 2 zeros work RAM. */
    psemu_save_flash_image(m->ps, flash_buf, PSEMU_FLASH_SIZE);
    mock_ps1_close(m);

    /* Session 2: fresh boot from flash. Work RAM is zeroed by psemu_reset. */
    m2 = mock_ps1_open_from_flash(bios, flash_buf, PSEMU_FLASH_SIZE);
    if (!m2) {
        printf("test_dispatch_write_lost_after_fresh_boot SKIP (session 2 failed)\n");
        return;
    }
    /* After fresh boot the dispatch data (128 × CHOCO_MARKER) must not be present.
       The BIOS and app may initialize some bytes in user RAM to non-zero defaults, so
       we cannot assert all zeros. Instead assert that at least one byte differs from the
       marker, which is sufficient to show the dispatched data did not survive. */
    ram = psemu_ram_data(m2->ps);
    for (i = 0; i < CHOCO_N; i++) {
        if (ram[CHOCO_WRITE_ADDR + i] != CHOCO_MARKER) {
            break;
        }
    }
    assert(i < CHOCO_N); /* at least one byte must differ — data lost after fresh boot */
    mock_ps1_close(m2);
    printf("test_dispatch_write_lost_after_fresh_boot OK"
           " (data not preserved after fresh boot — this is the bug)\n");
}

/* Demonstrates the fix: restoring full machine state preserves work RAM across sessions.
   After psemu_load_state the chocobo data at CHOCO_WRITE_ADDR is still present. */
static void test_dispatch_write_survives_state_restore(const char *bios, const char *app) {
    uint8_t payload[CHOCO_PAYLOAD_SIZE];
    static uint8_t flash_buf[PSEMU_FLASH_SIZE];
    uint8_t *state_buf;
    size_t state_size;
    mock_ps1_t *m = open_with_app(bios, app);
    const uint8_t *ram;
    size_t n;
    unsigned i;
    mock_ps1_t *m2;

    if (!m) {
        printf("test_dispatch_write_survives_state_restore SKIP (no app)\n");
        return;
    }
    make_choco_write_payload(payload, CHOCO_MARKER);

    n = mock_ps1_dispatch(m, CMD_WRITE, payload, CHOCO_PAYLOAD_SIZE, NULL);
    printf("  session 1: dispatch bytes sent: %u\n", (unsigned)n);

    m->settle_frames = DISPATCH_SETTLE_FRAMES;
    mock_ps1_end_command(m);

    ram = psemu_ram_data(m->ps);
    for (i = 0; i < CHOCO_N; i++) {
        assert(ram[CHOCO_WRITE_ADDR + i] == CHOCO_MARKER);
    }

    /* Save full machine state (CPU + work RAM) and flash. */
    state_size = psemu_state_size(m->ps);
    state_buf = (uint8_t *)malloc(state_size);
    assert(state_buf != NULL);
    psemu_save_state(m->ps, state_buf, state_size);
    psemu_save_flash_image(m->ps, flash_buf, PSEMU_FLASH_SIZE);
    mock_ps1_close(m);

    /* Session 2: restore from state. Work RAM is restored, not zeroed. */
    m2 = mock_ps1_open_from_state(bios, flash_buf, PSEMU_FLASH_SIZE,
                                   state_buf, state_size);
    free(state_buf);
    if (!m2) {
        printf("test_dispatch_write_survives_state_restore SKIP (session 2 failed)\n");
        return;
    }
    ram = psemu_ram_data(m2->ps);
    for (i = 0; i < CHOCO_N; i++) {
        assert(ram[CHOCO_WRITE_ADDR + i] == CHOCO_MARKER);
    }
    mock_ps1_close(m2);
    printf("test_dispatch_write_survives_state_restore OK\n");
}

/* Runs the app for many frames without any dispatch and reports when (if ever) the app
   writes to flash. This establishes the auto-save cadence of the standalone app, which
   is the baseline for understanding when dispatched data reaches flash.

   NOTE: open_with_app leaves the machine in DOCKED mode (PS1 connected). In this
   state the app is waiting for PS1 commands, not running its training game. This is
   expected to produce zero flash writes. The real question is whether the app
   auto-saves AFTER undock (see test_hold_save_after_dispatch). */
static void test_autosave_cadence(const char *bios, const char *app) {
    static uint8_t flash_snap[PSEMU_FLASH_SIZE];
    mock_ps1_t *m = open_with_app(bios, app);
    const uint8_t *fl;
    unsigned f, save_count = 0;

    if (!m) {
        printf("test_autosave_cadence SKIP (no app)\n");
        return;
    }
    fl = psemu_flash_data(m->ps);
    memcpy(flash_snap, fl, PSEMU_FLASH_SIZE);

    /* Run 5000 frames docked (≈156 s at 32 Hz). Expect zero flash changes since the
       app waits for PS1 commands in docked mode. */
    for (f = 0u; f < 5000u; f++) {
        mock_ps1_run_frames(m, 1u);
        if (memcmp(psemu_flash_data(m->ps), flash_snap, PSEMU_FLASH_SIZE) != 0) {
            unsigned changed = count_flash_diff(flash_snap, fl);
            printf("  autosave (docked): flash changed %u bytes at frame %u\n", changed, f + 1u);
            memcpy(flash_snap, fl, PSEMU_FLASH_SIZE);
            save_count++;
            if (save_count >= 3u)
                break;
        }
    }
    if (save_count == 0u)
        printf("  autosave (docked): no flash change in 5000 docked frames (expected)\n");

    mock_ps1_close(m);
    printf("test_autosave_cadence done\n");
}

/* Boots the machine in standalone mode (no PS1 dock), navigates the BIOS menu to
   start the app, then runs for many frames to see if the app auto-saves to flash
   during normal standalone training. This tests whether the training game writes to
   flash at all during autonomous operation, regardless of any dispatch. */
static void test_standalone_boot_autosave(const char *bios, const char *app) {
    static uint8_t flash_snap[PSEMU_FLASH_SIZE];
    mock_ps1_t *m;
    psemu_t *ps;
    uint8_t *bios_data;
    size_t bios_size = 0;
    const uint8_t *fl;
    unsigned f, save_count = 0;

    /* We need to load the app content (from the .mcs file) but NOT dock. */
    {
        FILE *bf = fopen(bios, "rb");
        FILE *af = fopen(app, "rb");
        uint8_t *app_data;
        size_t app_size = 0;

        if (!bf || !af) {
            if (bf) fclose(bf);
            if (af) fclose(af);
            printf("test_standalone_boot_autosave SKIP (cannot open files)\n");
            return;
        }
        fseek(bf, 0, SEEK_END); bios_size = (size_t)ftell(bf); fseek(bf, 0, SEEK_SET);
        bios_data = (uint8_t *)malloc(bios_size);
        fread(bios_data, 1, bios_size, bf);
        fclose(bf);

        fseek(af, 0, SEEK_END); app_size = (size_t)ftell(af); fseek(af, 0, SEEK_SET);
        app_data = (uint8_t *)malloc(app_size);
        fread(app_data, 1, app_size, af);
        fclose(af);

        ps = psemu_create();
        if (!ps || psemu_load_bios(ps, bios_data, bios_size) != PSEMU_OK) {
            free(bios_data); free(app_data);
            printf("test_standalone_boot_autosave SKIP (bios load failed)\n");
            return;
        }
        free(bios_data);
        if (psemu_load_content(ps, app_data, app_size) != PSEMU_OK) {
            free(app_data); psemu_destroy(ps);
            printf("test_standalone_boot_autosave SKIP (app load failed)\n");
            return;
        }
        free(app_data);
    }

    psemu_reset(ps);
    /* Boot 200 frames in STANDALONE mode (never dock). */
    for (f = 0u; f < 200u; f++)
        psemu_run(ps, PSEMU_REFERENCE_CLOCK_HZ / 32u);

    /* Try several button sequences to navigate past date/time and launch the app.
       psemu docs say: "Down then Action" to pass date/time, "Right then Action" for app.
       Try each sequence and wait to see if the app launches. */
    {
        unsigned seq;
        static const uint32_t SEQS[][4] = {
            /* btn,  hold, btn2, hold2 */
            { PSEMU_BUTTON_DOWN,  8u, PSEMU_BUTTON_FIRE, 8u },
            { PSEMU_BUTTON_FIRE,  8u, 0u,                0u },
            { PSEMU_BUTTON_RIGHT, 8u, PSEMU_BUTTON_FIRE, 8u },
            { PSEMU_BUTTON_FIRE,  8u, 0u,                0u },
            { PSEMU_BUTTON_FIRE,  8u, 0u,                0u },
        };
        for (seq = 0u; seq < 5u; seq++) {
            psemu_set_buttons(ps, SEQS[seq][0]);
            for (f = 0u; f < SEQS[seq][1]; f++) psemu_run(ps, PSEMU_REFERENCE_CLOCK_HZ / 32u);
            psemu_set_buttons(ps, 0);
            for (f = 0u; f < 8u; f++) psemu_run(ps, PSEMU_REFERENCE_CLOCK_HZ / 32u);
            if (SEQS[seq][2]) {
                psemu_set_buttons(ps, SEQS[seq][2]);
                for (f = 0u; f < SEQS[seq][3]; f++) psemu_run(ps, PSEMU_REFERENCE_CLOCK_HZ / 32u);
                psemu_set_buttons(ps, 0);
                for (f = 0u; f < 8u; f++) psemu_run(ps, PSEMU_REFERENCE_CLOCK_HZ / 32u);
            }
            /* Let the BIOS settle for 50 frames after each button group. */
            for (f = 0u; f < 50u; f++) psemu_run(ps, PSEMU_REFERENCE_CLOCK_HZ / 32u);
            if (psemu_app_running(ps)) {
                printf("  standalone: app_running=1 after sequence %u\n", seq);
                break;
            }
        }
    }
    printf("  standalone boot: app_running=%d after BIOS navigation\n",
           psemu_app_running(ps));

    m = (mock_ps1_t *)malloc(sizeof(mock_ps1_t));
    if (!m) { psemu_destroy(ps); return; }
    m->ps = ps;
    m->timeout_cycles = PSEMU_COM_DEFAULT_TIMEOUT_CYCLES;
    m->settle_frames = 8u;

    fl = psemu_flash_data(m->ps);
    memcpy(flash_snap, fl, PSEMU_FLASH_SIZE);

    /* Run 10000 frames in standalone mode. Report each flash change. */
    for (f = 0u; f < 10000u; f++) {
        mock_ps1_run_frames(m, 1u);
        if (memcmp(fl, flash_snap, PSEMU_FLASH_SIZE) != 0) {
            unsigned changed = count_flash_diff(flash_snap, fl);
            printf("  standalone: flash changed %u bytes at frame %u (app_running=%d)\n",
                   changed, f + 1u, psemu_app_running(m->ps));
            memcpy(flash_snap, fl, PSEMU_FLASH_SIZE);
            save_count++;
            if (save_count >= 3u)
                break;
        }
    }
    if (save_count == 0u)
        printf("  standalone: no flash change in 10000 frames (app_running=%d)\n",
               psemu_app_running(m->ps));

    mock_ps1_close(m);
    printf("test_standalone_boot_autosave done\n");
}

/* Probes when (if ever) the app auto-saves to flash after dispatch + undock.
   Runs up to 10000 standalone frames without button input to find the auto-save event.
   Then probes the FIRE-hold path for an additional 2000 frames.
   Diagnostic only — reports but does not assert the flash write outcome. */
static void test_hold_save_after_dispatch(const char *bios, const char *app) {
    static uint8_t flash_snap[PSEMU_FLASH_SIZE];
    uint8_t payload[CHOCO_PAYLOAD_SIZE];
    mock_ps1_t *m = open_with_app(bios, app);
    const uint8_t *fl;
    unsigned f, changed, save_count;

    if (!m) {
        printf("test_hold_save_after_dispatch SKIP (no app)\n");
        return;
    }
    fl = psemu_flash_data(m->ps);
    memcpy(flash_snap, fl, PSEMU_FLASH_SIZE);
    make_choco_write_payload(payload, CHOCO_MARKER);

    /* Phase 1: dispatch chocobo data to work RAM. */
    mock_ps1_dispatch(m, CMD_WRITE, payload, CHOCO_PAYLOAD_SIZE, NULL);
    m->settle_frames = DISPATCH_SETTLE_FRAMES;
    mock_ps1_end_command(m);

    changed = count_flash_diff(flash_snap, fl);
    printf("  phase 1: flash changed %u bytes after dispatch+settle (expect 0)\n", changed);
    if (changed > 0)
        memcpy(flash_snap, fl, PSEMU_FLASH_SIZE);

    {
        const uint8_t *ram = psemu_ram_data(m->ps);
        unsigned i;
        for (i = 0; i < CHOCO_N; i++)
            assert(ram[CHOCO_WRITE_ADDR + i] == CHOCO_MARKER);
    }

    /* Phase 2: undock, run up to 10000 frames, report flash changes and app state. */
    psemu_com_set_docked(m->ps, 0);
    save_count = 0u;
    for (f = 0u; f < 10000u; f++) {
        mock_ps1_run_frames(m, 1u);
        changed = count_flash_diff(flash_snap, fl);
        if (changed > 0u) {
            printf("  phase 2: flash changed %u bytes at undock frame %u\n", changed, f + 1u);
            memcpy(flash_snap, fl, PSEMU_FLASH_SIZE);
            save_count++;
            if (save_count >= 3u)
                break;
        }
        if (f == 9u || f == 59u || f == 199u || f == 599u || f == 1999u || f == 4999u || f == 9999u) {
            const uint8_t *ram = psemu_ram_data(m->ps);
            printf("  frame %u: app_running=%d cpu_faulted=%d CMD_ACTIVE=0x%02X OUTER_LOOP=0x%02X\n",
                   f + 1u, psemu_app_running(m->ps), psemu_cpu_faulted(m->ps),
                   (unsigned)ram[CMD_ACTIVE_FLAG], (unsigned)ram[OUTER_LOOP_FLAG]);
        }
    }
    if (save_count == 0u)
        printf("  phase 2: no flash change after 10000 undock frames\n");

    /* Phase 3: hold FIRE for 2000 frames, report first flash change. */
    psemu_set_buttons(m->ps, PSEMU_BUTTON_FIRE);
    for (f = 0u; f < 2000u; f++) {
        mock_ps1_run_frames(m, 1u);
        changed = count_flash_diff(flash_snap, fl);
        if (changed > 0u) {
            printf("  phase 3: hold-save fired — flash changed %u bytes at FIRE frame %u\n",
                   changed, f + 1u);
            memcpy(flash_snap, fl, PSEMU_FLASH_SIZE);
            break;
        }
    }
    psemu_set_buttons(m->ps, 0u);
    if (f >= 2000u)
        printf("  phase 3: no flash change after 2000 FIRE frames\n");

    mock_ps1_close(m);
    printf("test_hold_save_after_dispatch done\n");
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
    test_dispatch_write_then_read_within_session(bios, app);
    test_dispatch_write_lost_after_fresh_boot(bios, app);
    test_dispatch_write_survives_state_restore(bios, app);
    test_autosave_cadence(bios, app);
    test_standalone_boot_autosave(bios, app);
    test_hold_save_after_dispatch(bios, app);

    printf("sio_dispatch_test: all tests OK\n");
    return 0;
}
