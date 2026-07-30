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
     IRDA_DATA bit0 LED     Transmit: 0=LED off, 1=LED on. Receive: demodulated carrier level (this emulator's
                            own inference - undocumented and unconfirmed against real hardware, since nothing in
                            this project's traced corpus has ever touched these registers).

   Real pulses are long or short ON periods, separated by short OFF gaps. A long pulse is approximately 2x a
   short pulse.
   The real RX-IRQ handler (INT_IRDA, see intc.h) measures an incoming pulse's length.
   It does this by reading Timer 2's live counter (reload 0xFFFFh) at the interrupt.

   This models IR as an asynchronous edge relay between two independently-clocked instances, matching how real
   IR hardware actually works: two separate devices, two separate oscillators, linked only by an optical signal,
   with no shared clock. There is deliberately no lockstep timing assumption between a local core and whatever
   feeds it RX edges (a loopback caller, or eventually a second emulator instance over some transport) - core
   only tracks its own local monotonic time and reacts to edges timestamped against that local timeline.

   Core stays platform-agnostic: it has no notion of a second instance, a pipe, or a socket. It only exposes an
   edge-queue API (ir_pop_tx_edge/ir_push_rx_edge/ir_get_clock_us) for whatever sits above it to drive - the same
   shape psemu_get_audio_samples already uses to let a frontend pull PCM out without core knowing about SDL.

   What is NOT modeled: an actual toggling 40kHz square wave within a pulse's "on" interval. Nothing observable
   depends on the sub-carrier itself, only on the gated on/off envelope - so IR_CARRIER_HZ exists solely to derive
   the BFLT debounce window below, and to gate TX emission (a pulse only actually emits while the carrier
   generator is enabled).

   The BFLT debounce window (approximately 2 carrier periods) is an inferred constant, not a confirmed hardware
   measurement - flagged here the same way this project flags every other unconfirmed assumption (see
   docs/hardware-notes.md). */
#define IR_MODE_IFMODE (1u << 0)
#define IR_MODE_STDBY (1u << 1)
#define IR_MODE_BGEN (1u << 2)
#define IR_MODE_BFLT (1u << 3)
#define IR_DATA_LED (1u << 0)

#define IR_CARRIER_HZ 40000u

#define IR_EDGE_QUEUE_CAPACITY 64u

/* An edge is a transition of the demodulated IR signal: level 1 = carrier/LED just went on, level 0 = just
   went off. Timestamps are in this ir_t instance's own local monotonic clock (see ir_get_clock_cycles),
   expressed in the same reference-clock cycle units psemu_run already computes for rtc_tick/dac_tick - not
   real microseconds. Converting to/from wall-clock microseconds, for relaying edges to another instance, is a
   frontend-level concern (see psemu_ir_get_clock_us in psemu.h). */
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

    uint64_t clock_cycles; /* local monotonic clock, advanced only by ir_tick */

    int tx_led_state; /* last edge level actually emitted onto tx_queue */

    ir_edge_queue_t tx_queue; /* edges produced locally by CPU writes, awaiting pickup by ir_pop_tx_edge */
    ir_edge_queue_t rx_queue; /* edges pushed in by ir_push_rx_edge, awaiting their scheduled delivery time */

    int rx_level; /* current demodulated level, surfaced on IRDA_DATA bit0 while in receive mode */

    /* BFLT glitch-filter state: a just-arrived edge is held pending until it has stood unchallenged for
       IR_BFLT_DEBOUNCE_CYCLES before it is accepted as real and applied to rx_level/INT_IRDA. */
    int rx_pending_valid;
    int rx_pending_level;
    uint64_t rx_pending_since_cycles;
} ir_t;

/* Diagnostic flag, same pattern/rationale as intc.h's psemu_intc_trace_enabled: off by default, so it costs
   nothing in normal use. Logs every real IR register access with its real PC, which is what identifies the
   app-side IR routines worth disassembling (static disassembly alone cannot reliably tell ARM from Thumb
   without tracking runtime mode). Permanent diagnostic infrastructure, not tied to any single investigation. */
extern int psemu_ir_trace_enabled;

void ir_init(ir_t *ir);
uint32_t ir_read(ir_t *ir, uint32_t offset);
void ir_write(ir_t *ir, uint32_t offset, uint32_t value);

/* Advances ir's local clock by `cycles` (same reference-rate cycle units passed to rtc_tick/dac_tick), then
   resolves any rx_queue edges now due: applies BFLT debounce, updates rx_level, and calls
   intc_set_line(intc, INT_IRDA, 1) on each edge that survives the filter while in active receive mode
   (IFMODE=0, STDBY=0). An edge that becomes due while not in that mode is simply dropped, matching a real
   half-duplex transceiver not listening for it. */
void ir_tick(ir_t *ir, struct intc *intc, uint32_t cycles);

/* Pulls the next locally-produced TX edge, if any. Returns 1 and fills *out_edge, or returns 0 if tx_queue is
   empty. Call this every frame and relay each edge onward (converted to wall-clock time) to link this
   instance's IR TX to another instance's IR RX. */
int ir_pop_tx_edge(ir_t *ir, ir_edge_t *out_edge);

/* Queues an externally-sourced RX edge, scheduled for delivery at `timestamp_cycles` on this instance's own
   local clock (see ir_get_clock_us for converting a wall-clock timestamp into this timeline). Silently drops
   the edge if rx_queue is full - the caller is not draining/producing fast enough, and dropping a stale edge
   is preferable to corrupting queue ordering. */
void ir_push_rx_edge(ir_t *ir, uint64_t timestamp_cycles, int level);

uint64_t ir_get_clock_cycles(const ir_t *ir);

#endif
