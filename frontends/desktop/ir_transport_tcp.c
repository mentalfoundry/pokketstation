/* The TCP transport for an IR link, between two machines. See ir_transport.h for the interface, and
   ir_link.h for the timing that operates above it.

   TCP and not UDP. A discarded edge is unrecoverable, because the space between the edges holds the data.
   TCP gives the order and the delivery, and a transport on UDP would have to give them again. A
   retransmission delays a full group by the same quantity, and a uniform delay is what the playout buffer
   absorbs.

   The sockets are non-blocking, and the I/O is not overlapped. The model of this interface is "test what
   is ready and return immediately", and a non-blocking socket gives that model with no completion object.

   The two machines do not share a wall clock. ir_link.c measures that difference with PING and PONG, and
   this file has no part in it. */
#include "ir_transport.h"

#include <stdio.h>
#include <string.h>

static void set_error(ir_transport_t *t, const char *prefix, int err) {
    snprintf(t->error, sizeof(t->error), "%s (error %d)", prefix, err);
    t->state = IR_TRANSPORT_ERROR;
}

/* Winsock needs one start call for each process before any socket call, and one stop call at the end.
   This counter holds that pair against several links in one process. The frontend and the test tool are
   each one thread, thus this counter needs no lock. */
static int wsa_refs = 0;

static int wsa_acquire(ir_transport_t *t) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    if (c->wsa_held) {
        return 1;
    }
    if (wsa_refs == 0) {
        WSADATA data;
        int err = WSAStartup(MAKEWORD(2, 2), &data);
        if (err != 0) {
            set_error(t, "Couldn't start Winsock", err);
            return 0;
        }
    }
    wsa_refs++;
    c->wsa_held = 1;
    return 1;
}

static void wsa_release(ir_transport_tcp_t *c) {
    if (!c->wsa_held) {
        return;
    }
    c->wsa_held = 0;
    if (--wsa_refs == 0) {
        WSACleanup();
    }
}

static void close_socket(SOCKET *s) {
    if (*s != INVALID_SOCKET) {
        closesocket(*s);
        *s = INVALID_SOCKET;
    }
}

static int set_non_blocking(SOCKET s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}

/* Nagle would hold a small message until an earlier one is acknowledged, and it would join several into
   one segment. A wire message is 24 bytes, and a group of them must go at the rate that the sender makes
   them. A delay of tens of milliseconds on each message would consume the playout buffer. */
static void set_no_delay(SOCKET s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, (int)sizeof(on));
}

/* Splits "host:port", "[v6address]:port", ":port", or a bare port.
   A name with no port uses IR_TRANSPORT_DEFAULT_TCP_PORT. An empty host means "each interface of this
   machine" on the host side. */
static void split_address(const char *address, char *host, size_t host_size, char *port, size_t port_size) {
    const char *colon;
    host[0] = 0;
    snprintf(port, port_size, "%s", IR_TRANSPORT_DEFAULT_TCP_PORT);
    if (address == NULL || address[0] == 0) {
        return;
    }
    if (address[0] == '[') {
        const char *end = strchr(address, ']');
        if (end != NULL) {
            size_t len = (size_t)(end - address - 1);
            if (len >= host_size) {
                len = host_size - 1;
            }
            memcpy(host, address + 1, len);
            host[len] = 0;
            if (end[1] == ':' && end[2] != 0) {
                snprintf(port, port_size, "%s", end + 2);
            }
            return;
        }
    }
    /* An address with more than one colon and no brackets is a bare IPv6 address, thus it holds no port. */
    colon = strchr(address, ':');
    if (colon == NULL) {
        int digits_only = 1;
        const char *p;
        for (p = address; *p != 0; p++) {
            if (*p < '0' || *p > '9') {
                digits_only = 0;
                break;
            }
        }
        if (digits_only) {
            snprintf(port, port_size, "%s", address);
        } else {
            snprintf(host, host_size, "%s", address);
        }
        return;
    }
    if (strchr(colon + 1, ':') != NULL) {
        snprintf(host, host_size, "%s", address);
        return;
    }
    {
        size_t len = (size_t)(colon - address);
        if (len >= host_size) {
            len = host_size - 1;
        }
        memcpy(host, address, len);
        host[len] = 0;
        if (colon[1] != 0) {
            snprintf(port, port_size, "%s", colon + 1);
        }
    }
}

static void tcp_close(ir_transport_t *t) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    close_socket(&c->fd);
    close_socket(&c->listen_fd);
    c->read_fill = 0;
    c->read_head = 0;
    c->connect_in_progress = 0;
    c->peer_addr_len = 0;
    wsa_release(c);
    t->state = IR_TRANSPORT_IDLE;
    t->error[0] = 0;
}

static void on_connected(ir_transport_t *t) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    set_no_delay(c->fd);
    close_socket(&c->listen_fd); /* point-to-point: no second peer can join */
    c->read_fill = 0;
    c->read_head = 0;
    c->connect_in_progress = 0;
    t->state = IR_TRANSPORT_CONNECTED;
}

static int tcp_host(ir_transport_t *t, const char *address) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    char host[128], port[32];
    struct addrinfo hints, *result = NULL, *it;
    int err;

    tcp_close(t); /* idempotent: clears any previous attempt first */
    if (!wsa_acquire(t)) {
        return 0;
    }
    snprintf(c->name, sizeof(c->name), "%s", address);
    split_address(address, host, sizeof(host), port, sizeof(port));

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    err = getaddrinfo(host[0] != 0 ? host : NULL, port, &hints, &result);
    if (err != 0) {
        set_error(t, "Couldn't resolve the listen address", err);
        return 0;
    }
    for (it = result; it != NULL; it = it->ai_next) {
        SOCKET s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        int reuse = 1;
        if (s == INVALID_SOCKET) {
            continue;
        }
        /* A host that ends a session and hosts again finds its own port in the TIME_WAIT state of TCP.
           Without this option that second attempt fails, and the user sees an error for a port that
           nothing uses. */
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, (int)sizeof(reuse));
        if (bind(s, it->ai_addr, (int)it->ai_addrlen) == 0 && listen(s, 1) == 0 && set_non_blocking(s)) {
            c->listen_fd = s;
            break;
        }
        closesocket(s);
    }
    freeaddrinfo(result);
    if (c->listen_fd == INVALID_SOCKET) {
        set_error(t, "Couldn't listen", WSAGetLastError());
        return 0;
    }
    t->state = IR_TRANSPORT_LISTENING;
    return 1;
}

/* Starts one connection attempt against the stored address. A refused attempt is not a fault: the host
   can start after the client. tcp_poll calls this function again at each frame, in the manner of the
   named pipe. */
static void try_client_connect(ir_transport_t *t) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    if (c->fd != INVALID_SOCKET) {
        return;
    }
    c->fd = socket(c->peer_addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (c->fd == INVALID_SOCKET) {
        set_error(t, "Couldn't make a socket", WSAGetLastError());
        return;
    }
    if (!set_non_blocking(c->fd)) {
        set_error(t, "Couldn't set the socket mode", WSAGetLastError());
        close_socket(&c->fd);
        return;
    }
    if (connect(c->fd, (const struct sockaddr *)&c->peer_addr, c->peer_addr_len) == 0) {
        on_connected(t); /* a loopback connection can be immediately successful */
        return;
    }
    if (WSAGetLastError() == WSAEWOULDBLOCK) {
        c->connect_in_progress = 1;
        return;
    }
    close_socket(&c->fd); /* no host at this address yet: a later poll call tries again */
}

/* Tests a connection attempt that is in progress. A socket that can accept a write is connected, and
   SO_ERROR holds the result. */
static void poll_client_connect(ir_transport_t *t) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    fd_set write_set, except_set;
    struct timeval zero;
    int err = 0;
    int err_len = (int)sizeof(err);

    FD_ZERO(&write_set);
    FD_ZERO(&except_set);
    FD_SET(c->fd, &write_set);
    FD_SET(c->fd, &except_set);
    zero.tv_sec = 0;
    zero.tv_usec = 0;
    if (select(0, NULL, &write_set, &except_set, &zero) <= 0) {
        return; /* still in progress */
    }
    if (FD_ISSET(c->fd, &except_set)) {
        c->connect_in_progress = 0;
        close_socket(&c->fd);
        return; /* refused: try again at the next poll call */
    }
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &err_len) != 0 || err != 0) {
        c->connect_in_progress = 0;
        close_socket(&c->fd);
        return;
    }
    c->connect_in_progress = 0;
    on_connected(t);
}

static int tcp_connect(ir_transport_t *t, const char *address) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    char host[128], port[32];
    struct addrinfo hints, *result = NULL;
    int err;

    tcp_close(t);
    if (!wsa_acquire(t)) {
        return 0;
    }
    snprintf(c->name, sizeof(c->name), "%s", address);
    split_address(address, host, sizeof(host), port, sizeof(port));

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    err = getaddrinfo(host[0] != 0 ? host : "localhost", port, &hints, &result);
    if (err != 0 || result == NULL) {
        set_error(t, "Couldn't resolve the address of the peer", err);
        return 0;
    }
    memcpy(&c->peer_addr, result->ai_addr, result->ai_addrlen);
    c->peer_addr_len = (int)result->ai_addrlen;
    freeaddrinfo(result);

    t->state = IR_TRANSPORT_CONNECTING;
    try_client_connect(t);
    return 1;
}

static void tcp_poll(ir_transport_t *t) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    switch (t->state) {
    case IR_TRANSPORT_LISTENING: {
        SOCKET s = accept(c->listen_fd, NULL, NULL);
        if (s == INVALID_SOCKET) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                set_error(t, "Listen failed", err);
            }
            return;
        }
        if (!set_non_blocking(s)) {
            set_error(t, "Couldn't set the socket mode", WSAGetLastError());
            closesocket(s);
            return;
        }
        c->fd = s;
        on_connected(t);
        break;
    }
    case IR_TRANSPORT_CONNECTING:
        if (c->connect_in_progress) {
            poll_client_connect(t);
        } else {
            try_client_connect(t);
        }
        break;
    default:
        break;
    }
}

static size_t tcp_recv(ir_transport_t *t, void *data, size_t size) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    size_t available;
    if (t->state != IR_TRANSPORT_CONNECTED || size == 0) {
        return 0;
    }
    if (c->read_head == c->read_fill) {
        /* One recv call for each buffer, and not one for each wire message. ir_link.c asks for 24 bytes at
           a time, and a call into the operating system for each of those is the largest cost in this
           path. */
        int n = recv(c->fd, (char *)c->read_buf, (int)sizeof(c->read_buf), 0);
        if (n == 0) {
            set_error(t, "Peer disconnected", 0); /* an orderly close by the peer */
            return 0;
        }
        if (n < 0) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                set_error(t, "Peer disconnected", err);
            }
            return 0;
        }
        c->read_fill = (uint32_t)n;
        c->read_head = 0;
    }
    available = c->read_fill - c->read_head;
    if (available > size) {
        available = size;
    }
    memcpy(data, c->read_buf + c->read_head, available);
    c->read_head += (uint32_t)available;
    return available;
}

static size_t tcp_send(ir_transport_t *t, const void *data, size_t size) {
    ir_transport_tcp_t *c = &t->impl.tcp;
    int n;
    if (t->state != IR_TRANSPORT_CONNECTED || size == 0) {
        return 0;
    }
    n = send(c->fd, (const char *)data, (int)size, 0);
    if (n < 0) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            set_error(t, "Peer disconnected", err);
        }
        return 0; /* the send buffer is full: the caller keeps its bytes */
    }
    return (size_t)n;
}

static const ir_transport_vtable_t tcp_vtable = {
    tcp_host,
    tcp_connect,
    tcp_poll,
    tcp_send,
    tcp_recv,
    tcp_close,
};

void ir_transport_init_tcp(ir_transport_t *t) {
    ZeroMemory(t, sizeof(*t));
    t->vt = &tcp_vtable;
    t->state = IR_TRANSPORT_IDLE;
    t->impl.tcp.fd = INVALID_SOCKET;
    t->impl.tcp.listen_fd = INVALID_SOCKET;
}
