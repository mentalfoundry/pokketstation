#include "ir.h"

#include <stdio.h>

#include "cpu.h" /* psemu_debug_current_pc, for the trace flag below */
#include "dac.h" /* PSEMU_ASSUMED_CPU_HZ: the reference-rate cycle unit ir_tick's `cycles` argument is in */
#include "intc.h"

/* See ir.h. Off by default; tools/ir_probe.c turns it on. */
int psemu_ir_trace_enabled = 0;

/* Approximately 2 carrier periods, in PSEMU_ASSUMED_CPU_HZ reference-rate cycles.
   This is an inferred debounce window, not a confirmed hardware measurement. See ir.h's top comment. */
#define IR_BFLT_DEBOUNCE_CYCLES ((2ull * PSEMU_ASSUMED_CPU_HZ) / IR_CARRIER_HZ)

/* A deliberate concession, not a modeled physical effect. Documented here, not hidden: a real IR-using
   app's receive-side sync-pulse acceptance window rejects this emulator's own transmitted sync pulse by a
   small margin (measured at 26-38 Timer2 ticks short of the window floor - see docs/hardware-notes.md's
   "Unresolved" bullet for the full history). Real hardware measurements, three separate times, ruled out
   every CPU and interrupt-dispatch explanation for a shortfall of this shape: timer reload semantics,
   interrupt entry cost, and re-arm latency over both IRQ and FIQ, including a full realistic dispatch
   chain with the same shape the real transmit handler uses. All three matched real hardware exactly. This
   emulator's own digital timing is not the source of the gap.
   The leading remaining explanation is real transceiver physics outside the CPU entirely: an LED does not
   switch off instantly, and a receiving photodiode's own response and AGC settling add real time before a
   pulse reads as "ended". This emulator does not model IR as an analog signal, and does not try to here
   either. It only stretches when a transmitted pulse appears to end, by a fixed amount, so that a real
   app's own receive-side timing expectations - tuned against real analog hardware this project has no way
   to reproduce - can still be met by this fully digital link.
   Only the falling edge (LED digitally commanded off) is delayed. The rising edge (LED turning on) stays
   at its true digital time, and the OFF gap between pulses is intentionally left unstretched: there is no
   real-hardware evidence for what a receiver expects there, only for the ON-pulse acceptance window.
   The constant is tuned against this emulator's own measured shortfall (38 Timer2 ticks, at a real IR
   app's own IR-screen clock rate and Timer2's /2 divisor), converted to this fixed PSEMU_ASSUMED_CPU_HZ
   reference rate, not against the real app's own unrelated "184" constant: an earlier attempt used 184
   directly and reordered edges outright, delaying a falling edge past the next pulse's rising edge - the
   real gap between transmitted pulses is far smaller than 184 Timer2 ticks. See tools/ir_probe.c for the
   two-instance test this was calibrated against, and for the trace that caught that reordering before it
   shipped.
   A second, related failure came from the same root cause at a smaller scale: stretching the falling edge
   necessarily compresses the OFF gap that follows it (there is no way to lengthen an ON pulse without
   taking the time from somewhere adjacent). At 316, that compression left only ~1.2-1.6x IR_BFLT_DEBOUNCE_
   CYCLES of margin on the shortest real gaps - thin enough that this emulator's own glitch filter
   occasionally rejected a genuine gap as noise, merging two pulses (and the bits they encoded) into one.
   tools/ir_probe.c's own byte-for-byte buffer comparison caught this directly: specific bytes decoded
   wrong, and the raw Timer2-tick deltas at those positions did not match any single-pulse duration, only
   sums of two or three consecutive ones. 200 keeps sync inside its acceptance window with room to spare
   (4268 against a 4200-5400 window) while keeping over 3x debounce margin on the shortest gap.

   The stretch is additionally capped at the pulse's own ON duration (see enqueue_tx_edge). Everything above
   was tuned against one app whose pulses are wide envelopes, where 200 cycles is a small additive
   correction. It is not one for every app. Yu-Gi-Oh Forbidden Memories transmits ~7-cycle (6.6us) pulses
   spaced 205 or 406 cycles apart, and encodes each bit in the gap length rather than in the pulse. Applying
   a flat 200 there inverted the waveform outright: measured over a real transfer, 7-cycle ON / 205-cycle OFF
   arrived as 207-cycle ON / 5-cycle OFF, and 272 gaps collapsed to exactly 0 cycles - the falling edge
   landing on the next rising edge, merging two pulses into one continuous ON. Only the reordering guard
   below kept that from going backwards outright, and a zero-length gap is already unrecoverable.
   Capping at the ON duration keeps this a turn-off tail rather than fabricated signal. A tail longer than
   the pulse that caused it is not a tail, and a receiver's AGC settles faster after less delivered energy,
   so a short pulse earns a proportionally short stretch. The cap is inert for the app the constant was
   tuned against, whose sync pulse is ~4068 cycles wide: min(200, 4068) is still 200, and its measured
   4268-against-4200-5400 sync figure is unchanged. */
#define IR_TX_FALL_STRETCH_CYCLES 200ull

void ir_init(ir_t *ir) {
    ir->mode = 0;
    ir->data = 0;
    ir->clock_cycles = 0;
    ir->tx_led_state = 0;
    ir->tx_last_edge_cycles = 0;
    ir->tx_queue.head = 0;
    ir->tx_queue.count = 0;
    ir->rx_queue.head = 0;
    ir->rx_queue.count = 0;
    ir->rx_level = 0;
    ir->rx_pending_valid = 0;
    ir->rx_pending_level = 0;
    ir->rx_pending_since_cycles = 0;
}

/* Names offsets 0/4/8/0xC for trace output. Offset 8 has no named register in any external reference.
   It falls in the gap between IRDA_DATA and IRDA_MISC. This reports offset 8 as part of IRDA_MISC's
   reserved span, instead of inventing a name for it. Nothing distinguishes the two, in this emulator
   or externally. */
static const char *ir_reg_name(uint32_t word_index) {
    switch (word_index) {
    case 0:
        return "IRDA_MODE";
    case 1:
        return "IRDA_DATA";
    default:
        return "IRDA_MISC";
    }
}

uint32_t ir_read(ir_t *ir, uint32_t offset) {
    uint32_t word_index = (offset / 4u) % 4u;
    uint32_t shift = (offset % 4u) * 8u;
    uint32_t value;

    if (word_index == 1u && (ir->mode & IR_MODE_IFMODE) == 0u) {
        /* Receive mode: DATA bit0 mirrors the live demodulated line, which is active low. Carrier present
           reads 0, idle reads 1. ir->rx_level itself stays in physical terms (1 = carrier present), so the
           inversion lives here and in apply_rx_level's INT_IRDA level, the two places software can observe.
           See ir.h's top comment for the disassembly this rests on. */
        value = ir->rx_level ? 0u : IR_DATA_LED;
    } else if (word_index >= 2u) {
        /* IRDA_MISC (+0xC) and the gap before it (+0x8) get the same treatment. An external reference marks
           IRDA_MISC unknown or reserved, with no documented reset value or behavior. This emulator has no
           basis to invent register state for it. It reads back 0. Writes have no effect. This is the same
           stub treatment this project already gives BATT_CTRL (see docs/hardware-notes.md, "Known open
           questions"). */
        value = 0u;
    } else {
        value = (word_index == 0u) ? ir->mode : ir->data;
    }
    if (psemu_ir_trace_enabled) {
        printf("[ir trace] t=%llu pc=0x%08X READ %s (+0x%X) = 0x%02X (full=0x%08X)\n",
            (unsigned long long)ir->clock_cycles, psemu_debug_current_pc, ir_reg_name(word_index), (unsigned)offset,
            (unsigned)((value >> shift) & 0xFFu), value);
    }
    return (value >> shift) & 0xFFu;
}

static int queue_push(ir_edge_queue_t *q, uint64_t timestamp_cycles, int level) {
    uint32_t tail;
    if (q->count >= IR_EDGE_QUEUE_CAPACITY) {
        if (psemu_ir_trace_enabled) {
            printf("[ir trace] QUEUE FULL: dropped edge level=%d t=%llu\n", level,
                (unsigned long long)timestamp_cycles);
        }
        return 0; /* full: drop the newest edge rather than corrupt ordering */
    }
    tail = (q->head + q->count) % IR_EDGE_QUEUE_CAPACITY;
    q->entries[tail].timestamp_cycles = timestamp_cycles;
    q->entries[tail].level = level;
    q->count++;
    return 1;
}

static int queue_pop(ir_edge_queue_t *q, ir_edge_t *out) {
    if (q->count == 0u) {
        return 0;
    }
    *out = q->entries[q->head];
    q->head = (q->head + 1u) % IR_EDGE_QUEUE_CAPACITY;
    q->count--;
    return 1;
}

static const ir_edge_t *queue_peek(const ir_edge_queue_t *q) {
    return q->count ? &q->entries[q->head] : (void *)0;
}

/* Transmit is observable under two conditions. IFMODE selects transmit, and STDBY is clear.
   A write to IRDA_DATA outside those conditions still moves the register value, exactly like real hardware.
   It produces no edge, because the transmitter is not driving the LED at all.

   BGEN is deliberately not part of this test, and that is a correction of an earlier model. BGEN selects
   whether the hardware chops the LED's ON envelope into a 40kHz burst. It does not decide whether the LED
   lights at all. This emulator relays only that ON/OFF envelope and explicitly does not model the
   sub-carrier inside it (see ir.h's top comment), so BGEN has nothing left to gate here.

   Two real, working apps settle this, and they disagree on BGEN while agreeing on everything else:
     - Chocobo World transmits with IRDA_MODE=0x01: BGEN=0 (hardware carrier on), BFLT=0 (glitch filter on).
       It sends wide envelope pulses and lets the hardware fill them with carrier.
     - Yu-Gi-Oh Forbidden Memories transmits with IRDA_MODE=0x0D: BGEN=1 (hardware carrier off), BFLT=1
       (glitch filter off). It drives the LED directly, in ~7-cycle (6.6us) pulses spaced 205 or 406 cycles
       apart - pulse-distance modulation, where the gap carries the bit. Those pulses are far narrower than
       IR_BFLT_DEBOUNCE_CYCLES, which is exactly why this app turns the glitch filter off in the same write.
   Both transfer on real hardware. Gating emission on BGEN made the second app emit nothing at all: every
   IRDA_DATA write was discarded, and tools/ir_probe.c reported "edges relayed: A->B 0" against a save state
   sitting on its own transfer screen. */
static int tx_emit_active(const ir_t *ir) {
    return (ir->mode & IR_MODE_IFMODE) != 0u && (ir->mode & IR_MODE_STDBY) == 0u;
}

static void enqueue_tx_edge(ir_t *ir, int level) {
    /* See IR_TX_FALL_STRETCH_CYCLES above: only a falling edge (level 0, LED commanded off) is delayed, and
       never by more than the pulse's own ON duration. */
    uint64_t timestamp = ir->clock_cycles;
    if (level == 0) {
        /* A falling edge always follows a rising one (handle_data_write only enqueues on a real change, and
           handle_mode_write only forces 0 while tx_led_state is 1), and a rising edge is never stretched.
           tx_last_edge_cycles therefore still holds this pulse's true rise time, so the ON duration is
           available here without keeping a second timestamp. Deliberately so: ir_t is part of the raw
           psemu_t struct dump a save state is made of, and an extra field there invalidates every existing
           save. See QUICKSAVE_VERSION in frontends/desktop/main.c. */
        uint64_t on_duration = ir->clock_cycles - ir->tx_last_edge_cycles;
        uint64_t stretch = on_duration < IR_TX_FALL_STRETCH_CYCLES ? on_duration : IR_TX_FALL_STRETCH_CYCLES;
        timestamp += stretch;
    }
    /* Guards against a stretched falling edge landing after the pulse that follows it. A real gap this
       short should not happen at the tuned constant above, but this makes that a compressed edge instead
       of a silently reordered queue if it ever does - caught exactly this way once already, while tuning
       the constant: an earlier, larger value delayed a falling edge past the next rising edge outright. */
    if (timestamp < ir->tx_last_edge_cycles) {
        timestamp = ir->tx_last_edge_cycles;
    }
    ir->tx_last_edge_cycles = timestamp;
    queue_push(&ir->tx_queue, timestamp, level);
}

static void handle_mode_write(ir_t *ir) {
    /* The LED goes off as soon as the transmit-emit condition ends. Standby, receive mode, or a disabled
       carrier all end it.
       A real IR LED behaves the same way when it loses power. Software cannot leave it stuck on. */
    if (!tx_emit_active(ir) && ir->tx_led_state != 0) {
        enqueue_tx_edge(ir, 0);
        ir->tx_led_state = 0;
    }
}

static void handle_data_write(ir_t *ir) {
    int new_level;
    if (!tx_emit_active(ir)) {
        return;
    }
    new_level = (ir->data & IR_DATA_LED) ? 1 : 0;
    if (new_level != ir->tx_led_state) {
        enqueue_tx_edge(ir, new_level);
        ir->tx_led_state = new_level;
    }
}

void ir_write(ir_t *ir, uint32_t offset, uint32_t value) {
    uint32_t word_index = (offset / 4u) % 4u;
    uint32_t shift = (offset % 4u) * 8u;
    uint32_t byte = value & 0xFFu;
    uint32_t *reg;

    if (word_index >= 2u) {
        /* IRDA_MISC and the gap before it get a no-op write. See ir_read's comment on the same span. */
        if (psemu_ir_trace_enabled) {
            printf("[ir trace] t=%llu pc=0x%08X WRITE %s (+0x%X) = 0x%02X (ignored, reserved)\n",
                (unsigned long long)ir->clock_cycles, psemu_debug_current_pc, ir_reg_name(word_index),
                (unsigned)offset, (unsigned)byte);
        }
        return;
    }

    reg = (word_index == 0u) ? &ir->mode : &ir->data;
    *reg = (*reg & ~(0xFFu << shift)) | (byte << shift);

    if (psemu_ir_trace_enabled) {
        printf("[ir trace] t=%llu pc=0x%08X WRITE %s (+0x%X) = 0x%02X (full=0x%08X)\n",
            (unsigned long long)ir->clock_cycles, psemu_debug_current_pc, ir_reg_name(word_index), (unsigned)offset,
            (unsigned)byte, *reg);
    }

    if (word_index == 0u) {
        handle_mode_write(ir);
    } else if (shift == 0u) {
        handle_data_write(ir);
    }
}

/* Applies a level that passed the debounce filter to rx_level.
   It asserts INT_IRDA if receive mode is active (IFMODE=0, STDBY=0).
   It drops an edge that arrives while this is not listening, that is, while transmitting or in standby.
   A real half-duplex transceiver does not see that edge either. */
static void apply_rx_level(ir_t *ir, struct intc *intc, int level) {
    int listening;
    if (level == ir->rx_level) {
        return;
    }
    ir->rx_level = level;
    listening = (ir->mode & IR_MODE_IFMODE) == 0u && (ir->mode & IR_MODE_STDBY) == 0u;
    if (psemu_ir_trace_enabled) {
        printf("[ir trace] t=%llu RX edge level=%d %s (mode=0x%08X)\n", (unsigned long long)ir->clock_cycles, level,
            listening ? "-> INT_IRDA asserted" : "DROPPED (not listening)", ir->mode);
    }
    if (listening) {
        /* This is not a plain intc_set_line.
           The receive handler reads the live line level back out of STATUS bit 12.
           It then compares that level against the level it expected.
           STATUS must therefore follow the real level, while HOLD still latches an interrupt on both edges.
           A disassembly of a real app's INT_IRDA handler confirms this.
           See intc.h's INT_STATUS_MASK comment. */
        intc_set_level_and_pulse(intc, INT_IRDA, !level);
    }
}

/* BFLT is bit 3, and it uses inverted logic, so 0 enables the filter.
   The filter rejects a transition that does not last IR_BFLT_DEBOUNCE_CYCLES before an opposite edge arrives.
   A raw edge starts a pending candidate, or restarts one.
   ir_tick's resolve_pending accepts that candidate once enough local time passes with no opposite edge. */
static void debounce_edge(ir_t *ir, struct intc *intc, int level, uint64_t at_cycles) {
    int filter_enabled = (ir->mode & IR_MODE_BFLT) == 0u;
    if (!filter_enabled) {
        apply_rx_level(ir, intc, level);
        ir->rx_pending_valid = 0;
        return;
    }
    if (level == ir->rx_level) {
        ir->rx_pending_valid = 0; /* matches the already-accepted level; cancel any stale opposite candidate */
        return;
    }
    ir->rx_pending_valid = 1;
    ir->rx_pending_level = level;
    ir->rx_pending_since_cycles = at_cycles;
}

static void resolve_pending(ir_t *ir, struct intc *intc) {
    if (!ir->rx_pending_valid) {
        return;
    }
    if (ir->clock_cycles - ir->rx_pending_since_cycles >= IR_BFLT_DEBOUNCE_CYCLES) {
        apply_rx_level(ir, intc, ir->rx_pending_level);
        ir->rx_pending_valid = 0;
    }
}

void ir_tick(ir_t *ir, struct intc *intc, uint32_t cycles) {
    ir->clock_cycles += cycles;

    for (;;) {
        const ir_edge_t *front = queue_peek(&ir->rx_queue);
        ir_edge_t edge;
        if (!front || front->timestamp_cycles > ir->clock_cycles) {
            break;
        }
        queue_pop(&ir->rx_queue, &edge);
        debounce_edge(ir, intc, edge.level, edge.timestamp_cycles);
    }
    resolve_pending(ir, intc);
}

int ir_pop_tx_edge(ir_t *ir, ir_edge_t *out_edge) {
    return queue_pop(&ir->tx_queue, out_edge);
}

void ir_push_rx_edge(ir_t *ir, uint64_t timestamp_cycles, int level) {
    queue_push(&ir->rx_queue, timestamp_cycles, level);
}

uint64_t ir_get_clock_cycles(const ir_t *ir) {
    return ir->clock_cycles;
}
