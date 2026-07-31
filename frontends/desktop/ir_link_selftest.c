/* Verification tool for ir_link.c's real Windows named-pipe transport. Run it by hand.
   It is not part of the automated CTest suite.
   tests/ir_test.c covers core/src/ir.c's state machine instead, with no transport at all.

   This drives two ir_link_t endpoints, a host and a client, over a real named pipe inside one process.
   It writes an edge on one psemu_t's IR TX registers.
   It then confirms that the edge asserts INT_IRDA on a completely separate psemu_t, after the pipe relays it.
   Two real pokketstation.exe instances use that same path, without the two-process split.

   With arguments, it instead runs a full message transfer end to end over that same real pipe:

     ir_link_selftest <bios.bin> <app.mcs> <quicksave.dat> [frames]

   That mode is the one that matters for "does the desktop IR Link actually work". The single-edge test
   above proves only that one edge survives the pipe; it says nothing about whether a bit-banged message
   keeps its timing across a real transport, which is what an app actually needs. Both instances load the
   same save, so they are given distinct hardware ids and the verdict is whether each ends up holding the
   other's - a real IR message carries the sender's id, and neither side can learn it any other way.
   tools/ir_probe.c runs the same transfer with an in-process relay; this runs it through ir_link.c. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_link.h"
#include "psemu_internal.h"

#define IRDA_MODE (PSEMU_IR_BASE + 0x0u)
#define IRDA_DATA (PSEMU_IR_BASE + 0x4u)
#define TX_ACTIVE_MODE (IR_MODE_IFMODE | IR_MODE_BFLT)
#define RX_ACTIVE_MODE (IR_MODE_BFLT) /* receive, filter disabled for an immediate assert in this test */

#define SELFTEST_PIPE_NAME "\\\\.\\pipe\\pokketstation_ir_link_selftest"

static int pump_until(ir_link_t *a, psemu_t *ps_a, ir_link_t *b, psemu_t *ps_b, int max_iterations,
    int (*done)(ir_link_t *, ir_link_t *)) {
    int i;
    for (i = 0; i < max_iterations; i++) {
        ir_link_pump(a, ps_a);
        ir_link_pump(b, ps_b);
        if (done(a, b)) {
            return 1;
        }
        Sleep(1);
    }
    return 0;
}

static int both_connected(ir_link_t *a, ir_link_t *b) {
    return a->state == IR_LINK_CONNECTED && b->state == IR_LINK_CONNECTED;
}

#define QUICKSAVE_HEADER_SIZE 16 /* magic[4] + version + app_size + app_hash; see main.c */
#define FRAME_CYCLES 33000u

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
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

/* Searches an instance's low RAM for a 32-bit value. A message carries the sender's hardware id, so finding
   one instance's id in the other's RAM is proof that message content crossed the link. */
static int ram_holds_word(psemu_t *ps, uint32_t needle, uint32_t *out_addr) {
    uint32_t addr;
    for (addr = 0x300u; addr + 4u <= 0x800u; addr++) {
        uint32_t v = (uint32_t)psemu_bus_read8(&ps->bus, addr) |
                     ((uint32_t)psemu_bus_read8(&ps->bus, addr + 1u) << 8) |
                     ((uint32_t)psemu_bus_read8(&ps->bus, addr + 2u) << 16) |
                     ((uint32_t)psemu_bus_read8(&ps->bus, addr + 3u) << 24);
        if (v == needle) {
            *out_addr = addr;
            return 1;
        }
    }
    return 0;
}

/* Runs the real Chocobo World IR transfer between two instances, over the real named-pipe transport, pumping
   the link once per emulated frame exactly as the desktop frontend's main loop does. */
static int run_transfer(const char *bios_path, const char *app_path, const char *save_path, long frames) {
    size_t bios_size = 0, app_size = 0, save_size = 0;
    uint8_t *bios = read_file(bios_path, &bios_size);
    uint8_t *app = read_file(app_path, &app_size);
    uint8_t *save = read_file(save_path, &save_size);
    ir_link_t host_link, client_link;
    psemu_t *a, *b;
    long f;
    uint32_t id_a = 0xAA1111AAu, id_b = 0xBB2222BBu, addr = 0;
    int b_has_a, a_has_b;

    if (!bios || !app || !save) {
        fprintf(stderr, "failed to read one of the input files\n");
        return 1;
    }
    a = psemu_create();
    b = psemu_create();
    if (psemu_load_bios(a, bios, bios_size) != PSEMU_OK || psemu_load_bios(b, bios, bios_size) != PSEMU_OK ||
        psemu_load_content(a, app, app_size) != PSEMU_OK || psemu_load_content(b, app, app_size) != PSEMU_OK) {
        fprintf(stderr, "failed to load bios/app\n");
        return 1;
    }
    psemu_reset(a);
    psemu_reset(b);
    if (save_size > QUICKSAVE_HEADER_SIZE) {
        const uint8_t *state = save + QUICKSAVE_HEADER_SIZE;
        size_t state_size = save_size - QUICKSAVE_HEADER_SIZE;
        if (state_size != psemu_state_size(a)) {
            fprintf(stderr, "quicksave state is %zu bytes, this build expects %zu. Rebuild mismatch.\n", state_size,
                psemu_state_size(a));
            return 1;
        }
        if (psemu_load_state(a, state, state_size) != PSEMU_OK ||
            psemu_load_state(b, state, state_size) != PSEMU_OK) {
            fprintf(stderr, "psemu_load_state failed\n");
            return 1;
        }
    }
    psemu_set_hardware_id(a, id_a);
    psemu_set_hardware_id(b, id_b);

    ir_link_init(&host_link);
    ir_link_init(&client_link);
    if (!ir_link_host(&host_link, SELFTEST_PIPE_NAME)) {
        fprintf(stderr, "ir_link_host failed: %s\n", ir_link_status_text(&host_link));
        return 1;
    }
    ir_link_connect(&client_link, SELFTEST_PIPE_NAME);
    if (!pump_until(&host_link, a, &client_link, b, 2000, both_connected)) {
        fprintf(stderr, "never connected\n");
        return 1;
    }
    printf("connected: host=%s client=%s\n", ir_link_status_text(&host_link), ir_link_status_text(&client_link));
    printf("hardware ids: A=0x%08X B=0x%08X\n", psemu_get_hardware_id(a), psemu_get_hardware_id(b));

    for (f = 0; f < frames; f++) {
        /* The same button script tools/ir_probe.c drives the transfer with: both instances pick their side
           of the IR menu, then confirm. */
        uint32_t btn_a = 0, btn_b = 0;
        if (f >= 20 && f < 30) {
            btn_a = PSEMU_BUTTON_UP;
            btn_b = PSEMU_BUTTON_DOWN;
        } else if (f >= 60 && f < 70) {
            btn_a = PSEMU_BUTTON_FIRE;
            btn_b = PSEMU_BUTTON_FIRE;
        }
        psemu_set_buttons(a, btn_a);
        psemu_set_buttons(b, btn_b);
        psemu_run(a, FRAME_CYCLES);
        psemu_run(b, FRAME_CYCLES);
        /* Exactly where the desktop main loop pumps: once per frame, after psemu_run. */
        ir_link_pump(&host_link, a);
        ir_link_pump(&client_link, b);
    }

    b_has_a = ram_holds_word(b, id_a, &addr);
    if (b_has_a) {
        printf("B holds A's id 0x%08X at 0x%08X\n", id_a, addr);
    }
    a_has_b = ram_holds_word(a, id_b, &addr);
    if (a_has_b) {
        printf("A holds B's id 0x%08X at 0x%08X\n", id_b, addr);
    }
    printf("\nIR TRANSFER OVER REAL PIPE: A->B %s, B->A %s\n", b_has_a ? "VERIFIED" : "not seen",
        a_has_b ? "VERIFIED" : "not seen");

    ir_link_disconnect(&host_link);
    ir_link_disconnect(&client_link);
    psemu_destroy(a);
    psemu_destroy(b);
    free(bios);
    free(app);
    free(save);
    return (b_has_a && a_has_b) ? 0 : 1;
}

int main(int argc, char **argv) {
    ir_link_t host_link, client_link;
    psemu_t *ps_tx;
    psemu_t *ps_rx;

    if (argc >= 4) {
        return run_transfer(argv[1], argv[2], argv[3], argc >= 5 ? atol(argv[4]) : 400);
    }

    ps_tx = psemu_create();
    ps_rx = psemu_create();

    ir_link_init(&host_link);
    ir_link_init(&client_link);

    if (!ir_link_host(&host_link, SELFTEST_PIPE_NAME)) {
        fprintf(stderr, "ir_link_host failed: %s\n", ir_link_status_text(&host_link));
        return 1;
    }
    ir_link_connect(&client_link, SELFTEST_PIPE_NAME);

    if (!pump_until(&host_link, ps_tx, &client_link, ps_rx, 2000, both_connected)) {
        fprintf(stderr, "never connected: host=%s client=%s\n", ir_link_status_text(&host_link),
            ir_link_status_text(&client_link));
        return 1;
    }
    printf("connected: host=%s client=%s\n", ir_link_status_text(&host_link), ir_link_status_text(&client_link));

    /* Drain the connect-time HELLO handshake messages both sides queue in on_connected, so the assertions below
       see only the real edge this test produces next. */
    {
        int i;
        for (i = 0; i < 50; i++) {
            ir_link_pump(&host_link, ps_tx);
            ir_link_pump(&client_link, ps_rx);
            Sleep(1);
        }
    }

    ps_rx->intc.enable |= INT_IRDA;
    psemu_bus_write32(&ps_rx->bus, IRDA_MODE, RX_ACTIVE_MODE);
    psemu_bus_write32(&ps_tx->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&ps_tx->bus, IRDA_DATA, IR_DATA_LED); /* LED on: produces one TX edge in ps_tx */

    {
        int i;
        int seen = 0;
        /* ir_link.c schedules an incoming edge IR_LINK_PLAYOUT_DELAY_US into the receiver's own future.
           That is the jitter buffer. See ir_link.h.
           ps_rx's IR clock must therefore advance that far before the edge becomes due.
           Nothing else in this test calls psemu_run, so this advances the clock by hand.
           1056 cycles per loop iteration matches the 1ms this loop already sleeps each iteration.
           That figure is PSEMU_ASSUMED_CPU_HZ divided by 1000. */
        for (i = 0; i < 3000 && !seen; i++) {
            ir_link_pump(&host_link, ps_tx);
            ir_link_pump(&client_link, ps_rx);
            ir_tick(&ps_rx->ir, &ps_rx->intc, 1056u);
            seen = intc_irq_asserted(&ps_rx->intc);
            Sleep(1);
        }
        if (!seen) {
            fprintf(stderr, "INT_IRDA never asserted on the receiving instance. The transport did not relay the edge.\n");
            return 1;
        }
    }

    printf("PASS: an edge written on one psemu_t's IR TX registers relayed over the named pipe and asserted "
           "INT_IRDA on a separate psemu_t\n");

    ir_link_disconnect(&host_link);
    ir_link_disconnect(&client_link);
    psemu_destroy(ps_tx);
    psemu_destroy(ps_rx);
    return 0;
}
