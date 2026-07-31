#include "ir_link.h"

#include <stdio.h>
#include <string.h>

static uint64_t host_wall_us_now(void) {
    FILETIME ft;
    ULARGE_INTEGER uli;
    /* This clock is precise to below a millisecond.
       More importantly, any two processes on this same machine can compare it with no coordination.
       QueryPerformanceCounter cannot do that. On some hardware it is meaningful only within one process. */
    GetSystemTimePreciseAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart / 10ull; /* 100ns units since 1601 -> microseconds */
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

static void set_status_with_error(ir_link_t *link, const char *prefix, DWORD err) {
    snprintf(link->status, sizeof(link->status), "%s (error %lu)", prefix, (unsigned long)err);
}

void ir_link_init(ir_link_t *link) {
    ZeroMemory(link, sizeof(*link));
    link->pipe = INVALID_HANDLE_VALUE;
    link->state = IR_LINK_IDLE;
    /* Manual-reset events, one for each outstanding operation.
       This only polls GetOverlappedResult with bWait=FALSE.
       Even so, a NULL hEvent makes the OVERLAPPED use the pipe handle itself as its signal object.
       That is unsafe once a read and a write are outstanding on that same handle at the same time.
       See the ReadFile and WriteFile remarks in the platform documentation.
       Each operation therefore gets its own event. */
    link->ev_connect = CreateEventA(NULL, TRUE, FALSE, NULL);
    link->ev_read = CreateEventA(NULL, TRUE, FALSE, NULL);
    link->ev_write = CreateEventA(NULL, TRUE, FALSE, NULL);
    set_status(link, "Idle");
}

void ir_link_disconnect(ir_link_t *link) {
    if (link->pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(link->pipe, NULL);
        if (link->is_server) {
            DisconnectNamedPipe(link->pipe);
        }
        CloseHandle(link->pipe);
        link->pipe = INVALID_HANDLE_VALUE;
    }
    link->state = IR_LINK_IDLE;
    link->read_pending = 0;
    link->write_pending = 0;
    link->write_head = 0;
    link->write_count = 0;
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
    link->write_count++;
}

static void start_read(ir_link_t *link) {
    BOOL ok;
    ZeroMemory(&link->ov_read, sizeof(link->ov_read));
    link->ov_read.hEvent = link->ev_read;
    ResetEvent(link->ev_read);
    ok = ReadFile(link->pipe, &link->read_msg, sizeof(link->read_msg), NULL, &link->ov_read);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        link->state = IR_LINK_ERROR;
        set_status_with_error(link, "Read failed", GetLastError());
        return;
    }
    link->read_pending = 1;
}

static void start_write(ir_link_t *link) {
    ir_wire_message_t *msg;
    BOOL ok;
    if (link->write_pending || link->write_count == 0) {
        return;
    }
    msg = &link->write_queue[link->write_head];
    ZeroMemory(&link->ov_write, sizeof(link->ov_write));
    link->ov_write.hEvent = link->ev_write;
    ResetEvent(link->ev_write);
    ok = WriteFile(link->pipe, msg, sizeof(*msg), NULL, &link->ov_write);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        link->state = IR_LINK_ERROR;
        set_status_with_error(link, "Write failed", GetLastError());
        return;
    }
    link->write_pending = 1;
}

static void on_connected(ir_link_t *link) {
    link->state = IR_LINK_CONNECTED;
    link->read_pending = 0;
    link->write_pending = 0;
    link->write_head = 0;
    link->write_count = 0;
    link->clock_offset_latched = 0; /* re-latched on this connection's first conversion */
    set_status(link, "Connected");
    start_read(link);
    enqueue_write(link, 0, 0, IR_WIRE_KIND_HELLO);
}

int ir_link_host(ir_link_t *link, const char *pipe_name) {
    ir_link_disconnect(link); /* idempotent: clears any previous attempt first */
    snprintf(link->pipe_name, sizeof(link->pipe_name), "%s", pipe_name);
    link->is_server = 1;
    link->pipe = CreateNamedPipeA(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1 /* one peer, this is a point-to-point link */,
        /* Buffers sized for a whole message burst. At 8 messages the pipe itself became the bottleneck: a
           sender could only hand over 8 edges before blocking, against the ~65 per frame a real transfer
           produces. */
        (DWORD)(sizeof(ir_wire_message_t) * IR_LINK_WRITE_QUEUE_CAPACITY),
        (DWORD)(sizeof(ir_wire_message_t) * IR_LINK_WRITE_QUEUE_CAPACITY), 0, NULL);
    if (link->pipe == INVALID_HANDLE_VALUE) {
        link->state = IR_LINK_ERROR;
        set_status_with_error(link, "Couldn't create pipe", GetLastError());
        return 0;
    }

    ZeroMemory(&link->ov_connect, sizeof(link->ov_connect));
    link->ov_connect.hEvent = link->ev_connect;
    ResetEvent(link->ev_connect);
    if (ConnectNamedPipe(link->pipe, &link->ov_connect)) {
        on_connected(link); /* unexpected-but-handled synchronous success */
        return 1;
    }
    switch (GetLastError()) {
    case ERROR_PIPE_CONNECTED: /* a peer already raced in before this call */
        on_connected(link);
        break;
    case ERROR_IO_PENDING:
        link->state = IR_LINK_HOSTING;
        set_status(link, "Waiting for peer...");
        break;
    default:
        CloseHandle(link->pipe);
        link->pipe = INVALID_HANDLE_VALUE;
        link->state = IR_LINK_ERROR;
        set_status_with_error(link, "Couldn't listen", GetLastError());
        return 0;
    }
    return 1;
}

/* Named pipes have no separate client-side "connect" handshake step the way sockets do: CreateFileA either
   succeeds immediately (the client is connected, full stop) or fails because no server is listening yet.
   Retrying that call every pump is what stands in for a real async connect. */
static void try_client_connect(ir_link_t *link) {
    DWORD mode;
    HANDLE h = CreateFileA(
        link->pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PIPE_BUSY) {
            return; /* no host listening yet (or its single slot is taken) - keep retrying */
        }
        link->state = IR_LINK_ERROR;
        set_status_with_error(link, "Couldn't connect", err);
        return;
    }
    mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
    link->pipe = h;
    on_connected(link);
}

int ir_link_connect(ir_link_t *link, const char *pipe_name) {
    ir_link_disconnect(link);
    snprintf(link->pipe_name, sizeof(link->pipe_name), "%s", pipe_name);
    link->is_server = 0;
    link->state = IR_LINK_CONNECTING;
    set_status(link, "Connecting...");
    try_client_connect(link);
    return 1;
}

/* Returns this link's wall-to-core offset, sampling it once on first use after a connect and reusing it from
   then on. See ir_link.h's wall_minus_core_us for why this must not be resampled per call. */
static int64_t link_clock_offset(ir_link_t *link, psemu_t *ps) {
    uint64_t now = host_wall_us_now();
    /* Re-latch on a fresh connection, and again whenever the link has been quiet long enough that no message
       can be in flight. Holding it across a message keeps that message's edge spacing exact; refreshing it
       between messages stops the two processes' clock drift from accumulating past the playout buffer.
       See IR_LINK_OFFSET_RELATCH_IDLE_US. */
    if (!link->clock_offset_latched || now - link->last_edge_wall_us >= IR_LINK_OFFSET_RELATCH_IDLE_US) {
        link->wall_minus_core_us = (int64_t)now - (int64_t)psemu_ir_get_clock_us(ps);
        link->clock_offset_latched = 1;
    }
    return link->wall_minus_core_us;
}

/* Records edge activity, which is what the idle test above measures. */
static void note_edge_activity(ir_link_t *link) {
    link->last_edge_wall_us = host_wall_us_now();
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
    {
        int64_t wall_minus_core_us = link_clock_offset(link, ps);
        uint64_t local_us = wall_to_local_us(wall_minus_core_us, msg->timestamp_us) + IR_LINK_PLAYOUT_DELAY_US;
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

/* Returns 1 if a message was completed and consumed, 0 if nothing was ready. */
static int poll_read(ir_link_t *link, psemu_t *ps) {
    DWORD bytes;
    if (!link->read_pending) {
        return 0;
    }
    if (!GetOverlappedResult(link->pipe, &link->ov_read, &bytes, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            return 0;
        }
        link->state = IR_LINK_ERROR;
        set_status_with_error(link, "Peer disconnected", err);
        return 0;
    }
    link->read_pending = 0;
    if (bytes == sizeof(link->read_msg)) {
        handle_incoming_message(link, ps, &link->read_msg);
    }
    if (link->state == IR_LINK_CONNECTED) {
        start_read(link);
    }
    return 1;
}

/* Returns 1 if the outstanding write completed, 0 if it is still in flight. */
static int poll_write(ir_link_t *link) {
    DWORD bytes;
    if (!link->write_pending) {
        return 0;
    }
    if (!GetOverlappedResult(link->pipe, &link->ov_write, &bytes, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            return 0;
        }
        link->state = IR_LINK_ERROR;
        set_status_with_error(link, "Peer disconnected", err);
        return 0;
    }
    link->write_pending = 0;
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

/* One pump must move a whole frame's worth of edges, not one message.
   A real IR burst is hundreds of transitions produced across a handful of emulated frames - measured at 658
   edges for one Chocobo World message, roughly 65 per frame. This used to complete exactly one read and one
   write per pump, so the transport carried about one edge per frame in each direction and a burst could
   never get through in time; the write queue then overflowed and dropped the rest. Both directions now drain
   until the pipe has nothing left to give or nothing left to take, which is the same backpressure as before,
   just no longer throttled to one message per frame.
   The bound is a safety net against an unexpectedly hot pipe starving the rest of the frame, not an
   expected limit. */
#define IR_LINK_MAX_MESSAGES_PER_PUMP 4096

/* Keeps the connected status line carrying live link counters, so the window title shows what the transport
   is actually doing. A link that is connected but not carrying a transfer looks identical to a working one
   otherwise, and the three failure modes seen so far are only distinguishable by these numbers: the peer
   sending nothing (tx stays 0 on its side), edges dropped because a queue filled (drop climbs), and edges
   arriving too late to be placed in time (late climbs, which destroys decoding while everything else looks
   healthy). */
static void update_connected_status(ir_link_t *link) {
    if (!link->show_diagnostics) {
        return; /* plain "Connected", set once by on_connected */
    }
    snprintf(link->status, sizeof(link->status), "Connected  tx=%lu rx=%lu drop=%lu late=%lu", link->edges_sent,
        link->edges_received, link->dropped_tx, link->late_edges);
}

static void pump_connected(ir_link_t *link, psemu_t *ps) {
    int i;
    for (i = 0; i < IR_LINK_MAX_MESSAGES_PER_PUMP; i++) {
        if (!poll_read(link, ps) || link->state != IR_LINK_CONNECTED) {
            break;
        }
    }
    if (link->state != IR_LINK_CONNECTED) {
        return;
    }
    drain_tx_edges(link, ps);
    for (i = 0; i < IR_LINK_MAX_MESSAGES_PER_PUMP; i++) {
        poll_write(link);
        if (link->state != IR_LINK_CONNECTED) {
            return;
        }
        if (link->write_pending || link->write_count == 0) {
            break; /* still in flight, or nothing left to send */
        }
        start_write(link);
    }
    if (link->state == IR_LINK_CONNECTED) {
        update_connected_status(link);
    }
}

void ir_link_pump(ir_link_t *link, psemu_t *ps) {
    switch (link->state) {
    case IR_LINK_IDLE:
    case IR_LINK_ERROR:
        return;
    case IR_LINK_HOSTING: {
        DWORD bytes;
        if (GetOverlappedResult(link->pipe, &link->ov_connect, &bytes, FALSE)) {
            on_connected(link);
        } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
            link->state = IR_LINK_ERROR;
            set_status_with_error(link, "Listen failed", GetLastError());
        }
        break;
    }
    case IR_LINK_CONNECTING:
        try_client_connect(link);
        break;
    case IR_LINK_CONNECTED:
        break;
    }
    if (link->state == IR_LINK_CONNECTED) {
        pump_connected(link, ps);
    }
}
