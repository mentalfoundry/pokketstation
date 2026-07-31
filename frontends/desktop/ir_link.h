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

   One instance hosts through ir_link_host. The other connects through ir_link_connect.
   Both use the same well-known pipe name.
   Once connected, ir_link_pump runs once per frame, right after psemu_run.
   It drains this instance's locally-produced TX edges onto the pipe.
   It also feeds edges that arrive from the other instance into this instance's RX queue.

   Edges relay as absolute host wall-clock microseconds, from GetSystemTimePreciseAsFileTime.
   They do not relay as raw core cycle counts.
   The two instances' IR clocks are never synchronized with each other. Real IR hardware shares no clock
   either. See ir.h.
   Both processes do run on the same machine, and each can read the same wall clock with no coordination.
   To convert a wall-clock timestamp into this instance's own local IR timeline, or back out again, needs only
   this instance's own current offset. That offset is wall_us minus core_us, recomputed on every pump call.

   All I/O is overlapped, that is, asynchronous.
   ir_link_pump only polls operations that are already issued. It never blocks.
   The desktop frontend's main loop is single-threaded and uses no locking, and this matches it. */

typedef enum {
    IR_LINK_IDLE,
    IR_LINK_HOSTING,    /* server: pipe created, waiting for a peer to connect */
    IR_LINK_CONNECTING, /* client: retrying CreateFileA until a host is listening */
    IR_LINK_CONNECTED,
    IR_LINK_ERROR
} ir_link_state_t;

#define IR_LINK_DEFAULT_PIPE_NAME "\\\\.\\pipe\\pokketstation_ir_link"

/* One real IR message is far bigger than a "burst of transitions in one frame" suggests: a Chocobo World
   transfer measured 658 edges, and the reply brings the total near 1000. At 64 this queue overflowed part
   way through every message and silently dropped the rest, which is unrecoverable for a bit-banged protocol
   where each pulse width carries a bit. Sized to hold several whole messages so a slow peer causes delay
   rather than corruption. This mirrors IR_EDGE_QUEUE_CAPACITY in core/src/ir.h, which had the identical
   problem at the identical size. */
#define IR_LINK_WRITE_QUEUE_CAPACITY 4096u

/* ir_link_pump runs once per rendered frame, about every 31ms, after that frame's psemu_run finished.
   An edge from the peer's whole last frame is therefore always somewhat stale when it reaches this instance's
   RX queue. The delay is the transit time plus one frame of batching on each side.

   Immediate delivery would make that staleness a problem.
   The edge's timestamp would already be at or before this instance's current clock the moment it arrives.
   ir_tick's due-edge loop would then release the whole batch in one shot, on the very next CPU step.
   It would not release the edges one at a time, as this instance's own clock advances through the frame.
   That destroys the relative spacing between edges. The receiving side's IR protocol needs that spacing to
   decode a transfer.

   A standard jitter buffer, also called a playout buffer, avoids this.
   It schedules every incoming edge this many microseconds into this instance's own future.
   That delay is comfortably longer than the worst-case round trip, which is about two frames.
   The sender drains at the end of its frame, delivery happens at the end of this instance's next frame, and
   OS scheduling adds more jitter.
   Every edge in a batch gets the same constant shift, so their original relative spacing survives exactly.
   The pulse widths the receiver measures therefore survive too.
   Only the arrival of the whole batch is later, and a turn-based IR exchange does not notice that. */
#define IR_LINK_PLAYOUT_DELAY_US 100000ull

/* One 16-byte message per edge, with kind=IR_WIRE_KIND_EDGE.
   A connect-time handshake uses kind=IR_WIRE_KIND_HELLO instead.
   A HELLO message never reaches psemu_ir_push_rx_edge.
   It exists only to catch a magic or version mismatch at connect time, rather than whenever real IR traffic
   first happens.
   Every received message carries magic and version, and this checks both.
   A mismatch means the two instances run incompatible builds, so it tears the link down and reports why. */
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

    /* Outgoing edges. In the worst case the CPU bit-bangs a burst of transitions in one frame, faster than
       one pipe round trip can drain.
       A full queue drops the newest edge. It does not block, and it does not grow without limit.
       See enqueue_write. */
    ir_wire_message_t write_queue[IR_LINK_WRITE_QUEUE_CAPACITY];
    uint32_t write_head;
    uint32_t write_count;
    int write_pending;

    /* Wall-clock-to-core-clock offset, latched once when the link connects rather than recomputed per use.
       An edge's timestamp says when it was produced, so converting it with an offset sampled later mixes two
       different moments. Emulated time and wall time never advance at exactly the same rate, so a
       recomputed offset differs from frame to frame, and edges produced in different frames then get
       shifted by different amounts. That distorts the spacing between edges, and in this protocol the
       spacing is the data: pulse width is what encodes each bit. Latching keeps every conversion on one
       consistent mapping, so relative spacing survives exactly and only a uniform shift remains, which is
       what IR_LINK_PLAYOUT_DELAY_US exists to absorb.
       Confirmed by measurement: with the offset recomputed per call, a real app's transfer failed over this
       transport while succeeding through an in-process relay; latching it makes the same transfer complete
       in both directions. See frontends/desktop/ir_link_selftest.c's transfer mode. */
    int64_t wall_minus_core_us;
    int clock_offset_latched;

    char pipe_name[256];
    char status[128]; /* human-readable text for ir_link_status_text, such as a window-title suffix */
} ir_link_t;

void ir_link_init(ir_link_t *link);

/* Creates the named pipe, then starts to listen for a peer.
   It returns 1 on success. A state of "still waiting for a peer" counts as success, not as an error.
   It returns 0 if it could not create the pipe at all. */
int ir_link_host(ir_link_t *link, const char *pipe_name);

/* Starts to connect to a peer that already hosts at `pipe_name`.
   It always returns 1. ir_link_pump retries the attempt on later calls, until a host listens. */
int ir_link_connect(ir_link_t *link, const char *pipe_name);

/* Tears down the link, if there is one, and returns to IR_LINK_IDLE.
   It is safe to call at any time, including when the link is already idle.
   Call it before psemu_reset or psemu_load_state. See main.c.
   Core wipes ir_t's clock and every queued edge on both of those calls.
   A link left connected across one of them would silently lose sync with the peer. */
void ir_link_disconnect(ir_link_t *link);

/* Call once per frame, after psemu_run. No-op while idle or errored. */
void ir_link_pump(ir_link_t *link, psemu_t *ps);

const char *ir_link_status_text(const ir_link_t *link);

/* Nonzero while this hosts, connects, or is connected.
   That is, nonzero whenever ir_link_disconnect has real work to do. */
int ir_link_is_active(const ir_link_t *link);

#endif
