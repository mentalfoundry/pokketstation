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

void ir_init(ir_t *ir) {
    ir->mode = 0;
    ir->data = 0;
    ir->clock_cycles = 0;
    ir->tx_led_state = 0;
    ir->tx_queue.head = 0;
    ir->tx_queue.count = 0;
    ir->rx_queue.head = 0;
    ir->rx_queue.count = 0;
    ir->rx_level = 0;
    ir->rx_pending_valid = 0;
    ir->rx_pending_level = 0;
    ir->rx_pending_since_cycles = 0;
}

uint32_t ir_read(ir_t *ir, uint32_t offset) {
    uint32_t word_index = (offset / 4u) % 2u;
    uint32_t shift = (offset % 4u) * 8u;
    uint32_t value;

    if (word_index == 1u && (ir->mode & IR_MODE_IFMODE) == 0u) {
        /* Receive mode: DATA bit0 mirrors the live demodulated level, not the last value software wrote
           there (software has no reason to be writing it while receiving anyway). See ir.h's top comment
           on this being an inferred, unconfirmed read-back semantic. */
        value = ir->rx_level ? IR_DATA_LED : 0u;
    } else {
        value = (word_index == 0u) ? ir->mode : ir->data;
    }
    if (psemu_ir_trace_enabled) {
        printf("[ir trace] t=%llu pc=0x%08X READ %s (+0x%X) = 0x%02X (full=0x%08X)\n",
            (unsigned long long)ir->clock_cycles, psemu_debug_current_pc, word_index == 0u ? "IRDA_MODE" : "IRDA_DATA",
            (unsigned)offset, (unsigned)((value >> shift) & 0xFFu), value);
    }
    return (value >> shift) & 0xFFu;
}

static int queue_push(ir_edge_queue_t *q, uint64_t timestamp_cycles, int level) {
    uint32_t tail;
    if (q->count >= IR_EDGE_QUEUE_CAPACITY) {
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

/* Transmit is observable only under three conditions. IFMODE selects transmit, STDBY is clear, and the
   carrier generator is enabled. BGEN uses inverted logic, so 0 enables it.
   A write to IRDA_DATA outside those conditions still moves the register value, exactly like real hardware.
   It produces no edge, because there is no carrier for it to modulate. */
static int tx_emit_active(const ir_t *ir) {
    return (ir->mode & IR_MODE_IFMODE) != 0u && (ir->mode & IR_MODE_STDBY) == 0u &&
           (ir->mode & IR_MODE_BGEN) == 0u;
}

static void enqueue_tx_edge(ir_t *ir, int level) {
    queue_push(&ir->tx_queue, ir->clock_cycles, level);
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
    uint32_t word_index = (offset / 4u) % 2u;
    uint32_t shift = (offset % 4u) * 8u;
    uint32_t byte = value & 0xFFu;
    uint32_t *reg = (word_index == 0u) ? &ir->mode : &ir->data;

    *reg = (*reg & ~(0xFFu << shift)) | (byte << shift);

    if (psemu_ir_trace_enabled) {
        printf("[ir trace] t=%llu pc=0x%08X WRITE %s (+0x%X) = 0x%02X (full=0x%08X)\n",
            (unsigned long long)ir->clock_cycles, psemu_debug_current_pc, word_index == 0u ? "IRDA_MODE" : "IRDA_DATA",
            (unsigned)offset, (unsigned)byte, *reg);
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
        intc_set_level_and_pulse(intc, INT_IRDA, level);
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
