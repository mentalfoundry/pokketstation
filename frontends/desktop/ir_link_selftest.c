/* Ad-hoc, manually-run verification tool for ir_link.c's actual Windows named-pipe transport - not part of the
   automated CTest suite (see tests/ir_test.c for that; it covers core/src/ir.c's state machine without any
   transport at all). This drives two ir_link_t endpoints, host and client, over a real named pipe within one
   process, and confirms an edge written on one psemu_t's IR TX registers ends up asserting INT_IRDA on a
   completely separate psemu_t after relaying through the pipe - the same path two real pokketstation.exe
   instances would use, minus the two-process split itself. */
#include <assert.h>
#include <stdio.h>

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

int main(void) {
    ir_link_t host_link, client_link;
    psemu_t *ps_tx = psemu_create();
    psemu_t *ps_rx = psemu_create();

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
        /* ir_link.c now deliberately schedules an incoming edge IR_LINK_PLAYOUT_DELAY_US into the receiver's
           own future (a jitter buffer - see ir_link.h), so ps_rx's IR clock actually has to advance that far
           before the edge becomes due. Nothing else in this test calls psemu_run, so it has to be advanced by
           hand here; ~1056 cycles per loop iteration matches the real ~1ms this loop already sleeps per
           iteration (PSEMU_ASSUMED_CPU_HZ / 1000). */
        for (i = 0; i < 3000 && !seen; i++) {
            ir_link_pump(&host_link, ps_tx);
            ir_link_pump(&client_link, ps_rx);
            ir_tick(&ps_rx->ir, &ps_rx->intc, 1056u);
            seen = intc_irq_asserted(&ps_rx->intc);
            Sleep(1);
        }
        if (!seen) {
            fprintf(stderr, "INT_IRDA never asserted on the receiving instance - transport did not relay the edge\n");
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
