#ifndef POKKETSTATION_IR_LINK_H
#define POKKETSTATION_IR_LINK_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>

#include "psemu/psemu.h"

/* A local IR link between two instances, on a Windows named pipe.
   It connects psemu_ir_pop_tx_edge and psemu_ir_push_rx_edge of this instance (see psemu.h) to a
   different pokketstation.exe process on the same machine. Thus two independent emulator instances can
   exchange real IR signals. This is the same operation as two physical PocketStation units that a user
   holds together.

   One instance hosts the link with ir_link_host. The other instance connects with ir_link_connect.
   Both use the same known pipe name.
   After the connection, ir_link_pump executes one time for each frame, immediately after psemu_run.
   It moves the local TX edges of this instance onto the pipe.
   It also sends the edges from the other instance into the RX queue of this instance.

   The edges relay as absolute host wall-clock microseconds, from GetSystemTimePreciseAsFileTime. They
   do not relay as raw core cycle counts.
   Nothing synchronizes the two IR clocks of the instances. Real IR hardware also shares no clock. See
   ir.h.
   Both processes operate on the same machine, and each process can read the same wall clock with no
   coordination. A conversion of a wall-clock timestamp into the local IR timeline of this instance,
   or back, needs only the current offset of this instance. That offset is wall_us minus core_us, and
   each pump call calculates it again.

   All I/O is overlapped, which means asynchronous.
   ir_link_pump only tests operations that are already in progress. It never blocks.
   The main loop of the desktop frontend has one thread and uses no locks, and this design agrees with
   that loop. */

typedef enum {
    IR_LINK_IDLE,
    IR_LINK_HOSTING,    /* server: the pipe exists, and this instance waits for a peer to connect */
    IR_LINK_CONNECTING, /* client: this instance calls CreateFileA again until a host listens */
    IR_LINK_CONNECTED,
    IR_LINK_ERROR
} ir_link_state_t;

#define IR_LINK_DEFAULT_PIPE_NAME "\\\\.\\pipe\\pokketstation_ir_link"

/* One real IR message is much larger than the term "a group of transitions in one frame" suggests. A
   measurement of one transfer gave 658 edges, and the reply makes the total near 1000. At a capacity
   of 64, this queue overflowed during each message and discarded the remainder with no error. That
   loss is unrecoverable for a bit-banged protocol, where each pulse width holds a bit. This capacity
   holds several full messages. Thus a slow peer causes a delay, and not corruption. This value agrees
   with IR_EDGE_QUEUE_CAPACITY in core/src/ir.h, which had the same problem at the same size. */
#define IR_LINK_WRITE_QUEUE_CAPACITY 4096u

/* ir_link_pump executes one time for each rendered frame, at intervals of approximately 31ms, after
   the psemu_run call of that frame. Thus an edge from the last full frame of the peer is always old
   when it gets to the RX queue of this instance. The delay is the transit time, and one frame of
   batching on each side.

   Immediate delivery makes that age a problem.
   At arrival, the timestamp of the edge is already at or before the current clock of this instance.
   The due-edge loop in ir_tick then releases the full group at the next CPU step. It does not release
   the edges one at a time, while the local clock advances through the frame. That behavior destroys
   the relative space between the edges. The IR protocol of the receive side needs that space to decode
   a transfer.

   A standard jitter buffer, also called a playout buffer, prevents this problem.
   It schedules each incoming edge this number of microseconds into the future of this instance.
   That delay is longer than the worst-case round trip, which is approximately two frames. The sender
   drains its queue at the end of its frame, delivery occurs at the end of the next frame of this
   instance, and the operating system scheduler adds more variation.
   Each edge in a group gets the same constant shift. Thus the original relative space between them
   stays exact, and the pulse widths that the receiver measures also stay exact. Only the arrival of
   the full group is later, and a turn-based IR exchange does not detect that delay.

   The size of this value: the delay must cover the transit time, the batching, and the clock drift
   over one full message. This code holds the offset constant for the duration of a message (see
   IR_LINK_OFFSET_RELATCH_IDLE_US). A measured transfer between two processes used approximately 85ms
   of margin in this way. Thus a value of 100ms left only approximately 16ms. A value of 250ms keeps a
   good margin on a slower or busier machine. The extra latency has no cost here, because a turn-based
   IR exchange has no interactive time limit. */
#define IR_LINK_PLAYOUT_DELAY_US 250000ull

/* The time that the link must have no edge traffic before this code can latch the wall-to-core offset
   again.

   The code cannot hold the offset permanently, and it cannot sample the offset at each use. Each
   instance advances its emulated clock by exactly one frame of cycles for each rendered frame. But a
   real frame takes more wall-clock time than that, and the extra time is different between two
   processes, because of different startup costs, render loads, and scheduling. Thus their core clocks
   drift apart with no limit. If the code holds the offset permanently, that drift moves each arriving
   edge outside the playout buffer. A measurement between two real processes showed this: one side
   received each edge approximately 200ms in its own past, released the full group at one time, and
   decoded nothing. If the code samples the offset at each use, the offset changes between frames and
   destroys the space that encodes each bit.

   A new latch after a quiet period gives both necessary properties. In one message the offset is
   constant, thus the space between edges is exact. Between messages, the offset corrects the drift
   between the two clocks. A turn-based IR exchange always has gaps that are much longer than this
   value, and this value is less than the shortest gap between a message and its reply. */
#define IR_LINK_OFFSET_RELATCH_IDLE_US 250000ull

/* One message of 16 bytes for each edge, with kind = IR_WIRE_KIND_EDGE.
   A handshake at connect time uses kind = IR_WIRE_KIND_HELLO.
   A HELLO message never gets to psemu_ir_push_rx_edge.
   Its only function is to find a magic-number or version difference at connect time. Without it, the
   code finds the difference only at the first real IR traffic.
   Each received message contains the magic number and the version, and this code tests both.
   A difference shows that the two instances execute incompatible builds. Thus this code closes the
   link and reports the reason. */
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
    uint64_t timestamp_us; /* absolute host wall-clock microseconds. This field has no meaning in a HELLO message. */
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

    ir_wire_message_t read_msg; /* the target buffer for the one overlapped read that is always in progress */
    int read_pending;

    /* The outgoing edges. In the worst condition, the CPU makes a group of transitions in one frame,
       faster than one pipe round trip can drain the queue.
       A full queue discards the newest edge. It does not block, and it does not increase without a
       limit. See enqueue_write. */
    ir_wire_message_t write_queue[IR_LINK_WRITE_QUEUE_CAPACITY];
    uint32_t write_head;
    uint32_t write_count;
    int write_pending;

    /* The offset from the wall clock to the core clock. This code latches the offset one time, when
       the link connects. It does not calculate the offset at each use.
       The timestamp of an edge gives the time of its creation. Thus a conversion with an offset from
       a later time mixes two different moments. Emulated time and wall time never advance at exactly
       the same rate. Thus a new calculation gives a different offset at each frame, and edges from
       different frames then get different shifts. That difference changes the space between the
       edges. In this protocol the space is the data: the pulse width encodes each bit. One latched
       offset keeps each conversion on one consistent mapping. Thus the relative space stays exact,
       and only one uniform shift remains. IR_LINK_PLAYOUT_DELAY_US absorbs that shift.
       A measurement confirms this: with a new offset calculation at each call, a transfer from a real
       app failed on this transport, but the same transfer was successful through an in-process relay.
       With a latched offset, the same transfer completes in both directions. See the transfer mode of
       frontends/desktop/ir_link_selftest.c. */
    int64_t wall_minus_core_us;
    int clock_offset_latched;
    uint64_t last_edge_wall_us; /* the time of the last edge on this link, for the idle test above */

    /* Simple counters. They give diagnostic data for a link that connects but carries no useful data.
       They are inexpensive, thus they operate always. They are the only method to tell "the peer sent
       nothing" from "the peer sent the data and this instance discarded it".
       dropped_tx counts the edges that enqueue_write discarded because the queue was full. */
    unsigned long edges_sent;
    unsigned long edges_received;
    unsigned long dropped_tx;
    /* While this flag is set, the connected status line contains the counters above. Thus the
       counters get to the window title. This code always keeps the counters; only their display is
       optional. The flag is off until the frontend sets it (ir_link_diagnostics in settings.cfg),
       because the numbers change at each frame and have no meaning to a person who only uses the
       link. */
    int show_diagnostics;
    /* The time by which each arriving edge is scheduled ahead of the local IR clock of this instance.
       This is the margin that the playout buffer delivers, and not the margin that it plans. When
       this value gets to zero, an edge is already due at its arrival. Thus ir_tick releases it
       immediately, together with each other late edge. The space in the group then collapses, and a
       bit-banged message stops decoding. */
    int64_t min_lead_us;
    int64_t max_lead_us;
    unsigned long late_edges;

    char pipe_name[256];
    char status[128]; /* human-readable text for ir_link_status_text, for example a window-title suffix */
} ir_link_t;

void ir_link_init(ir_link_t *link);

/* Makes the named pipe, and then listens for a peer.
   It returns 1 if the operation is successful. The state "this instance still waits for a peer" is
   success, and not an error.
   It returns 0 if it cannot make the pipe. */
int ir_link_host(ir_link_t *link, const char *pipe_name);

/* Starts a connection to a peer that already hosts a link at `pipe_name`.
   It always returns 1. ir_link_pump tries the connection again at later calls, until a host
   listens. */
int ir_link_connect(ir_link_t *link, const char *pipe_name);

/* Closes the link, if a link is present, and returns to IR_LINK_IDLE.
   You can call this function at any time. This includes the time when the link is already idle.
   Call it before psemu_reset or psemu_load_state. See main.c.
   Both of those calls clear the clock of ir_t and each edge in the queues.
   A link that stays connected through one of those calls loses synchronization with the peer, and
   gives no error. */
void ir_link_disconnect(ir_link_t *link);

/* Call this function one time for each frame, after psemu_run. It does nothing while the link is idle
   or has an error. */
void ir_link_pump(ir_link_t *link, psemu_t *ps);

const char *ir_link_status_text(const ir_link_t *link);

/* Returns a nonzero value while this link hosts, connects, or is connected.
   Thus it returns a nonzero value when ir_link_disconnect has work to do. */
int ir_link_is_active(const ir_link_t *link);

#endif
