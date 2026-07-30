#ifndef PSEMU_IR_H
#define PSEMU_IR_H

#include <stdint.h>

struct intc;

#define IR_REG_SPAN 0x8u

/* IRDA_MODE (offset 0) bits and IRDA_DATA (offset 4) bit 0, per the documented `PMIrMode`-style register layout:
     IRDA_MODE bit0 IFMODE  0=Receive, 1=Transmit
     IRDA_MODE bit1 STDBY   0=Active, 1=Stand-by
     IRDA_MODE bit2 BGEN    0=Enable 40KHz carrier generator, 1=Disable
     IRDA_MODE bit3 BFLT    0=Enable filter, 1=Disable
     IRDA_DATA bit0 LED     Transmit: 0=LED off, 1=LED on. Receive: the demodulated carrier level.

   The receive meaning of IRDA_DATA bit 0 is this emulator's own inference.
   It is undocumented, and unconfirmed against real hardware.
   Nothing in this project's traced corpus has ever touched these registers.

   Real pulses are long or short ON periods, separated by short OFF gaps. A long pulse is approximately 2x a
   short pulse.
   The real RX-IRQ handler (INT_IRDA, see intc.h) measures an incoming pulse's length.
   It does this by reading Timer 2's live counter (reload 0xFFFFh) at the interrupt.

   This models IR as an asynchronous edge relay between two independently-clocked instances.
   Real IR hardware works the same way: two separate devices, two separate oscillators, and an optical signal
   between them. The two devices share no clock.
   There is no lockstep timing assumption between a local core and whatever supplies its RX edges.
   That supplier is a loopback caller, or a second emulator instance over some transport.
   Core tracks only its own local monotonic time. It reacts to edges timestamped against that local timeline.

   Core stays platform-agnostic. It has no notion of a second instance, a pipe, or a socket.
   It exposes only an edge-queue API: ir_pop_tx_edge, ir_push_rx_edge, and ir_get_clock_cycles.
   Whatever sits above core drives that API.
   psemu_get_audio_samples already uses the same shape. It lets a frontend pull PCM out, and core knows nothing
   about SDL.

   This does not model a toggling 40kHz square wave inside a pulse's "on" interval.
   Nothing observable depends on the sub-carrier itself. Only the gated on/off envelope matters.
   IR_CARRIER_HZ therefore has two uses only. It derives the BFLT debounce window below, and it gates TX
   emission. A pulse emits only while the carrier generator is enabled.

   The BFLT debounce window is approximately 2 carrier periods. This is an inferred constant, not a confirmed
   hardware measurement. This project flags every other unconfirmed assumption the same way.
   See docs/hardware-notes.md. */
#define IR_MODE_IFMODE (1u << 0)
#define IR_MODE_STDBY (1u << 1)
#define IR_MODE_BGEN (1u << 2)
#define IR_MODE_BFLT (1u << 3)
#define IR_DATA_LED (1u << 0)

#define IR_CARRIER_HZ 40000u

#define IR_EDGE_QUEUE_CAPACITY 64u

/* An edge is a transition of the demodulated IR signal.
   Level 1 means the carrier/LED went on. Level 0 means it went off.
   Timestamps use this ir_t instance's own local monotonic clock (see ir_get_clock_cycles).
   They are in the same reference-clock cycle units psemu_run already computes for rtc_tick and dac_tick.
   They are not real microseconds.
   A frontend converts to and from wall-clock microseconds when it relays edges to another instance.
   See psemu_ir_get_clock_us in psemu.h. */
typedef struct ir_edge {
    uint64_t timestamp_cycles;
    int level;
} ir_edge_t;

typedef struct ir_edge_queue {
    ir_edge_t entries[IR_EDGE_QUEUE_CAPACITY];
    uint32_t head;
    uint32_t count;
} ir_edge_queue_t;

typedef struct ir {
    uint32_t mode;
    uint32_t data;

    uint64_t clock_cycles; /* local monotonic clock. Only ir_tick advances it. */

    int tx_led_state; /* the last edge level this emitted onto tx_queue */

    ir_edge_queue_t tx_queue; /* edges from local CPU writes. ir_pop_tx_edge collects them. */
    ir_edge_queue_t rx_queue; /* edges from ir_push_rx_edge. They wait for their delivery time. */

    int rx_level; /* the current demodulated level. IRDA_DATA bit0 shows it in receive mode. */

    /* BFLT glitch-filter state. A new edge stays pending for IR_BFLT_DEBOUNCE_CYCLES.
       If no opposite edge arrives in that time, the filter accepts it.
       It then applies it to rx_level and to INT_IRDA. */
    int rx_pending_valid;
    int rx_pending_level;
    uint64_t rx_pending_since_cycles;
} ir_t;

/* Diagnostic flag. It follows the same pattern as intc.h's psemu_intc_trace_enabled.
   It is off by default, so it costs nothing in normal use.
   It logs every real IR register access with its real PC.
   Those PCs identify the app-side IR routines worth disassembling.
   Static disassembly alone cannot reliably tell ARM from Thumb, because it does not track runtime mode.
   This is permanent diagnostic infrastructure. It is not tied to any single investigation. */
extern int psemu_ir_trace_enabled;

void ir_init(ir_t *ir);
uint32_t ir_read(ir_t *ir, uint32_t offset);
void ir_write(ir_t *ir, uint32_t offset, uint32_t value);

/* Advances ir's local clock by `cycles`.
   These are the same reference-rate cycle units rtc_tick and dac_tick receive.
   It then resolves every rx_queue edge that is now due.
   For each one it applies the BFLT debounce and updates rx_level.
   It asserts INT_IRDA for each edge that passes the filter while receive mode is active (IFMODE=0, STDBY=0).
   It drops an edge that becomes due outside that mode.
   A real half-duplex transceiver does not listen for that edge either. */
void ir_tick(ir_t *ir, struct intc *intc, uint32_t cycles);

/* Pulls the next locally-produced TX edge.
   It returns 1 and fills *out_edge. It returns 0 if tx_queue is empty.
   Call this every frame. Convert each edge to wall-clock time, then relay it onward.
   That links this instance's IR transmit side to another instance's IR receive side. */
int ir_pop_tx_edge(ir_t *ir, ir_edge_t *out_edge);

/* Queues an RX edge from an external source.
   It delivers the edge at `timestamp_cycles` on this instance's own local clock.
   See ir_get_clock_cycles to convert a wall-clock timestamp into this timeline.
   It drops the edge if rx_queue is full.
   A full queue means the caller does not drain it fast enough.
   To drop a stale edge is better than to corrupt the queue order. */
void ir_push_rx_edge(ir_t *ir, uint64_t timestamp_cycles, int level);

uint64_t ir_get_clock_cycles(const ir_t *ir);

#endif
