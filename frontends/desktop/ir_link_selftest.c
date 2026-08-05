/* A verification tool for the real Windows named-pipe transport in ir_link.c. Start it manually.
   It is not part of the automatic CTest suite.
   tests/ir_test.c tests the state machine in core/src/ir.c, with no transport.

   This tool operates two ir_link_t endpoints, a host and a client, on a real named pipe in one process.
   It writes an edge to the IR TX registers of one psemu_t instance.
   It then confirms that the pipe relays the edge, and that the edge asserts INT_IRDA on a separate
   psemu_t instance.
   Two real pokketstation.exe instances use that same path, but with two processes.

   With arguments, this tool instead does a full message transfer on that same real pipe:

     ir_link_selftest <bios.bin> <app.mcs> <quicksave.dat> [frames]

   That mode answers the question "does the desktop IR link operate correctly?". The single-edge test
   above shows only that one edge crosses the pipe. It gives no data about the timing of a bit-banged
   message on a real transport, which is what an app needs. Both instances load the same save. Thus this
   tool gives them different hardware IDs, and the result is whether each instance then holds the ID of
   the other. A real IR message contains the ID of the sender, and no other method can give that ID to
   the receiver.
   tools/ir_probe.c does the same transfer with an in-process relay. This tool does it through
   ir_link.c.

   The last mode uses one side for each process. It is the only mode that reproduces the operation of two
   real windows:

     ir_link_selftest --host   <bios.bin> <app.mcs> <quicksave.dat> [frames] [frames_before_connecting]
     ir_link_selftest --client <bios.bin> <app.mcs> <quicksave.dat> [frames] [frames_before_connecting]

   Start both, and start the host first. Each process uses the real pacing of the desktop main loop:
   psemu_run, and then a sleep of approximately 31ms. Thus emulated time falls behind wall time, exactly
   as it does in the frontend.

   The single-process modes cannot show one class of fault, and they did not show it. When both endpoints
   use one scheduler, their clocks drift by the same quantity, thus the drift cancels. With two
   processes, the drift does not cancel. A transfer that passed each single-process test still failed in
   one direction, and each arriving edge was scheduled into the past of the receiver.
   frames_before_connecting reproduces the real order of operations: both units are already on the IR
   screen of the app, with very different quantities of emulated time, before the link exists. */
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

/* Searches the low RAM of an instance for a 32-bit value. A message contains the hardware ID of the
   sender. Thus the ID of one instance in the RAM of the other instance is proof that message content
   crossed the link. */
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

/* Does a real IR transfer between two instances, on the real named-pipe transport. It pumps the link
   one time for each emulated frame, exactly as the main loop of the desktop frontend does. */
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
        /* The same button sequence that tools/ir_probe.c uses for the transfer: each instance selects
           its side of the IR menu, and then confirms the selection. */
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

/* One side of a true two-process run.

   run_transfer above operates both endpoints in one process. That method conceals each condition that
   only two real processes make: independent frame pacing, independent scheduling by the operating
   system, and two clocks that drift apart because each process renders at its own speed. This mode
   instead reproduces the true timing of the desktop main loop: psemu_run(33000), and then a sleep of
   approximately 31ms. Thus emulated time advances more slowly than wall time, exactly as in the real
   frontend. Start this mode two times, one time for each role. */
static int run_role(int is_host, const char *bios_path, const char *app_path, const char *save_path,
    long frames, long pre_frames) {
    size_t bios_size = 0, app_size = 0, save_size = 0;
    uint8_t *bios = read_file(bios_path, &bios_size);
    uint8_t *app = read_file(app_path, &app_size);
    uint8_t *save = read_file(save_path, &save_size);
    const char *tag = is_host ? "host" : "client";
    ir_link_t link;
    psemu_t *ps;
    long f;
    uint32_t own_id = is_host ? 0xAA1111AAu : 0xBB2222BBu;
    uint32_t peer_id = is_host ? 0xBB2222BBu : 0xAA1111AAu;
    uint32_t addr = 0;
    int holds_peer;

    if (!bios || !app || !save) {
        fprintf(stderr, "[%s] failed to read input files\n", tag);
        return 2;
    }
    ps = psemu_create();
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK || psemu_load_content(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr, "[%s] failed to load bios/app\n", tag);
        return 2;
    }
    psemu_reset(ps);
    if (save_size > QUICKSAVE_HEADER_SIZE) {
        const uint8_t *state = save + QUICKSAVE_HEADER_SIZE;
        size_t state_size = save_size - QUICKSAVE_HEADER_SIZE;
        if (state_size != psemu_state_size(ps)) {
            fprintf(stderr, "[%s] quicksave is %zu bytes, this build expects %zu\n", tag, state_size,
                psemu_state_size(ps));
            return 2;
        }
        if (psemu_load_state(ps, state, state_size) != PSEMU_OK) {
            fprintf(stderr, "[%s] psemu_load_state failed\n", tag);
            return 2;
        }
    }
    psemu_set_hardware_id(ps, own_id);

    /* In the real procedure, a user connects the two units only after both units are on the IR screen
       of the app for some time. To get to that screen, a user must load a save and move through a
       menu. Thus the two instances have very different quantities of emulated time on their clocks
       when the link starts, and they got to that screen at different wall-clock times. To execute
       frames before the connection reproduces this condition. To connect immediately at startup, which
       this test did at first, does not reproduce it. */
    {
        long p;
        for (p = 0; p < pre_frames; p++) {
            psemu_set_buttons(ps, 0);
            psemu_run(ps, FRAME_CYCLES);
            Sleep(31);
        }
        if (pre_frames > 0) {
            printf("[%s] ran %ld frames before connecting, core clock now %lluus\n", tag, pre_frames,
                (unsigned long long)psemu_ir_get_clock_us(ps));
            fflush(stdout);
        }
    }

    ir_link_init(&link);
    if (is_host) {
        if (!ir_link_host(&link, SELFTEST_PIPE_NAME)) {
            fprintf(stderr, "[%s] ir_link_host failed: %s\n", tag, ir_link_status_text(&link));
            return 2;
        }
    } else {
        ir_link_connect(&link, SELFTEST_PIPE_NAME);
    }
    {
        int i;
        for (i = 0; i < 4000 && link.state != IR_LINK_CONNECTED; i++) {
            ir_link_pump(&link, ps);
            Sleep(1);
        }
    }
    if (link.state != IR_LINK_CONNECTED) {
        fprintf(stderr, "[%s] never connected: %s\n", tag, ir_link_status_text(&link));
        return 2;
    }
    printf("[%s] connected, own id 0x%08X, waiting to see peer id 0x%08X\n", tag, own_id, peer_id);
    fflush(stdout);

    for (f = 0; f < frames; f++) {
        uint32_t btn = 0;
        if (f >= 20 && f < 30) {
            btn = is_host ? PSEMU_BUTTON_UP : PSEMU_BUTTON_DOWN;
        } else if (f >= 60 && f < 70) {
            btn = PSEMU_BUTTON_FIRE;
        }
        psemu_set_buttons(ps, btn);
        psemu_run(ps, FRAME_CYCLES);
        ir_link_pump(&link, ps);
        Sleep(31); /* what the desktop loop does after rendering each frame */
    }

    holds_peer = ram_holds_word(ps, peer_id, &addr);
    if (holds_peer) {
        printf("[%s] holds peer id 0x%08X at 0x%08X\n", tag, peer_id, addr);
    }
    printf("[%s] edges sent=%lu received=%lu dropped=%lu\n", tag, link.edges_sent, link.edges_received,
        link.dropped_tx);
    printf("[%s] playout lead: min=%lldus max=%lldus late=%lu\n", tag, (long long)link.min_lead_us,
        (long long)link.max_lead_us, link.late_edges);
    printf("[%s] RESULT: peer id %s\n", tag, holds_peer ? "RECEIVED" : "NOT RECEIVED");
    fflush(stdout);

    ir_link_disconnect(&link);
    psemu_destroy(ps);
    free(bios);
    free(app);
    free(save);
    return holds_peer ? 0 : 1;
}

int main(int argc, char **argv) {
    ir_link_t host_link, client_link;
    psemu_t *ps_tx;
    psemu_t *ps_rx;

    if (argc >= 5 && (strcmp(argv[1], "--host") == 0 || strcmp(argv[1], "--client") == 0)) {
        return run_role(strcmp(argv[1], "--host") == 0, argv[2], argv[3], argv[4],
            argc >= 6 ? atol(argv[5]) : 400, argc >= 7 ? atol(argv[6]) : 0);
    }
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

    /* Remove the HELLO handshake messages that both sides put into the queue in on_connected. Thus the
       tests below see only the real edge that this test makes next. */
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
        /* ir_link.c schedules an incoming edge IR_LINK_PLAYOUT_DELAY_US into the future of the
           receiver. This is the jitter buffer. See ir_link.h.
           Thus the IR clock of ps_rx must advance that quantity before the edge is due.
           No other code in this test calls psemu_run, thus this loop advances the clock directly.
           The value of 1056 cycles for each iteration agrees with the 1ms sleep in each iteration of
           this loop. That value is PSEMU_ASSUMED_CPU_HZ divided by 1000. */
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
