/* A verification tool for the real transports in ir_transport_pipe.c and ir_transport_tcp.c, and for the
   timing above them in ir_link.c. Start it manually.
   It is not part of the automatic CTest suite.
   tests/ir_test.c tests the state machine in core/src/ir.c, with no transport.

   A --tcp first argument moves whichever mode follows it onto the TCP transport, on the loopback
   interface. Without it, each mode uses the local named pipe. The modes are the same for both, thus one
   set of tests covers both transports:

     ir_link_selftest [--tcp] ...

   Three modes need no BIOS and no save state, thus they run at any time:

     ir_link_selftest --bytes [total]
     ir_link_selftest --burst [edges] [injected clock offset in us]
     ir_link_selftest --sync  [seconds] [injected clock offset in us]

   --bytes tests the transport alone. Both endpoints send the same number of bytes at the same time, in
   uneven chunks, and each byte must arrive one time and in order. This mode covers the read buffer, the
   partial-write path, and a concurrent read and write on one handle.
   --burst tests ir_link.c with a group of edges near the size of a real IR message. The single-edge
   mode moves one edge, thus it cannot show a lost message or a message in the incorrect order.
   --sync tests the measurement of the offset between the two wall clocks.

   Both --burst and --sync take an injected clock offset, which they give to the client side through
   test_wall_offset_us (see ir_link.h). Two processes on one machine always share a wall clock. Thus
   without that hook, a local test cannot separate "the code measured an offset of 0 correctly" from "the
   code has no measurement": both give the same result. With an injected offset of one hour, a build with
   no measurement fails by one hour. --sync confirms that the estimator reports the injected value, and
   --burst confirms that the placement of each edge uses it.

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
/* The loopback interface. Thus a TCP run of this tool needs no network and no second machine, and it
   still uses the real socket path: a real connect, a real accept, and a real byte stream. */
#define SELFTEST_TCP_ADDRESS "127.0.0.1:27412"

/* A --tcp first argument moves each mode of this tool onto the TCP transport. Without it, each mode uses
   the local named pipe. The modes are the same, thus one set of tests covers both transports. */
static int g_use_tcp = 0;

static const char *selftest_address(void) {
    return g_use_tcp ? SELFTEST_TCP_ADDRESS : SELFTEST_PIPE_NAME;
}

static void selftest_link_init(ir_link_t *link) {
    ir_link_init(link, g_use_tcp ? IR_LINK_TRANSPORT_TCP : IR_LINK_TRANSPORT_PIPE);
}

static void selftest_transport_init(ir_transport_t *t) {
    if (g_use_tcp) {
        ir_transport_init_tcp(t);
    } else {
        ir_transport_init_pipe(t);
    }
}

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

    selftest_link_init(&host_link);
    selftest_link_init(&client_link);
    if (!ir_link_host(&host_link, selftest_address())) {
        fprintf(stderr, "ir_link_host failed: %s\n", ir_link_status_text(&host_link));
        return 1;
    }
    ir_link_connect(&client_link, selftest_address());
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
    printf("\nIR TRANSFER OVER THE REAL TRANSPORT: A->B %s, B->A %s\n", b_has_a ? "VERIFIED" : "not seen",
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

    selftest_link_init(&link);
    if (is_host) {
        if (!ir_link_host(&link, selftest_address())) {
            fprintf(stderr, "[%s] ir_link_host failed: %s\n", tag, ir_link_status_text(&link));
            return 2;
        }
    } else {
        ir_link_connect(&link, selftest_address());
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

/* The limit on the error of one offset sample, in microseconds.

   A measurement gives this limit, and the pump interval is the mechanism. ir_link_pump runs one time for
   each frame, at intervals of approximately 31ms. A message that arrives between two pump calls waits
   until the next call. Thus one direction of a round trip holds up to one full pump interval of delay
   that the other direction does not hold, and one half of that difference is error.
   A run of this mode reports the legs directly. The side that pumps second reads the PING of its peer
   after approximately 15us, and the answer then waits for the next pump call of the initiator:
   fwd=4us with rev=42796us is one measured sample. The turnaround of the peer (t2 - t1) measures 1us to
   3us, thus the four-timestamp form has nothing to remove in that condition. It still earns its place,
   because the turnaround is not small while a queue with a full IR message drains.
   The largest error over a run of 8 seconds measured 22879us. This limit is near three times that value.
   Thus it reports a mechanism that stopped working, and it does not report normal variation.

   The error does not need to be smaller. It is one constant for each latch, thus each edge of one message
   gets the same shift and only the arrival of the full group moves. IR_LINK_PLAYOUT_DELAY_US is 250000us,
   which is more than three times this limit. */
#define SYNC_OFFSET_ERROR_LIMIT_US 60000

/* The clock measurement of ir_link.c, on the local pipe.

   Two processes on one machine always read the same wall clock. Thus a local test has no true offset to
   find, and "the code measured 0 correctly" and "the code has no measurement at all" give the same
   result. This mode uses test_wall_offset_us (see ir_link.h) to put one link on a clock that is a known
   distance away. The estimator must then report that distance, with the sign that belongs to each side.
   A build that ignores the clock of its peer reports 0 and fails.

   This mode also confirms that a link reaches IR_LINK_CONNECTED, which needs one completed round trip,
   and that it keeps collecting samples after that. */
static int run_sync(long seconds, int64_t injected_offset_us) {
    ir_link_t host_link, client_link;
    psemu_t *ps_a = psemu_create();
    psemu_t *ps_b = psemu_create();
    int64_t host_worst = 0, client_worst = 0;
    int64_t host_max_rtt = 0, client_max_rtt = 0;
    unsigned long host_samples = 0, client_samples = 0;
    int64_t host_last = 0, client_last = 0;
    int64_t host_prev_rtt = 0, client_prev_rtt = 0;
    long frames = seconds * 32; /* the desktop loop renders approximately 32 frames for each second */
    long f;
    int ok = 1;

    selftest_link_init(&host_link);
    selftest_link_init(&client_link);
    /* The client operates on a clock that is injected_offset_us ahead of the clock of the host. Thus the
       host must measure +injected_offset_us for its peer, and the client must measure the negative of
       that value. */
    client_link.test_wall_offset_us = injected_offset_us;
    if (!ir_link_host(&host_link, selftest_address())) {
        fprintf(stderr, "ir_link_host failed: %s\n", ir_link_status_text(&host_link));
        return 1;
    }
    ir_link_connect(&client_link, selftest_address());

    for (f = 0; f < frames; f++) {
        ir_link_pump(&host_link, ps_a);
        ir_link_pump(&client_link, ps_b);
        if (host_link.state == IR_LINK_ERROR || client_link.state == IR_LINK_ERROR) {
            fprintf(stderr, "link fault: host=%s client=%s\n", ir_link_status_text(&host_link),
                ir_link_status_text(&client_link));
            return 1;
        }
        /* Count each new sample, and keep the largest error and the largest round trip. The error of a
           sample is its distance from the offset that this test injected. */
        if (host_link.last_rtt_us != 0 &&
            (host_link.last_offset_us != host_last || host_link.last_rtt_us != host_prev_rtt)) {
            int64_t error = host_link.last_offset_us - injected_offset_us;
            host_last = host_link.last_offset_us;
            host_prev_rtt = host_link.last_rtt_us;
            host_samples++;
            if (error < 0) {
                error = -error;
            }
            if (error > host_worst) {
                host_worst = error;
            }
            if (host_link.last_rtt_us > host_max_rtt) {
                host_max_rtt = host_link.last_rtt_us;
            }
        }
        if (client_link.last_rtt_us != 0 &&
            (client_link.last_offset_us != client_last || client_link.last_rtt_us != client_prev_rtt)) {
            int64_t error = client_link.last_offset_us + injected_offset_us;
            client_last = client_link.last_offset_us;
            client_prev_rtt = client_link.last_rtt_us;
            client_samples++;
            if (error < 0) {
                error = -error;
            }
            if (error > client_worst) {
                client_worst = error;
            }
            if (client_link.last_rtt_us > client_max_rtt) {
                client_max_rtt = client_link.last_rtt_us;
            }
        }
        Sleep(31); /* what the desktop loop does after rendering each frame */
    }

    printf("injected offset: the clock of the client is %lldus ahead of the clock of the host\n",
        (long long)injected_offset_us);
    printf("host  state=%s samples=%lu last offset=%lldus worst error=%lldus max rtt=%lldus\n",
        ir_link_status_text(&host_link), host_samples, (long long)host_link.last_offset_us, (long long)host_worst,
        (long long)host_max_rtt);
    printf("client state=%s samples=%lu last offset=%lldus worst error=%lldus max rtt=%lldus\n",
        ir_link_status_text(&client_link), client_samples, (long long)client_link.last_offset_us,
        (long long)client_worst, (long long)client_max_rtt);

    if (host_link.state != IR_LINK_CONNECTED || client_link.state != IR_LINK_CONNECTED) {
        fprintf(stderr, "FAIL: a link did not leave IR_LINK_SYNCING, thus no round trip completed\n");
        ok = 0;
    }
    if (host_samples < 2 || client_samples < 2) {
        fprintf(stderr, "FAIL: the measurement stopped after the first sample\n");
        ok = 0;
    }
    if (host_worst > SYNC_OFFSET_ERROR_LIMIT_US || client_worst > SYNC_OFFSET_ERROR_LIMIT_US) {
        fprintf(stderr, "FAIL: the estimator did not recover the injected offset within %dus\n",
            (int)SYNC_OFFSET_ERROR_LIMIT_US);
        ok = 0;
    }

    ir_link_disconnect(&host_link);
    ir_link_disconnect(&client_link);
    psemu_destroy(ps_a);
    psemu_destroy(ps_b);
    if (!ok) {
        return 1;
    }
    printf("PASS: the estimator recovered a clock offset of %lldus, with the sign of each side\n",
        (long long)injected_offset_us);
    return 0;
}

/* The byte pattern of the fidelity mode below. It is a function of the index only, thus neither side
   has to keep a copy of the data that it sent. */
static uint8_t stream_byte(unsigned long index) {
    return (uint8_t)(index * 31u + 7u);
}

/* Offers one chunk to the transport, and then takes each byte that the transport has ready.
   The chunk sizes are uneven, and some of them are larger than the buffers of the transport. Thus this
   loop makes partial sends and split reads, which a test with one fixed size of 16 bytes never makes.
   A byte that arrives out of order, two times, or not at all increases *mismatches or leaves *received
   below the total. */
static void pump_byte_stream(ir_transport_t *t, unsigned long total, unsigned long *sent,
    unsigned long *received, unsigned long *mismatches, unsigned *chunk_index) {
    static const size_t chunk_sizes[] = {1, 7, 13, 16, 17, 255, 1000, 4095, 4096, 5000};
    static const unsigned chunk_size_count = (unsigned)(sizeof(chunk_sizes) / sizeof(chunk_sizes[0]));
    uint8_t buf[6000];
    if (*sent < total) {
        size_t want = chunk_sizes[*chunk_index % chunk_size_count];
        size_t i, took;
        if (want > total - *sent) {
            want = (size_t)(total - *sent);
        }
        for (i = 0; i < want; i++) {
            buf[i] = stream_byte(*sent + i);
        }
        took = ir_transport_send(t, buf, want);
        if (took == want) {
            (*chunk_index)++; /* a full chunk went, thus the next call uses the next size */
        }
        *sent += (unsigned long)took;
    }
    for (;;) {
        size_t got = ir_transport_recv(t, buf, sizeof(buf));
        size_t i;
        if (got == 0) {
            break;
        }
        for (i = 0; i < got; i++) {
            if (buf[i] != stream_byte(*received + i)) {
                (*mismatches)++;
            }
        }
        *received += (unsigned long)got;
    }
}

/* Byte fidelity of the transport, with no psemu_t and no wire message.
   ir_transport.h gives a byte stream and keeps no message boundary. This mode confirms that property
   directly: each side sends the same number of bytes, both directions operate at the same time on one
   handle, and each byte must arrive one time and in order.
   This mode covers the read buffer, the partial-write path, and the concurrent read and write on one
   handle. The single-edge mode above covers only 16 bytes in one direction. */
static int run_byte_fidelity(unsigned long total) {
    ir_transport_t host, client;
    unsigned long host_sent = 0, host_received = 0, host_bad = 0;
    unsigned long client_sent = 0, client_received = 0, client_bad = 0;
    unsigned host_chunk = 0, client_chunk = 0;
    int i;

    selftest_transport_init(&host);
    selftest_transport_init(&client);
    if (!ir_transport_host(&host, selftest_address())) {
        fprintf(stderr, "ir_transport_host failed: %s\n", host.error);
        return 1;
    }
    ir_transport_connect(&client, selftest_address());
    for (i = 0; i < 2000; i++) {
        ir_transport_poll(&host);
        ir_transport_poll(&client);
        if (host.state == IR_TRANSPORT_CONNECTED && client.state == IR_TRANSPORT_CONNECTED) {
            break;
        }
        Sleep(1);
    }
    if (host.state != IR_TRANSPORT_CONNECTED || client.state != IR_TRANSPORT_CONNECTED) {
        fprintf(stderr, "never connected: host=%d client=%d\n", (int)host.state, (int)client.state);
        return 1;
    }

    for (i = 0; i < 2000000; i++) {
        pump_byte_stream(&host, total, &host_sent, &host_received, &host_bad, &host_chunk);
        pump_byte_stream(&client, total, &client_sent, &client_received, &client_bad, &client_chunk);
        if (host.state != IR_TRANSPORT_CONNECTED || client.state != IR_TRANSPORT_CONNECTED) {
            fprintf(stderr, "transport fault: host=%s client=%s\n", host.error, client.error);
            return 1;
        }
        if (host_received >= total && client_received >= total) {
            break;
        }
    }

    printf("host  sent=%lu received=%lu mismatches=%lu\n", host_sent, host_received, host_bad);
    printf("client sent=%lu received=%lu mismatches=%lu\n", client_sent, client_received, client_bad);
    ir_transport_close(&host);
    ir_transport_close(&client);
    if (host_received != total || client_received != total || host_bad != 0 || client_bad != 0) {
        fprintf(stderr, "FAIL: the transport did not relay every byte one time and in order\n");
        return 1;
    }
    printf("PASS: %lu bytes crossed the transport in each direction, in order, with no loss\n", total);
    return 0;
}

/* A full group of edges across the link, with no BIOS and no save state.
   One real IR message makes approximately 658 edges. The single-edge mode above moves one edge, thus it
   cannot show a fault that needs a full group: a lost message, a message in the incorrect order, or a
   frame boundary that moves.
   A frame boundary that moves needs no separate test here. Each wire message holds IR_WIRE_MAGIC, and
   ir_link.c tests that value for each message that it assembles. Thus a boundary that moves by one byte
   puts the link into IR_LINK_ERROR immediately, and this mode reports that state.
   The gaps between the edges are uneven, and the count is near the count of a real message. */
#define BURST_EDGES_PER_FRAME 65u
static int run_burst(unsigned long edge_count, int64_t injected_offset_us) {
    ir_link_t host_link, client_link;
    psemu_t *ps_tx = psemu_create();
    psemu_t *ps_rx = psemu_create();
    unsigned long i;
    uint64_t span_cycles = 0;
    int64_t span_us = 0;
    int ok = 1;

    /* Advance the IR clock of the receiver before the link exists.
       wall_to_local_us holds a result of less than zero at zero. With an IR clock of 0 on the receiver,
       each edge of this group converts to a time before that clock and clamps to the same value. Every
       lead then reads exactly IR_LINK_PLAYOUT_DELAY_US, whatever the offset of the peer clock is, and the
       test of the lead below shows nothing. A clock that already holds 2 seconds keeps each conversion
       above the clamp. Thus a wrong offset moves the lead, and the test can see it.
       The queue of the receiver is empty at this point, thus these calls release no edge. */
    {
        int t;
        for (t = 0; t < 2000; t++) {
            ir_tick(&ps_rx->ir, &ps_rx->intc, 1056u); /* PSEMU_ASSUMED_CPU_HZ / 1000, thus 1ms for each call */
        }
    }

    selftest_link_init(&host_link);
    selftest_link_init(&client_link);
    /* The receiving side operates on a clock that is this far from the clock of the sender. A count of the
       edges and an order of the levels cannot show whether the code applies the measured offset: each
       edge still reaches the queue with the incorrect offset. The test of the lead below is the check that
       shows it. See test_wall_offset_us in ir_link.h. */
    client_link.test_wall_offset_us = injected_offset_us;
    if (!ir_link_host(&host_link, selftest_address())) {
        fprintf(stderr, "ir_link_host failed: %s\n", ir_link_status_text(&host_link));
        return 1;
    }
    ir_link_connect(&client_link, selftest_address());
    if (!pump_until(&host_link, ps_tx, &client_link, ps_rx, 2000, both_connected)) {
        fprintf(stderr, "never connected: host=%s client=%s\n", ir_link_status_text(&host_link),
            ir_link_status_text(&client_link));
        return 1;
    }

    psemu_bus_write32(&ps_tx->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&ps_rx->bus, IRDA_MODE, RX_ACTIVE_MODE);

    /* Make the group across several pump calls, in the manner of the frontend. A real sender makes
       approximately 65 edges for each frame, and it relays them at the end of that frame.
       To make the full group before any pump call gives each edge an age of the full length of the group.
       That length is 265ms at these gaps, and the playout buffer plans 250000us. Thus the earliest edges
       arrive late. That result is a property of such a test and not of the transport, and a measurement
       shows it: 57 of 700 edges late, with the earliest lead at -21409us.
       The gaps are 300 and 600 cycles. Both are larger than IR_TX_FALL_STRETCH_CYCLES (200, in ir.c), thus
       a stretched falling edge stays before the pulse that follows it. */
    for (i = 0; i < edge_count; i++) {
        uint32_t gap = (i % 3u == 0u) ? 600u : 300u;
        psemu_bus_write32(&ps_tx->bus, IRDA_DATA, (i & 1u) ? 0u : IR_DATA_LED);
        ir_tick(&ps_tx->ir, &ps_tx->intc, gap);
        span_cycles += gap;
        if ((i + 1u) % BURST_EDGES_PER_FRAME == 0u) {
            ir_link_pump(&host_link, ps_tx);
            ir_link_pump(&client_link, ps_rx);
            Sleep(31); /* what the desktop loop does after rendering each frame */
        }
    }
    span_us = (int64_t)((span_cycles * 1000000ull) / (uint64_t)PSEMU_ASSUMED_CPU_HZ);

    /* This mode never calls ir_tick on ps_rx. Thus no edge becomes due, and rx_queue holds the full
       group for the comparison below. */
    for (i = 0; i < 20000 && client_link.edges_received < edge_count; i++) {
        ir_link_pump(&host_link, ps_tx);
        ir_link_pump(&client_link, ps_rx);
        if (host_link.state != IR_LINK_CONNECTED || client_link.state != IR_LINK_CONNECTED) {
            break;
        }
        Sleep(1);
    }

    printf("host  state=%s tx=%lu drop=%lu transport writes=%lu\n", ir_link_status_text(&host_link),
        host_link.edges_sent, host_link.dropped_tx, host_link.transport_writes);
    printf("client state=%s rx=%lu queued=%lu lead min=%lldus max=%lldus late=%lu\n",
        ir_link_status_text(&client_link), client_link.edges_received, (unsigned long)ps_rx->ir.rx_queue.count,
        (long long)client_link.min_lead_us, (long long)client_link.max_lead_us, client_link.late_edges);

    if (host_link.state != IR_LINK_CONNECTED || client_link.state != IR_LINK_CONNECTED) {
        fprintf(stderr, "FAIL: the link left the connected state during the group\n");
        ok = 0;
    }
    if (host_link.edges_sent != edge_count || host_link.dropped_tx != 0) {
        fprintf(stderr, "FAIL: the sender did not queue every edge\n");
        ok = 0;
    }
    if (client_link.edges_received != edge_count) {
        fprintf(stderr, "FAIL: the receiver got %lu of %lu edges\n", client_link.edges_received, edge_count);
        ok = 0;
    }
    /* The lead is the time by which an arriving edge is scheduled ahead of the local IR clock of the
       receiver. This mode calls ir_tick on the receiver only before the link exists, thus the clock of the
       receiver holds still while the group arrives. The emulated clock of the sender does advance, by
       span_us across the group. Thus the lead of the first edge is near IR_LINK_PLAYOUT_DELAY_US, and each
       later edge adds its part of span_us. A measurement of 700 edges gives 221027us to 485989us, against
       a planned 250000us and a span of 265000us.
       The limit must hold span_us, because a larger group gives larger leads for that reason alone. A
       group of 4000 edges measures 1739203us at the end, and that value is correct.

       This is the check that needs the measured offset of the peer clock. Without that offset, the
       timestamp of each edge is wrong by the full distance between the two clocks, and one hour of error
       is far outside this window in one direction or the other. A mutation test confirms it: with the
       offset term removed and one hour injected, all 700 edges arrive late, with each lead at -1750000us.
       A count of the edges and an order of the levels both stay correct in that condition, thus neither
       one can take the place of this check. */
    if (client_link.min_lead_us <= 0 || client_link.late_edges != 0 ||
        client_link.max_lead_us > (int64_t)IR_LINK_PLAYOUT_DELAY_US + span_us + 250000) {
        fprintf(stderr, "FAIL: the receiver placed the group outside the playout buffer (span=%lldus)\n",
            (long long)span_us);
        ok = 0;
    }
    if ((unsigned long)ps_rx->ir.rx_queue.count != edge_count) {
        fprintf(stderr, "FAIL: rx_queue holds %lu of %lu edges\n", (unsigned long)ps_rx->ir.rx_queue.count,
            edge_count);
        ok = 0;
    } else {
        /* A transmit group alternates the level at each edge, and it starts with the LED on. An edge
           that arrives in the incorrect order breaks that sequence. */
        unsigned long wrong_level = 0;
        for (i = 0; i < edge_count; i++) {
            uint32_t slot = (ps_rx->ir.rx_queue.head + (uint32_t)i) % IR_EDGE_QUEUE_CAPACITY;
            int expected = (i & 1u) ? 0 : 1;
            if (ps_rx->ir.rx_queue.entries[slot].level != expected) {
                wrong_level++;
            }
        }
        if (wrong_level != 0) {
            fprintf(stderr, "FAIL: %lu edges arrived with the incorrect level (order changed)\n", wrong_level);
            ok = 0;
        }
    }

    ir_link_disconnect(&host_link);
    ir_link_disconnect(&client_link);
    psemu_destroy(ps_tx);
    psemu_destroy(ps_rx);
    if (!ok) {
        return 1;
    }
    printf("PASS: %lu edges crossed the link as one group, in order, with no loss\n", edge_count);
    return 0;
}

int main(int argc, char **argv) {
    ir_link_t host_link, client_link;
    psemu_t *ps_tx;
    psemu_t *ps_rx;

    /* A --tcp first argument selects the TCP transport for whichever mode follows it. The remaining
       arguments then shift by one, thus each mode reads them from the same positions as before. */
    if (argc >= 2 && strcmp(argv[1], "--tcp") == 0) {
        g_use_tcp = 1;
        argv[1] = argv[0];
        argv++;
        argc--;
        printf("transport: TCP on %s\n", selftest_address());
    }

    if (argc >= 2 && strcmp(argv[1], "--bytes") == 0) {
        return run_byte_fidelity(argc >= 3 ? strtoul(argv[2], NULL, 10) : 262144ul);
    }
    if (argc >= 2 && strcmp(argv[1], "--burst") == 0) {
        return run_burst(argc >= 3 ? strtoul(argv[2], NULL, 10) : 700ul, argc >= 4 ? _atoi64(argv[3]) : 0ll);
    }
    if (argc >= 2 && strcmp(argv[1], "--sync") == 0) {
        /* One hour by default. A real pair of machines with no time service can differ by that much, and
           a value that large cannot come from the variation in the transit time. */
        return run_sync(argc >= 3 ? atol(argv[2]) : 4, argc >= 4 ? _atoi64(argv[3]) : 3600000000ll);
    }

    if (argc >= 5 && (strcmp(argv[1], "--host") == 0 || strcmp(argv[1], "--client") == 0)) {
        return run_role(strcmp(argv[1], "--host") == 0, argv[2], argv[3], argv[4],
            argc >= 6 ? atol(argv[5]) : 400, argc >= 7 ? atol(argv[6]) : 0);
    }
    if (argc >= 4) {
        return run_transfer(argv[1], argv[2], argv[3], argc >= 5 ? atol(argv[4]) : 400);
    }

    ps_tx = psemu_create();
    ps_rx = psemu_create();

    selftest_link_init(&host_link);
    selftest_link_init(&client_link);

    if (!ir_link_host(&host_link, selftest_address())) {
        fprintf(stderr, "ir_link_host failed: %s\n", ir_link_status_text(&host_link));
        return 1;
    }
    ir_link_connect(&client_link, selftest_address());

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

    printf("PASS: an edge written on one psemu_t's IR TX registers relayed over the link and asserted "
           "INT_IRDA on a separate psemu_t\n");

    ir_link_disconnect(&host_link);
    ir_link_disconnect(&client_link);
    psemu_destroy(ps_tx);
    psemu_destroy(ps_rx);
    return 0;
}
