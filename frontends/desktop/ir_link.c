#include "ir_link.h"

#include <stdio.h>
#include <string.h>

static uint64_t host_wall_us_now(void) {
    FILETIME ft;
    ULARGE_INTEGER uli;
    /* Precise (sub-millisecond) and, critically, directly comparable across any two processes on this same
       machine with zero coordination - unlike QueryPerformanceCounter, which is only meaningful within one
       process's own timeline on some hardware. */
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
    /* Manual-reset events, one per outstanding operation. GetOverlappedResult is only polled with bWait=FALSE
       here, but a NULL hEvent would make the OVERLAPPED use the pipe *handle itself* as its signal object -
       unsafe once a read and a write can be simultaneously outstanding on that same handle (see MSDN's
       ReadFile/WriteFile remarks). Each op gets its own event instead. */
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
        return; /* peer isn't draining fast enough (or the link is stuck) - drop rather than stall psemu_run */
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
        (DWORD)(sizeof(ir_wire_message_t) * 8u), (DWORD)(sizeof(ir_wire_message_t) * 8u), 0, NULL);
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
        int64_t wall_minus_core_us = (int64_t)host_wall_us_now() - (int64_t)psemu_ir_get_clock_us(ps);
        uint64_t local_us = wall_to_local_us(wall_minus_core_us, msg->timestamp_us) + IR_LINK_PLAYOUT_DELAY_US;
        psemu_ir_push_rx_edge(ps, local_us, msg->level);
    }
}

static void poll_read(ir_link_t *link, psemu_t *ps) {
    DWORD bytes;
    if (!link->read_pending) {
        return;
    }
    if (!GetOverlappedResult(link->pipe, &link->ov_read, &bytes, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            return;
        }
        link->state = IR_LINK_ERROR;
        set_status_with_error(link, "Peer disconnected", err);
        return;
    }
    link->read_pending = 0;
    if (bytes == sizeof(link->read_msg)) {
        handle_incoming_message(link, ps, &link->read_msg);
    }
    if (link->state == IR_LINK_CONNECTED) {
        start_read(link);
    }
}

static void poll_write(ir_link_t *link) {
    DWORD bytes;
    if (!link->write_pending) {
        return;
    }
    if (!GetOverlappedResult(link->pipe, &link->ov_write, &bytes, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_INCOMPLETE) {
            return;
        }
        link->state = IR_LINK_ERROR;
        set_status_with_error(link, "Peer disconnected", err);
        return;
    }
    link->write_pending = 0;
    link->write_head = (link->write_head + 1u) % IR_LINK_WRITE_QUEUE_CAPACITY;
    link->write_count--;
}

static void drain_tx_edges(ir_link_t *link, psemu_t *ps) {
    psemu_ir_edge_t edge;
    int64_t wall_minus_core_us = (int64_t)host_wall_us_now() - (int64_t)psemu_ir_get_clock_us(ps);
    while (psemu_ir_pop_tx_edge(ps, &edge)) {
        uint64_t wall_us = local_to_wall_us(wall_minus_core_us, edge.timestamp_us);
        enqueue_write(link, wall_us, edge.level, IR_WIRE_KIND_EDGE);
    }
}

static void pump_connected(ir_link_t *link, psemu_t *ps) {
    poll_read(link, ps);
    if (link->state != IR_LINK_CONNECTED) {
        return;
    }
    poll_write(link);
    if (link->state != IR_LINK_CONNECTED) {
        return;
    }
    drain_tx_edges(link, ps);
    start_write(link);
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
