#include "ir_link.h"

#include <stdio.h>
#include <string.h>

static uint64_t host_wall_us_now(void) {
    FILETIME ft;
    ULARGE_INTEGER uli;
    /* This clock has a resolution of less than one millisecond.
       More important, two processes on this same machine can compare its values with no coordination.
       QueryPerformanceCounter cannot do this. On some hardware, its value has a meaning only in one
       process. */
    GetSystemTimePreciseAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart / 10ull; /* 100ns units since 1601 -> microseconds */
}

/* The wall clock of this link. Each wall-clock read of ir_link.c uses this function, and not
   host_wall_us_now directly. Thus test_wall_offset_us moves the full clock of one link together: the
   timestamp that it writes on a message, and the time against which it measures an arrival. A test can
   then operate two links on two clocks that are hours apart, in one process. See test_wall_offset_us in
   ir_link.h. The field is 0 in the desktop frontend. */
static uint64_t link_wall_us_now(const ir_link_t *link) {
    return (uint64_t)((int64_t)host_wall_us_now() + link->test_wall_offset_us);
}

static uint64_t wall_to_local_us(int64_t wall_minus_core_us, uint64_t wall_us) {
    int64_t local = (int64_t)wall_us - wall_minus_core_us;
    return local < 0 ? 0u : (uint64_t)local;
}

static uint64_t local_to_wall_us(int64_t wall_minus_core_us, uint64_t local_us) {
    return (uint64_t)((int64_t)local_us + wall_minus_core_us);
}

static void set_status(ir_link_t *link, const char *text) {
    snprintf(link->status, sizeof(link->status), "%s", text);
}

void ir_link_init(ir_link_t *link) {
    ZeroMemory(link, sizeof(*link));
    link->state = IR_LINK_IDLE;
    ir_transport_init_pipe(&link->transport);
    set_status(link, "Idle");
}

void ir_link_disconnect(ir_link_t *link) {
    ir_transport_close(&link->transport);
    link->state = IR_LINK_IDLE;
    link->read_fill = 0;
    link->write_head = 0;
    link->write_count = 0;
    link->write_offset = 0;
    set_status(link, "Idle");
}

int ir_link_is_active(const ir_link_t *link) {
    return link->state != IR_LINK_IDLE;
}

const char *ir_link_status_text(const ir_link_t *link) {
    return link->status;
}

static void enqueue_write(ir_link_t *link, uint64_t timestamp_us, int level, uint8_t kind) {
    uint32_t tail;
    ir_wire_message_t *msg;
    if (link->write_count >= IR_LINK_WRITE_QUEUE_CAPACITY) {
        if (kind == IR_WIRE_KIND_EDGE) {
            link->dropped_tx++;
        }
        return; /* peer isn't draining fast enough (or the link is stuck) - drop rather than stall psemu_run */
    }
    if (kind == IR_WIRE_KIND_EDGE) {
        link->edges_sent++;
    }
    tail = (link->write_head + link->write_count) % IR_LINK_WRITE_QUEUE_CAPACITY;
    msg = &link->write_queue[tail];
    msg->magic = IR_WIRE_MAGIC;
    msg->version = IR_WIRE_VERSION;
    msg->level = (uint8_t)(level ? 1 : 0);
    msg->kind = kind;
    msg->timestamp_us = timestamp_us;
    msg->timestamp2_us = 0; /* only a PONG uses this field, and poll_write fills it at the send moment */
    link->write_count++;
}

/* Copies the fault text of the transport into the status line of the link. */
static void adopt_transport_error(ir_link_t *link) {
    link->state = IR_LINK_ERROR;
    set_status(link, link->transport.error[0] ? link->transport.error : "Link failed");
}

/* Puts one sample into the window. The window is small, thus it keeps the newest samples and it drops
   the oldest one. */
static void record_sync_sample(ir_link_t *link, int64_t offset_us, int64_t rtt_us) {
    uint32_t slot = (link->sync_sample_head + link->sync_sample_count) % IR_LINK_SYNC_WINDOW;
    if (link->sync_sample_count == IR_LINK_SYNC_WINDOW) {
        slot = link->sync_sample_head;
        link->sync_sample_head = (link->sync_sample_head + 1u) % IR_LINK_SYNC_WINDOW;
    } else {
        link->sync_sample_count++;
    }
    link->sync_samples[slot].offset_us = offset_us;
    link->sync_samples[slot].rtt_us = rtt_us;
    link->last_offset_us = offset_us;
    link->last_rtt_us = rtt_us;
}

/* Gives the offset of the sample with the smallest round trip. See IR_LINK_SYNC_WINDOW in ir_link.h for
   the reason that this code uses the smallest round trip, and not an average. */
static int64_t best_peer_offset(const ir_link_t *link) {
    uint32_t i;
    int64_t best_offset = 0;
    int64_t best_rtt = 0;
    for (i = 0; i < link->sync_sample_count; i++) {
        uint32_t slot = (link->sync_sample_head + i) % IR_LINK_SYNC_WINDOW;
        if (i == 0 || link->sync_samples[slot].rtt_us < best_rtt) {
            best_rtt = link->sync_samples[slot].rtt_us;
            best_offset = link->sync_samples[slot].offset_us;
        }
    }
    return best_offset;
}

/* Puts a PING in the queue when the interval is complete and no PING is in progress.
   A PING that gets no answer stops the measurement, thus IR_LINK_PING_TIMEOUT_US abandons it. */
static void maybe_send_ping(ir_link_t *link) {
    uint64_t now = link_wall_us_now(link);
    if (link->ping_outstanding) {
        if (now - link->ping_sent_wall_us < IR_LINK_PING_TIMEOUT_US) {
            return;
        }
        link->ping_outstanding = 0; /* the answer did not arrive: a new PING can go now */
    }
    if (link->sync_sample_count != 0 && now - link->last_ping_wall_us < IR_LINK_PING_INTERVAL_US) {
        return;
    }
    link->last_ping_wall_us = now;
    link->ping_sent_wall_us = now; /* poll_write records the send time again when the transport takes it */
    link->ping_outstanding = 1;
    enqueue_write(link, now, 0, IR_WIRE_KIND_PING);
}

static void on_connected(ir_link_t *link) {
    /* The link carries no edge before it has one clock sample. An edge that crosses with no measured
       offset lands as far from the correct time as the two wall clocks differ, which is minutes on two
       machines. The TX edges of this instance stay in the queue of the core until this state ends, thus
       no edge is lost. */
    link->state = IR_LINK_SYNCING;
    link->read_fill = 0;
    link->write_head = 0;
    link->write_count = 0;
    link->write_offset = 0;
    link->clock_offset_latched = 0; /* re-latched on this connection's first conversion */
    link->sync_sample_head = 0;
    link->sync_sample_count = 0;
    link->ping_outstanding = 0;
    link->peer_wall_minus_local_wall_us = 0;
    set_status(link, "Synchronizing...");
    enqueue_write(link, 0, 0, IR_WIRE_KIND_HELLO);
    maybe_send_ping(link);
}

int ir_link_host(ir_link_t *link, const char *address) {
    ir_link_disconnect(link); /* idempotent: clears any previous attempt first */
    if (!ir_transport_host(&link->transport, address)) {
        adopt_transport_error(link);
        return 0;
    }
    if (link->transport.state == IR_TRANSPORT_CONNECTED) {
        on_connected(link);
    } else {
        link->state = IR_LINK_HOSTING;
        set_status(link, "Waiting for peer...");
    }
    return 1;
}

int ir_link_connect(ir_link_t *link, const char *address) {
    ir_link_disconnect(link);
    link->state = IR_LINK_CONNECTING;
    set_status(link, "Connecting...");
    if (!ir_transport_connect(&link->transport, address)) {
        adopt_transport_error(link);
        return 1;
    }
    if (link->transport.state == IR_TRANSPORT_CONNECTED) {
        on_connected(link);
    }
    return 1;
}

/* Returns the wall-to-core offset of this link. This function samples the offset one time, at the
   first use after a connection. It then uses that same value. See wall_minus_core_us in ir_link.h for
   the reason that this code must not sample the offset at each call. */
static int64_t link_clock_offset(ir_link_t *link, psemu_t *ps) {
    uint64_t now = link_wall_us_now(link);
    /* Latch the offset again at a new connection. Latch it again also when the link is quiet for
       sufficient time, thus no message can be in transit. To hold the offset through a message keeps
       the edge spacing of that message exact. To latch it again between messages prevents an
       accumulation of clock drift between the two processes that is more than the playout buffer.
       See IR_LINK_OFFSET_RELATCH_IDLE_US. */
    if (!link->clock_offset_latched || now - link->last_edge_wall_us >= IR_LINK_OFFSET_RELATCH_IDLE_US) {
        link->wall_minus_core_us = (int64_t)now - (int64_t)psemu_ir_get_clock_us(ps);
        /* Latch the offset of the peer clock at this same moment, and hold it for this same duration.
           Both terms must be constant across one message. See peer_wall_minus_local_wall_us in
           ir_link.h.
           This point is also the best moment for the sample: the latch happens only after the link is
           quiet, thus the write queue holds no message and the recent samples have no queue delay. */
        link->peer_wall_minus_local_wall_us = best_peer_offset(link);
        link->clock_offset_latched = 1;
    }
    return link->wall_minus_core_us;
}

/* Records edge activity, which is what the idle test above measures. */
static void note_edge_activity(ir_link_t *link) {
    link->last_edge_wall_us = link_wall_us_now(link);
}

static void handle_incoming_message(ir_link_t *link, psemu_t *ps, const ir_wire_message_t *msg) {
    if (msg->magic != IR_WIRE_MAGIC || msg->version != IR_WIRE_VERSION) {
        link->state = IR_LINK_ERROR;
        set_status(link, "Peer speaks an incompatible protocol (mismatched build?)");
        return;
    }
    if (msg->kind == IR_WIRE_KIND_HELLO) {
        return; /* handshake only: its purpose was the magic/version check just above */
    }
    if (msg->kind == IR_WIRE_KIND_PING) {
        /* t1: the moment that this instance read the PING. poll_write adds t2 when the transport takes the
           answer, thus the turnaround between the two moments leaves the estimate of the peer. */
        enqueue_write(link, link_wall_us_now(link), 0, IR_WIRE_KIND_PONG);
        return;
    }
    if (msg->kind == IR_WIRE_KIND_PONG) {
        /* t0 and t3 are on the clock of this instance. t1 and t2 are on the clock of the peer. */
        int64_t t0 = (int64_t)link->ping_sent_wall_us;
        int64_t t1 = (int64_t)msg->timestamp_us;
        int64_t t2 = (int64_t)msg->timestamp2_us;
        int64_t t3 = (int64_t)link_wall_us_now(link);
        int64_t offset, rtt;
        if (!link->ping_outstanding) {
            return; /* a duplicate answer, or an answer to a PING that already timed out */
        }
        offset = ((t1 - t0) + (t2 - t3)) / 2;
        rtt = (t3 - t0) - (t2 - t1);
        link->ping_outstanding = 0;
        if (rtt < 0) {
            return; /* one of the two clocks moved during the round trip: this sample has no meaning */
        }
        record_sync_sample(link, offset, rtt);
        if (link->state == IR_LINK_SYNCING) {
            link->state = IR_LINK_CONNECTED;
            set_status(link, "Connected");
        }
        return;
    }
    {
        int64_t wall_minus_core_us = link_clock_offset(link, ps);
        /* Move the timestamp of the sender onto the wall clock of this instance, and then onto the local
           IR timeline. The term is 0 for two processes on one machine. */
        uint64_t peer_ts_local_wall_us =
            (uint64_t)((int64_t)msg->timestamp_us - link->peer_wall_minus_local_wall_us);
        uint64_t local_us = wall_to_local_us(wall_minus_core_us, peer_ts_local_wall_us) + IR_LINK_PLAYOUT_DELAY_US;
        int64_t lead = (int64_t)local_us - (int64_t)psemu_ir_get_clock_us(ps);
        if (link->edges_received == 0 || lead < link->min_lead_us) {
            link->min_lead_us = lead;
        }
        if (link->edges_received == 0 || lead > link->max_lead_us) {
            link->max_lead_us = lead;
        }
        if (lead <= 0) {
            link->late_edges++;
        }
        link->edges_received++;
        note_edge_activity(link);
        psemu_ir_push_rx_edge(ps, local_us, msg->level);
    }
}

/* Collects the bytes of one wire message from the transport, and uses that message when it is
   complete. A transport keeps no message boundary, thus a message can arrive in parts.
   Returns 1 if a message was completed and consumed, 0 if nothing was ready. */
static int poll_read(ir_link_t *link, psemu_t *ps) {
    size_t want = sizeof(link->read_bytes) - link->read_fill;
    size_t got = ir_transport_recv(&link->transport, link->read_bytes + link->read_fill, want);
    link->read_fill += (uint32_t)got;
    if (link->transport.state != IR_TRANSPORT_CONNECTED) {
        adopt_transport_error(link);
        return 0;
    }
    if (link->read_fill < sizeof(link->read_bytes)) {
        return 0; /* a partial message stays here until the remaining bytes arrive */
    }
    link->read_fill = 0;
    {
        ir_wire_message_t msg;
        memcpy(&msg, link->read_bytes, sizeof(msg));
        handle_incoming_message(link, ps, &msg);
    }
    return 1;
}

/* Offers the message at the head of the queue to the transport. A transport can take part of a
   message, thus write_offset records the part that it already took.
   Returns 1 if the full message went to the transport, 0 if the transport took no more data. */
static int poll_write(ir_link_t *link) {
    ir_wire_message_t *head = &link->write_queue[link->write_head];
    const uint8_t *bytes;
    size_t want = sizeof(ir_wire_message_t) - link->write_offset;
    /* A PING and a PONG get their timestamp here, and not at the moment that this code puts them in the
       queue. The queue can hold a full IR message of several hundred edges, and the time to drain it
       would become part of the measured round trip. The first byte of the message has not left yet, thus
       this point is the last moment at which the value can change. */
    if (link->write_offset == 0) {
        if (head->kind == IR_WIRE_KIND_PING) {
            uint64_t now = link_wall_us_now(link);
            head->timestamp_us = now;
            link->ping_sent_wall_us = now; /* t0 of the round trip */
        } else if (head->kind == IR_WIRE_KIND_PONG) {
            /* t2. timestamp_us already holds t1, from the moment that this instance read the PING. */
            head->timestamp2_us = link_wall_us_now(link);
        }
    }
    bytes = (const uint8_t *)head;
    size_t got = ir_transport_send(&link->transport, bytes + link->write_offset, want);
    if (link->transport.state != IR_TRANSPORT_CONNECTED) {
        adopt_transport_error(link);
        return 0;
    }
    link->write_offset += (uint32_t)got;
    if (link->write_offset < sizeof(ir_wire_message_t)) {
        return 0;
    }
    link->write_offset = 0;
    link->write_head = (link->write_head + 1u) % IR_LINK_WRITE_QUEUE_CAPACITY;
    link->write_count--;
    return 1;
}

static void drain_tx_edges(ir_link_t *link, psemu_t *ps) {
    psemu_ir_edge_t edge;
    int64_t wall_minus_core_us = link_clock_offset(link, ps);
    while (psemu_ir_pop_tx_edge(ps, &edge)) {
        uint64_t wall_us = local_to_wall_us(wall_minus_core_us, edge.timestamp_us);
        enqueue_write(link, wall_us, edge.level, IR_WIRE_KIND_EDGE);
        note_edge_activity(link);
    }
}

/* One pump call must move the edges of a full frame. It must not move only one message.
   A real IR group is hundreds of transitions across a few emulated frames. A measurement of one
   message gave 658 edges, which is approximately 65 edges for each frame. This code completed exactly
   one read and one write for each pump call before. Thus the transport carried approximately one edge
   for each frame in each direction, and a group could never get through in time. The write queue then
   overflowed and discarded the remainder. Both directions now drain until the transport has no more
   data to give or no more space to take data. The backpressure is the same as before, but the rate is
   no longer one message for each frame.
   The limit is a safety measure against a transport with unexpected activity that takes time from the
   remainder of the frame. It is not an expected limit. */
#define IR_LINK_MAX_MESSAGES_PER_PUMP 4096

/* Keeps the live link counters in the connected status line. Thus the window title shows the true
   operation of the transport. Without these counters, a link that is connected but carries no transfer
   looks the same as a link that operates correctly. These numbers are the only method to identify the
   three known failure modes: the peer sends nothing (tx stays at 0 on the side of the peer), a full
   queue causes discarded edges (drop increases), and edges arrive too late for correct placement (late
   increases, which stops the decode operation while each other value looks correct).
   The round trip and the clock offset are also here. They separate a fault in the network from a fault
   in the placement of an edge: a large round trip explains a late edge, and an offset that moves between
   two messages explains a group that does not decode. */
static void update_connected_status(ir_link_t *link) {
    if (!link->show_diagnostics) {
        return; /* plain "Connected", set once by the transition out of IR_LINK_SYNCING */
    }
    snprintf(link->status, sizeof(link->status), "Connected  tx=%lu rx=%lu drop=%lu late=%lu rtt=%lldus off=%lldus",
        link->edges_sent, link->edges_received, link->dropped_tx, link->late_edges, (long long)link->last_rtt_us,
        (long long)link->last_offset_us);
}

/* Returns a nonzero value while the transport carries messages. IR_LINK_SYNCING carries the handshake
   and the clock measurement, but no edge. */
static int link_carries_messages(const ir_link_t *link) {
    return link->state == IR_LINK_SYNCING || link->state == IR_LINK_CONNECTED;
}

static void pump_messages(ir_link_t *link, psemu_t *ps) {
    int i;
    for (i = 0; i < IR_LINK_MAX_MESSAGES_PER_PUMP; i++) {
        if (!poll_read(link, ps) || !link_carries_messages(link)) {
            break;
        }
    }
    if (!link_carries_messages(link)) {
        return;
    }
    if (link->state == IR_LINK_CONNECTED) {
        drain_tx_edges(link, ps);
    }
    maybe_send_ping(link);
    for (i = 0; i < IR_LINK_MAX_MESSAGES_PER_PUMP; i++) {
        if (link->write_count == 0) {
            break;
        }
        if (!poll_write(link) || !link_carries_messages(link)) {
            break; /* still in flight, or the transport reported a fault */
        }
    }
    if (link->state == IR_LINK_CONNECTED) {
        update_connected_status(link);
    }
}

void ir_link_pump(ir_link_t *link, psemu_t *ps) {
    if (link->state == IR_LINK_IDLE || link->state == IR_LINK_ERROR) {
        return;
    }
    ir_transport_poll(&link->transport);
    switch (link->transport.state) {
    case IR_TRANSPORT_LISTENING:
        link->state = IR_LINK_HOSTING;
        break;
    case IR_TRANSPORT_CONNECTING:
        link->state = IR_LINK_CONNECTING;
        break;
    case IR_TRANSPORT_CONNECTED:
        /* on_connected clears the sample window and puts the handshake in the queue. Thus it must run one
           time for each connection. IR_LINK_SYNCING and IR_LINK_CONNECTED both mean that it already
           ran. */
        if (!link_carries_messages(link)) {
            on_connected(link);
        }
        break;
    case IR_TRANSPORT_ERROR:
        adopt_transport_error(link);
        break;
    case IR_TRANSPORT_IDLE:
        link->state = IR_LINK_IDLE;
        set_status(link, "Idle");
        break;
    }
    if (link_carries_messages(link)) {
        pump_messages(link, ps);
    }
}
