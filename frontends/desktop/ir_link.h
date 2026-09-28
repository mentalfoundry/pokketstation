#ifndef POKKETSTATION_IR_LINK_H
#define POKKETSTATION_IR_LINK_H

#include <stdint.h>

#include "ir_transport.h"
#include "psemu/psemu.h"

/* An IR link between two instances.
   It connects psemu_ir_pop_tx_edge and psemu_ir_push_rx_edge of this instance (see psemu.h) to a
   different pokketstation.exe process. Thus two independent emulator instances can exchange real IR
   signals. This is the same operation as two physical PocketStation units that a user holds together.

   This file holds the timing of the link. ir_transport.h holds the movement of bytes. A transport gives
   a byte stream, and this file gives the wire messages, the playout buffer, and the clock conversion
   that operate on it. IR_LINK_DEFAULT_PIPE_NAME selects the local named-pipe transport, for two
   processes on one machine.

   One instance hosts the link with ir_link_host. The other instance connects with ir_link_connect.
   Both use the same address.
   After the connection, ir_link_pump executes one time for each frame, immediately after psemu_run.
   It moves the local TX edges of this instance onto the transport.
   It also sends the edges from the other instance into the RX queue of this instance.

   The edges relay as absolute host wall-clock microseconds, from GetSystemTimePreciseAsFileTime. They
   do not relay as raw core cycle counts.
   Nothing synchronizes the two IR clocks of the instances. Real IR hardware also shares no clock. See
   ir.h.
   Both processes operate on the same machine, and each process can read the same wall clock with no
   coordination. A conversion of a wall-clock timestamp into the local IR timeline of this instance, or
   back, needs only the current offset of this instance. That offset is wall_us minus core_us, and each
   pump call calculates it again.

   ir_link_pump only tests operations that are already in progress. It never blocks.
   The main loop of the desktop frontend has one thread and uses no locks, and this design agrees with
   that loop. */

typedef enum {
    IR_LINK_IDLE,
    IR_LINK_HOSTING,    /* server: the endpoint exists, and this instance waits for a peer to connect */
    IR_LINK_CONNECTING, /* client: this instance tries the connection again until a host listens */
    IR_LINK_SYNCING,    /* the transport is connected, and this instance waits for its first clock sample */
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

/* The wall clock of the peer, minus the wall clock of this instance.

   The transport gives an absolute host wall-clock timestamp for each edge. Two processes on one machine
   read the same wall clock, thus this term is 0 for the named pipe. Two machines do not: their clocks
   can differ by seconds or by minutes. Without a correction, each arriving edge lands that far outside
   the playout buffer.

   ir_link_ping_* below measures this term. The estimate has error, from the variation in the transit
   time and from a path that is not symmetric. That error is harmless, and the reason is the same reason
   that wall_minus_core_us must stay constant in one message: this code latches both terms at the same
   moment, and it holds both for the same duration. Thus each edge of one message gets the identical
   error, which is one uniform shift and not a change in the space between the edges.
   IR_LINK_PLAYOUT_DELAY_US absorbs a uniform shift.

   The measurement is the standard method for two clocks with a message path between them, and it uses
   four timestamps. This instance sends a PING at t0 on its own clock. The peer reads that PING at t1 and
   answers at t2, both on the clock of the peer. This instance reads the answer at t3 on its own clock.
   Then:

     offset = ((t1 - t0) + (t2 - t3)) / 2
     round trip = (t3 - t0) - (t2 - t1)

   Two timestamps are not sufficient here, and a measurement gives the reason. A form with two timestamps
   must assume that the peer answers in the middle of the round trip. The peer does not: it reads a
   message at one pump call and it answers at that same call, thus its turnaround is as long as the
   interval between two pump calls, which is approximately 31ms. A measurement on the local pipe, where
   the true offset is 0 and each reported value is error, gave a round trip of 46695us and an error of
   23336us, which is one half of that round trip. The four-timestamp form subtracts (t2 - t1), thus the
   turnaround of the peer leaves the estimate.

   This code keeps the last IR_LINK_SYNC_WINDOW samples and uses the offset of the sample with the
   smallest round trip. The error that remains is one half of the difference between the two directions of
   the path. A small round trip is the sample where both directions were fastest, thus its difference is
   also smallest. A sample from a busy moment has a large round trip, and the filter ignores it.
   A PING gets its t0, and a PONG gets its t2, at the moment that the transport takes the message. Neither
   one gets that value at the moment that this code puts the message in the queue. Thus the drain time of
   a queue that holds a full IR message does not enter the measurement.

   The error does not have to be small, and this is the reason. The error is one constant for each latch,
   thus each edge of one message gets the same shift and the space between the edges stays exact. A
   different message gets a different shift, and each message decodes on its own. Thus the only
   requirement is that the error stays well inside IR_LINK_PLAYOUT_DELAY_US. */
#define IR_LINK_SYNC_WINDOW 8u

/* The interval between the PING messages of this instance.
   The interval must be short enough that the window above stays current against the drift between two
   crystals, and long enough that the traffic is insignificant next to a real IR message. This value
   gives a window that covers 4 seconds, and 2 messages of 16 bytes for each second.
   A PING and a PONG are not edge traffic. Thus they must never call note_edge_activity: that function
   drives the idle test of IR_LINK_OFFSET_RELATCH_IDLE_US, and keepalive traffic that marked the link
   busy would stop each later latch. */
#define IR_LINK_PING_INTERVAL_US 500000ull

/* The time after which this code abandons a PING that has no PONG, and sends a new one.
   Without this limit, one lost answer would stop the measurement for the life of the link. */
#define IR_LINK_PING_TIMEOUT_US 5000000ull

/* One message of 16 bytes for each edge, with kind = IR_WIRE_KIND_EDGE.
   A handshake at connect time uses kind = IR_WIRE_KIND_HELLO.
   The clock measurement above uses kind = IR_WIRE_KIND_PING and kind = IR_WIRE_KIND_PONG.
   Only an EDGE message gets to psemu_ir_push_rx_edge.
   The function of HELLO is to find a magic-number or version difference at connect time. Without it, the
   code finds the difference only at the first real IR traffic.
   Each received message contains the magic number and the version, and this code tests both.
   A difference shows that the two instances execute incompatible builds. Thus this code closes the
   link and reports the reason.

   This code keeps one PING in progress at most. Thus a PING needs no sequence number: a PONG answers the
   one PING that has no answer. A PONG that arrives with no PING in progress is a duplicate, and this code
   discards it.

   A message is 24 bytes, because a PONG holds two timestamps. The second field costs 8 bytes for each
   edge, which is approximately 5kB for one IR message of 658 edges. That cost buys the removal of the
   largest term in the offset error, which the measurement above gives as 23336us. */
#define IR_WIRE_MAGIC 0x52494B50u /* 'PKIR' */
/* Version 2 adds PING and PONG. A build that speaks version 1 has no clock measurement, thus it would
   place each edge of a peer on a different machine minutes away from the correct time. The HELLO test
   rejects that build at connect time, and it reports the reason. */
#define IR_WIRE_VERSION 2u
#define IR_WIRE_KIND_EDGE 0u
#define IR_WIRE_KIND_HELLO 1u
#define IR_WIRE_KIND_PING 2u
#define IR_WIRE_KIND_PONG 3u

#pragma pack(push, 1)
typedef struct ir_wire_message {
    uint32_t magic;
    uint16_t version;
    uint8_t level;
    uint8_t kind;
    /* Absolute host wall-clock microseconds, on the clock of the sender.
       EDGE: the time that the sender made the edge.
       PING: the time that the transport took the message. This is t0 above.
       PONG: the time that the answering instance read the PING. This is t1 above.
       HELLO: this field has no meaning. */
    uint64_t timestamp_us;
    /* PONG: the time that the transport took the answer, on the clock of the answering instance. This is
       t2 above. This field is 0 for each other kind. */
    uint64_t timestamp2_us;
} ir_wire_message_t;
#pragma pack(pop)

typedef struct ir_link {
    ir_link_state_t state;
    ir_transport_t transport;

    /* The framing of the receive direction. A transport keeps no message boundary, thus this code
       collects the bytes of one wire message before it uses that message. A short read leaves a partial
       message here until more bytes arrive. */
    uint8_t read_bytes[sizeof(ir_wire_message_t)];
    uint32_t read_fill;

    /* The outgoing edges. In the worst condition, the CPU makes a group of transitions in one frame,
       faster than one transport round trip can drain the queue.
       A full queue discards the newest edge. It does not block, and it does not increase without a
       limit. See enqueue_write. */
    ir_wire_message_t write_queue[IR_LINK_WRITE_QUEUE_CAPACITY];
    uint32_t write_head;
    uint32_t write_count;
    /* The number of bytes of the message at write_head that the transport already took. A transport can
       take part of a message and refuse the remainder. */
    uint32_t write_offset;

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
    /* The wall clock of the peer, minus the wall clock of this instance. See IR_LINK_SYNC_WINDOW above.
       This code latches this term together with wall_minus_core_us, and it holds both for the same
       duration. The latch takes the term from the sample window below. */
    int64_t peer_wall_minus_local_wall_us;
    int clock_offset_latched;
    uint64_t last_edge_wall_us; /* the time of the last edge on this link, for the idle test above */

    /* The sample window of the clock measurement. */
    struct {
        int64_t offset_us;
        int64_t rtt_us;
    } sync_samples[IR_LINK_SYNC_WINDOW];
    uint32_t sync_sample_head;
    uint32_t sync_sample_count;
    uint64_t ping_sent_wall_us; /* t0 of the PING that has no PONG. It holds the queue time until the
                                   transport takes the message, and then the send time. */
    int ping_outstanding;
    uint64_t last_ping_wall_us; /* when this instance last put a PING in the queue */
    int64_t last_rtt_us;        /* the newest sample, for the diagnostic status line */
    int64_t last_offset_us;
    /* A test hook. Each wall-clock read of this link adds this value, thus one link can operate on a
       clock that is hours away from the clock of its peer. Two processes on one machine always share a
       wall clock, thus without this hook no local test can show whether the code measures the offset of
       the peer or ignores it: the correct answer and the answer of a build with no measurement are both
       0. The desktop frontend never writes this field, and it stays 0 there.
       This is permanent test equipment, in the same manner as psemu_ir_trace_enabled in core/src/ir.h. */
    int64_t test_wall_offset_us;

    /* Simple counters. They give diagnostic data for a link that connects but carries no useful data.
       They are inexpensive, thus they operate always. They are the only method to tell "the peer sent
       nothing" from "the peer sent the data and this instance discarded it".
       dropped_tx counts the edges that enqueue_write discarded because the queue was full. */
    unsigned long edges_sent;
    unsigned long edges_received;
    unsigned long dropped_tx;
    /* The number of calls into the transport that took data. One IR message is several hundred edges,
       thus a transport that takes one wire message for each call needs several hundred system calls for
       one message, and each one goes on the network as its own small packet. This counter gives that
       number directly. See poll_write in ir_link.c. */
    unsigned long transport_writes;
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

    char status[128]; /* human-readable text for ir_link_status_text, for example a window-title suffix */
} ir_link_t;

typedef enum {
    IR_LINK_TRANSPORT_PIPE, /* two processes on one machine. The address is a named-pipe name. */
    IR_LINK_TRANSPORT_TCP   /* two machines. The address is "host:port", and see ir_transport.h. */
} ir_link_transport_kind_t;

/* Prepares a link that uses `kind` for its transport. The clock measurement operates for each kind. It
   measures 0 for the named pipe, where both processes read the same wall clock. */
void ir_link_init(ir_link_t *link, ir_link_transport_kind_t kind);

/* Closes any link that is present, and then changes the transport to `kind`.
   It keeps each value of the link that is not part of a connection: show_diagnostics and the counters.
   Thus a frontend can offer both a local link and a network link on one ir_link_t. */
void ir_link_set_transport(ir_link_t *link, ir_link_transport_kind_t kind);

/* Makes the endpoint at `address`, and then listens for a peer.
   It returns 1 if the operation is successful. The state "this instance still waits for a peer" is
   success, and not an error.
   It returns 0 if it cannot make the endpoint. */
int ir_link_host(ir_link_t *link, const char *address);

/* Starts a connection to a peer that already hosts a link at `address`.
   It always returns 1. ir_link_pump tries the connection again at later calls, until a host
   listens. */
int ir_link_connect(ir_link_t *link, const char *address);

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
