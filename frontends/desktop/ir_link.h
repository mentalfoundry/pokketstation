#ifndef POKKETSTATION_IR_LINK_H
#define POKKETSTATION_IR_LINK_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>

#include "psemu/psemu.h"

/* Local two-instance IR link, over a Windows named pipe.
   Connects this instance's psemu_ir_pop_tx_edge/psemu_ir_push_rx_edge (see psemu.h) to another running
   pokketstation.exe on the same machine, so two independent emulator instances can exchange real IR signals -
   the same way two physical PocketStation units would, held up to each other.

   One instance hosts (ir_link_host), the other connects (ir_link_connect) to the same well-known pipe name.
   Once connected, ir_link_pump - called once per frame, right after psemu_run - drains this instance's
   locally-produced TX edges onto the pipe, and feeds any edges arriving from the other instance into this
   instance's RX queue.

   Edges are relayed as absolute host wall-clock microseconds (GetSystemTimePreciseAsFileTime), not raw core
   cycle counts: the two instances' IR clocks are never synchronized with each other (real IR hardware has no
   shared clock either - see ir.h), but both processes run on the same machine and can independently read the
   same wall clock with zero coordination. Converting a wall-clock timestamp into this instance's own local IR
   timeline, or back out, only ever needs this instance's own current (wall_us - core_us) offset, recomputed
   every pump call.

   All I/O is overlapped (asynchronous): ir_link_pump only ever polls already-issued operations and never
   blocks, matching the desktop frontend's existing single-threaded, no-locking main loop. */

typedef enum {
    IR_LINK_IDLE,
    IR_LINK_HOSTING,    /* server: pipe created, waiting for a peer to connect */
    IR_LINK_CONNECTING, /* client: retrying CreateFileA until a host is listening */
    IR_LINK_CONNECTED,
    IR_LINK_ERROR
} ir_link_state_t;

#define IR_LINK_DEFAULT_PIPE_NAME "\\\\.\\pipe\\pokketstation_ir_link"
#define IR_LINK_WRITE_QUEUE_CAPACITY 64u

/* ir_link_pump runs once per rendered frame (~31ms), after that frame's psemu_run already finished - so an
   edge from the peer's whole last frame is always somewhat stale (transit + one frame of batching on each
   side) by the time it is actually pushed into this instance's RX queue. Scheduling it for immediate/"as soon
   as possible" delivery would mean its timestamp is already <= this instance's current clock the moment it's
   pushed - ir_tick's due-edge loop would then release the *entire* batch in one shot, on the very next CPU
   step, instead of one at a time as this instance's own clock naturally advances through the frame. That
   destroys the relative pulse-width spacing between edges, which is exactly what the receiving side's IR
   protocol needs to decode a transfer.

   The fix is a standard jitter/playout buffer: every incoming edge is scheduled this many microseconds into
   this instance's own future instead, comfortably past the worst-case round trip (roughly two frames: draining
   at the end of the sender's frame, delivery at the end of this instance's next frame, plus OS scheduling
   jitter). Since every edge in a batch gets the same constant shift, their original relative spacing - and
   therefore the pulse widths the receiver needs to measure - is preserved exactly; only the batch's overall
   arrival is delayed, which is imperceptible for a turn-based IR exchange. */
#define IR_LINK_PLAYOUT_DELAY_US 100000ull

/* One 16-byte message per edge (kind=IR_WIRE_KIND_EDGE), or a connect-time handshake (kind=IR_WIRE_KIND_HELLO,
   not fed into psemu_ir_push_rx_edge - it exists only so a magic/version mismatch is caught immediately on
   connect, instead of only whenever real IR traffic eventually happens). magic/version are checked on every
   received message; a mismatch tears the link down with a clear status message, since it means the two
   instances are running incompatible builds. */
#define IR_WIRE_MAGIC 0x52494B50u /* 'PKIR' */
#define IR_WIRE_VERSION 1u
#define IR_WIRE_KIND_EDGE 0u
#define IR_WIRE_KIND_HELLO 1u

#pragma pack(push, 1)
typedef struct ir_wire_message {
    uint32_t magic;
    uint16_t version;
    uint8_t level;
    uint8_t kind;
    uint64_t timestamp_us; /* absolute host wall-clock microseconds; meaningless for a HELLO message */
} ir_wire_message_t;
#pragma pack(pop)

typedef struct ir_link {
    ir_link_state_t state;
    int is_server;
    HANDLE pipe;
    HANDLE ev_connect;
    HANDLE ev_read;
    HANDLE ev_write;
    OVERLAPPED ov_connect;
    OVERLAPPED ov_read;
    OVERLAPPED ov_write;

    ir_wire_message_t read_msg; /* target buffer for the one always-outstanding overlapped read */
    int read_pending;

    /* Outgoing edges, produced faster than one per pipe round trip can drain in the worst case (a burst of
       CPU-bit-banged transitions in one frame). A full queue drops the newest edge rather than block or grow
       unbounded - see enqueue_write. */
    ir_wire_message_t write_queue[IR_LINK_WRITE_QUEUE_CAPACITY];
    uint32_t write_head;
    uint32_t write_count;
    int write_pending;

    char pipe_name[256];
    char status[128]; /* human-readable, for ir_link_status_text - e.g. a window-title suffix */
} ir_link_t;

void ir_link_init(ir_link_t *link);

/* Creates the named pipe and starts listening for a peer. Returns 1 on success (including "still waiting for a
   peer", which is not an error), 0 if the pipe itself could not be created at all. */
int ir_link_host(ir_link_t *link, const char *pipe_name);

/* Starts trying to connect to a peer already hosting at `pipe_name`. Always returns 1; ir_link_pump retries the
   connection attempt on subsequent calls until a host is listening. */
int ir_link_connect(ir_link_t *link, const char *pipe_name);

/* Tears down the link, if any, and returns to IR_LINK_IDLE. Safe to call at any time, including when already
   idle. Callers should call this before psemu_reset/psemu_load_state (see main.c) - core wipes ir_t's clock and
   any queued edges on either of those, so a link left connected across one would silently desync. */
void ir_link_disconnect(ir_link_t *link);

/* Call once per frame, after psemu_run. No-op while idle or errored. */
void ir_link_pump(ir_link_t *link, psemu_t *ps);

const char *ir_link_status_text(const ir_link_t *link);

/* Nonzero while hosting, connecting, or connected - i.e. whenever ir_link_disconnect would actually do
   something other than confirm there was nothing to do. */
int ir_link_is_active(const ir_link_t *link);

#endif
