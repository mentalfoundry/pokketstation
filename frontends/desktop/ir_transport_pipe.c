/* The local named-pipe transport for an IR link. See ir_transport.h for the interface, and ir_link.h
   for the timing that operates above it.

   Both endpoints are on one machine. Thus this transport needs no address resolution and no clock
   synchronization: each process reads the same wall clock with no coordination.

   All I/O is overlapped, which means asynchronous. Each operation tests only whether an operation that
   is already in progress is complete. No call blocks. */
#include "ir_transport.h"

#include <stdio.h>
#include <string.h>

/* The size of the pipe buffer of the operating system, in bytes. It holds 4096 wire messages of 16
   bytes, which is several full IR messages.
   A small buffer is the limit on the send rate. At 8 messages, a sender could send only 8 edges before
   it blocked. A real transfer makes approximately 65 edges for each frame. */
#define IR_TRANSPORT_PIPE_OS_BUFFER_SIZE 65536u

static void set_error(ir_transport_t *t, const char *prefix, DWORD err) {
    snprintf(t->error, sizeof(t->error), "%s (error %lu)", prefix, (unsigned long)err);
    t->state = IR_TRANSPORT_ERROR;
}

/* Manual-reset events, one for each operation that can be in progress.
   This code only calls GetOverlappedResult with bWait = FALSE. But a NULL hEvent makes the OVERLAPPED
   structure use the pipe handle as its signal object. That is not safe when a read and a write are both
   in progress on that same handle. See the remarks on ReadFile and WriteFile in the platform
   documentation. Thus each operation gets its own event. */
static int make_events(ir_transport_t *t) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    if (p->ev_connect == NULL) {
        p->ev_connect = CreateEventA(NULL, TRUE, FALSE, NULL);
    }
    if (p->ev_read == NULL) {
        p->ev_read = CreateEventA(NULL, TRUE, FALSE, NULL);
    }
    if (p->ev_write == NULL) {
        p->ev_write = CreateEventA(NULL, TRUE, FALSE, NULL);
    }
    if (p->ev_connect == NULL || p->ev_read == NULL || p->ev_write == NULL) {
        set_error(t, "Couldn't create an event", GetLastError());
        return 0;
    }
    return 1;
}

static void close_events(ir_transport_pipe_t *p) {
    if (p->ev_connect != NULL) {
        CloseHandle(p->ev_connect);
        p->ev_connect = NULL;
    }
    if (p->ev_read != NULL) {
        CloseHandle(p->ev_read);
        p->ev_read = NULL;
    }
    if (p->ev_write != NULL) {
        CloseHandle(p->ev_write);
        p->ev_write = NULL;
    }
}

static void pipe_close(ir_transport_t *t) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    if (p->pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(p->pipe, NULL);
        if (p->is_server) {
            DisconnectNamedPipe(p->pipe);
        }
        CloseHandle(p->pipe);
        p->pipe = INVALID_HANDLE_VALUE;
    }
    /* The events close with the pipe. An overlapped operation can reference an event, thus this order
       is necessary: the pipe closes first, and CancelIoEx above ends each operation on it. */
    close_events(p);
    p->read_pending = 0;
    p->write_pending = 0;
    p->read_fill = 0;
    p->read_head = 0;
    p->write_len = 0;
    t->state = IR_TRANSPORT_IDLE;
    t->error[0] = 0;
}

static void start_read(ir_transport_t *t) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    BOOL ok;
    ZeroMemory(&p->ov_read, sizeof(p->ov_read));
    p->ov_read.hEvent = p->ev_read;
    ResetEvent(p->ev_read);
    p->read_fill = 0;
    p->read_head = 0;
    ok = ReadFile(p->pipe, p->read_buf, (DWORD)sizeof(p->read_buf), NULL, &p->ov_read);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        set_error(t, "Read failed", GetLastError());
        return;
    }
    p->read_pending = 1;
}

static void start_write(ir_transport_t *t) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    BOOL ok;
    if (p->write_pending || p->write_len == 0) {
        return;
    }
    ZeroMemory(&p->ov_write, sizeof(p->ov_write));
    p->ov_write.hEvent = p->ev_write;
    ResetEvent(p->ev_write);
    ok = WriteFile(p->pipe, p->write_buf, p->write_len, NULL, &p->ov_write);
    if (!ok && GetLastError() != ERROR_IO_PENDING) {
        set_error(t, "Write failed", GetLastError());
        return;
    }
    p->write_pending = 1;
}

static void on_connected(ir_transport_t *t) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    t->state = IR_TRANSPORT_CONNECTED;
    p->read_pending = 0;
    p->write_pending = 0;
    p->read_fill = 0;
    p->read_head = 0;
    p->write_len = 0;
    /* One read is in progress from this point. Thus the first ir_transport_recv call of the caller can
       complete an operation, and it does not have to start one first. */
    start_read(t);
}

static int pipe_host(ir_transport_t *t, const char *address) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    pipe_close(t); /* idempotent: clears any previous attempt first */
    if (!make_events(t)) {
        return 0;
    }
    snprintf(p->name, sizeof(p->name), "%s", address);
    p->is_server = 1;
    p->pipe = CreateNamedPipeA(p->name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1 /* one peer, this is a point-to-point link */,
        IR_TRANSPORT_PIPE_OS_BUFFER_SIZE, IR_TRANSPORT_PIPE_OS_BUFFER_SIZE, 0, NULL);
    if (p->pipe == INVALID_HANDLE_VALUE) {
        set_error(t, "Couldn't create pipe", GetLastError());
        return 0;
    }

    ZeroMemory(&p->ov_connect, sizeof(p->ov_connect));
    p->ov_connect.hEvent = p->ev_connect;
    ResetEvent(p->ev_connect);
    if (ConnectNamedPipe(p->pipe, &p->ov_connect)) {
        on_connected(t); /* unexpected-but-handled synchronous success */
        return 1;
    }
    switch (GetLastError()) {
    case ERROR_PIPE_CONNECTED: /* a peer already raced in before this call */
        on_connected(t);
        break;
    case ERROR_IO_PENDING:
        t->state = IR_TRANSPORT_LISTENING;
        break;
    default:
        set_error(t, "Couldn't listen", GetLastError());
        CloseHandle(p->pipe);
        p->pipe = INVALID_HANDLE_VALUE;
        return 0;
    }
    return 1;
}

/* A named pipe has no separate "connect" handshake step on the client side. A socket has such a step.
   CreateFileA is immediately successful, and the client is then connected. Or it fails, because no
   server listens at this time. Thus this code calls CreateFileA again at each poll call. That
   repetition does the function of an asynchronous connect. */
static void try_client_connect(ir_transport_t *t) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    DWORD mode;
    HANDLE h =
        CreateFileA(p->name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PIPE_BUSY) {
            return; /* no host listening yet (or its single slot is taken) - keep retrying */
        }
        set_error(t, "Couldn't connect", err);
        return;
    }
    mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(h, &mode, NULL, NULL);
    p->pipe = h;
    on_connected(t);
}

static int pipe_connect(ir_transport_t *t, const char *address) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    pipe_close(t);
    if (!make_events(t)) {
        return 0;
    }
    snprintf(p->name, sizeof(p->name), "%s", address);
    p->is_server = 0;
    t->state = IR_TRANSPORT_CONNECTING;
    try_client_connect(t);
    return 1;
}

static void pipe_poll(ir_transport_t *t) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    switch (t->state) {
    case IR_TRANSPORT_LISTENING: {
        DWORD bytes;
        if (GetOverlappedResult(p->pipe, &p->ov_connect, &bytes, FALSE)) {
            on_connected(t);
        } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
            set_error(t, "Listen failed", GetLastError());
        }
        break;
    }
    case IR_TRANSPORT_CONNECTING:
        try_client_connect(t);
        break;
    default:
        break;
    }
}

static size_t pipe_recv(ir_transport_t *t, void *data, size_t size) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    size_t available;
    if (t->state != IR_TRANSPORT_CONNECTED || size == 0) {
        return 0;
    }
    if (p->read_head == p->read_fill) {
        DWORD bytes = 0;
        /* The buffer holds no more data for the caller, thus a new read can use it. The read that is in
           progress owns the buffer until it completes. Thus this point is the earliest one at which
           this code can start the next read. */
        if (!p->read_pending) {
            start_read(t);
            return 0;
        }
        if (!GetOverlappedResult(p->pipe, &p->ov_read, &bytes, FALSE)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_INCOMPLETE) {
                set_error(t, "Peer disconnected", err);
            }
            return 0;
        }
        p->read_pending = 0;
        p->read_fill = (uint32_t)bytes;
        p->read_head = 0;
        if (bytes == 0) {
            start_read(t);
            return 0;
        }
    }
    available = p->read_fill - p->read_head;
    if (available > size) {
        available = size;
    }
    memcpy(data, p->read_buf + p->read_head, available);
    p->read_head += (uint32_t)available;
    if (p->read_head == p->read_fill && !p->read_pending) {
        /* The caller took each byte, thus a new read can use the buffer. Start that read here, and do
           not wait for the next call.
           The caller stops its loop when a call gives less data than it asked for. A caller that asks
           for the remainder of a partial wire message gets such a result, and it then leaves the read
           path for that frame. Thus a read that starts only at the next call makes the remainder of
           that message wait one full frame, and each later message of the group waits behind it. One
           read is always in progress at the end of this function, thus that delay does not occur. */
        start_read(t);
    }
    return available;
}

static size_t pipe_send(ir_transport_t *t, const void *data, size_t size) {
    ir_transport_pipe_t *p = &t->impl.pipe;
    if (t->state != IR_TRANSPORT_CONNECTED || size == 0) {
        return 0;
    }
    if (p->write_pending) {
        DWORD bytes = 0;
        if (!GetOverlappedResult(p->pipe, &p->ov_write, &bytes, FALSE)) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_INCOMPLETE) {
                set_error(t, "Peer disconnected", err);
            }
            return 0; /* the write is still in progress: the caller keeps its bytes */
        }
        p->write_pending = 0;
        if (bytes < p->write_len) {
            /* A partial write. The remainder moves to the front of the buffer, and a new operation
               sends it. The caller keeps its bytes until that operation is complete. */
            memmove(p->write_buf, p->write_buf + bytes, p->write_len - bytes);
            p->write_len -= bytes;
            start_write(t);
            return 0;
        }
        p->write_len = 0;
    }
    if (size > sizeof(p->write_buf)) {
        size = sizeof(p->write_buf);
    }
    memcpy(p->write_buf, data, size);
    p->write_len = (uint32_t)size;
    start_write(t);
    if (t->state != IR_TRANSPORT_CONNECTED) {
        return 0;
    }
    return size;
}

static const ir_transport_vtable_t pipe_vtable = {
    pipe_host,
    pipe_connect,
    pipe_poll,
    pipe_send,
    pipe_recv,
    pipe_close,
};

void ir_transport_init_pipe(ir_transport_t *t) {
    ZeroMemory(t, sizeof(*t));
    t->vt = &pipe_vtable;
    t->state = IR_TRANSPORT_IDLE;
    t->impl.pipe.pipe = INVALID_HANDLE_VALUE;
}

int ir_transport_host(ir_transport_t *t, const char *address) {
    return t->vt->host(t, address);
}

int ir_transport_connect(ir_transport_t *t, const char *address) {
    return t->vt->connect(t, address);
}

void ir_transport_poll(ir_transport_t *t) {
    t->vt->poll(t);
}

size_t ir_transport_send(ir_transport_t *t, const void *data, size_t size) {
    return t->vt->send(t, data, size);
}

size_t ir_transport_recv(ir_transport_t *t, void *data, size_t size) {
    return t->vt->recv(t, data, size);
}

void ir_transport_close(ir_transport_t *t) {
    t->vt->close(t);
}
