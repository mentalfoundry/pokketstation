#ifndef POKKETSTATION_IR_TRANSPORT_H
#define POKKETSTATION_IR_TRANSPORT_H

#define WIN32_LEAN_AND_MEAN
/* Before windows.h. WIN32_LEAN_AND_MEAN holds the version 1 winsock.h out of windows.h, and this order
   keeps the version 2 headers the only ones that this file uses. The two versions do not agree on the
   same names. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stddef.h>
#include <stdint.h>

/* The byte transport below an IR link.

   ir_link.c holds the timing of the link: the playout buffer, the wall-to-core offset, the wire
   messages, and the counters. This file holds only the movement of bytes between two endpoints. The
   timing is the same for each transport. The byte movement is not, thus it is separate.

   A transport is a byte stream, and it keeps no message boundary. A send of 16 bytes can arrive as
   two reads of 8 bytes. ir_link.c collects a full wire message before it uses one. A socket gives a
   byte stream, thus this interface uses that same model for a named pipe also. The named pipe is a
   byte pipe (PIPE_TYPE_BYTE) for this reason.

   Each operation is non-blocking. ir_transport_poll, ir_transport_send, and ir_transport_recv test
   the operations that are already in progress, and they return immediately. The main loop of the
   desktop frontend has one thread and uses no locks, and this design agrees with that loop.

   The transport owns the connection sequence. It listens, it tries a connection again until a host
   answers, and it gives the result in `state`. ir_link.c reads that state at each pump call. */

typedef enum {
    IR_TRANSPORT_IDLE,
    IR_TRANSPORT_LISTENING,  /* server: the endpoint exists, and it waits for a peer */
    IR_TRANSPORT_CONNECTING, /* client: the endpoint tries to reach a host */
    IR_TRANSPORT_CONNECTED,
    IR_TRANSPORT_ERROR
} ir_transport_state_t;

/* The staging buffers between the caller and the operating system.

   One overlapped operation needs a buffer that stays at one address until that operation completes.
   The ring buffer of ir_link.c cannot give such a buffer, because its head moves. Thus the transport
   holds its own buffer for each direction.

   The read buffer holds 256 wire messages. One real IR message makes approximately 658 edges, thus a
   full message needs approximately three read operations. ir_link.c calls ir_transport_recv in a loop
   until the transport gives no more data, and the loop starts a new read each time that the buffer
   empties. Thus this size sets the number of system calls, and not the throughput. */
#define IR_TRANSPORT_READ_BUFFER_SIZE 4096u
#define IR_TRANSPORT_WRITE_BUFFER_SIZE 4096u

typedef struct ir_transport_pipe {
    HANDLE pipe;
    HANDLE ev_connect;
    HANDLE ev_read;
    HANDLE ev_write;
    OVERLAPPED ov_connect;
    OVERLAPPED ov_read;
    OVERLAPPED ov_write;
    int is_server;
    int read_pending;
    int write_pending;

    uint8_t read_buf[IR_TRANSPORT_READ_BUFFER_SIZE];
    uint32_t read_fill; /* the number of bytes that the last read gave */
    uint32_t read_head; /* the number of those bytes that the caller has already taken */

    uint8_t write_buf[IR_TRANSPORT_WRITE_BUFFER_SIZE];
    uint32_t write_len; /* the number of bytes in the write that is in progress */

    char name[256];
} ir_transport_pipe_t;

/* The port that an address with no port uses. It has no assignment from any registry. */
#define IR_TRANSPORT_DEFAULT_TCP_PORT "27411"

/* The number of resolved addresses of the peer that a client keeps. A name with one address in each
   family needs two. This value holds more than that, and it stays small because a point-to-point link
   needs one reachable address and not a full list. */
#define IR_TRANSPORT_MAX_PEER_ADDRESSES 4u

/* The TCP transport, for two machines.
   An address is "host:port", "[v6address]:port", ":port", or a bare port. A host side with no host part
   listens on each interface of the machine.

   This transport uses non-blocking sockets, and not overlapped I/O. The model of this interface is "test
   what is ready and return immediately", and a non-blocking socket gives that model directly. Overlapped
   I/O would add a completion object for each operation and give nothing more here. */
typedef struct ir_transport_tcp {
    SOCKET fd;        /* the connected socket */
    SOCKET listen_fd; /* the listening socket. The host side closes it when a peer arrives. */
    int wsa_held;
    /* The addresses of the peer, resolved one time by ir_transport_connect. A connection that is refused
       tries again at each poll call, in the manner of the named pipe, and a stored address keeps that
       repetition off the name resolver.
       A name can give an address in each family, and only one of them can be reachable. Thus this code
       keeps several candidates and moves to the next one at each attempt that fails. One stored address
       would try one family for the life of the link. */
    struct sockaddr_storage peer_addr[IR_TRANSPORT_MAX_PEER_ADDRESSES];
    int peer_addr_len[IR_TRANSPORT_MAX_PEER_ADDRESSES];
    int peer_addr_count;
    int peer_addr_index;
    int connect_in_progress;

    uint8_t read_buf[IR_TRANSPORT_READ_BUFFER_SIZE];
    uint32_t read_fill;
    uint32_t read_head;

    char name[256];
} ir_transport_tcp_t;

struct ir_transport;
typedef struct ir_transport ir_transport_t;

typedef struct ir_transport_vtable {
    int (*host)(ir_transport_t *t, const char *address);
    int (*connect)(ir_transport_t *t, const char *address);
    void (*poll)(ir_transport_t *t);
    size_t (*send)(ir_transport_t *t, const void *data, size_t size);
    size_t (*recv)(ir_transport_t *t, void *data, size_t size);
    void (*close)(ir_transport_t *t);
} ir_transport_vtable_t;

struct ir_transport {
    const ir_transport_vtable_t *vt;
    ir_transport_state_t state;
    /* The text of the last fault, for example "Couldn't create pipe (error 5)". ir_link.c copies this
       text into its own status line. */
    char error[128];
    /* One member for each transport. A union keeps ir_transport_t a plain value that a caller can
       embed, thus this file needs no allocation. */
    union {
        ir_transport_pipe_t pipe;
        ir_transport_tcp_t tcp;
    } impl;
};

/* Prepares `t` as a local named-pipe transport. The transport is idle after this call.
   Call ir_transport_close before you prepare the same ir_transport_t a second time. */
void ir_transport_init_pipe(ir_transport_t *t);

/* Prepares `t` as a TCP transport, for a peer on a different machine. The transport is idle after this
   call. Call ir_transport_close before you prepare the same ir_transport_t a second time. */
void ir_transport_init_tcp(ir_transport_t *t);

/* Makes the endpoint at `address` and listens for a peer.
   It gives 1 if the operation is successful. The state "this endpoint still waits for a peer" is
   success, and not a fault. It gives 0 if it cannot make the endpoint. */
int ir_transport_host(ir_transport_t *t, const char *address);

/* Starts a connection to a peer at `address`. It always gives 1. ir_transport_poll tries the
   connection again at later calls, until a host answers. */
int ir_transport_connect(ir_transport_t *t, const char *address);

/* Advances the connection sequence. Call it one time for each frame, before the data operations.
   It does nothing while the transport is idle, connected, or in a fault state. */
void ir_transport_poll(ir_transport_t *t);

/* Gives the number of bytes that the transport took, which is 0 to `size`.
   A result of 0 means that an operation is still in progress, or that a fault occurred. Test `state`
   for the difference. The caller keeps the bytes that the transport did not take, and it offers them
   again at a later call. */
size_t ir_transport_send(ir_transport_t *t, const void *data, size_t size);

/* Gives the number of bytes that the transport wrote into `data`, which is 0 to `size`.
   A result that is less than `size` means only that no more data is ready at this time. */
size_t ir_transport_recv(ir_transport_t *t, void *data, size_t size);

/* Closes the connection, if a connection is present, and returns to IR_TRANSPORT_IDLE.
   You can call this function at any time. This includes the time when the transport is already
   idle. */
void ir_transport_close(ir_transport_t *t);

#endif
