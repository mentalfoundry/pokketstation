/* See the comment at the top of cpu_test.c. NDEBUG in a Release build removes each assert() call, thus
   this test suite must keep them. This code must come before <assert.h>. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>

#include "psemu_internal.h"

#define IRDA_MODE (PSEMU_IR_BASE + 0x0u)
#define IRDA_DATA (PSEMU_IR_BASE + 0x4u)

static psemu_t *make_ps(void) {
    psemu_t *ps = psemu_create();
    ps->intc.enable |= INT_IRDA;
    return ps;
}

/* IFMODE = 1 selects transmit mode. STDBY = 0 keeps the transmitter active. BGEN = 0 enables the
   carrier, because BGEN uses inverted logic. BFLT = 1 disables the filter, which has no effect during a
   transmission. */
#define TX_ACTIVE_MODE (IR_MODE_IFMODE | IR_MODE_BFLT)
/* IFMODE=0 (receive), STDBY=0, BFLT bit set per test as needed. */
#define RX_ACTIVE_MODE 0u

static void test_tx_write_with_carrier_enabled_enqueues_edges(void) {
    psemu_t *ps = make_ps();
    ir_edge_t edge;

    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED); /* LED on */

    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1);
    assert(edge.level == 1);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 0); /* nothing else queued */

    psemu_bus_write32(&ps->bus, IRDA_DATA, 0); /* LED off */
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1);
    assert(edge.level == 0);

    /* A repeated write to the same level is not a new edge. */
    psemu_bus_write32(&ps->bus, IRDA_DATA, 0);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 0);

    psemu_destroy(ps);
    printf("test_tx_write_with_carrier_enabled_enqueues_edges OK\n");
}

static void test_tx_write_while_not_driving_produces_no_edge(void) {
    psemu_t *ps = make_ps();
    ir_edge_t edge;

    /* Standby set: no emission even though IFMODE=transmit. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, IR_MODE_IFMODE | IR_MODE_STDBY);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 0);

    /* Receive mode (IFMODE=0): writes to DATA don't drive a TX LED at all. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, 0);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 0);

    psemu_destroy(ps);
    printf("test_tx_write_while_not_driving_produces_no_edge OK\n");
}

/* BGEN does not gate the emission. This test asserted the opposite condition before.
   BGEN selects whether the hardware divides the ON envelope of the LED into a 40kHz burst. It does not
   control whether the LED comes on. This emulator relays only that envelope, and it does not model the
   sub-carrier in the envelope (see ir.h). Thus BGEN has nothing to gate.
   Two real apps give the answer. One app transmits with BGEN = 0, and a second app transmits with
   BGEN = 1. Both apps complete a transfer on real hardware. While BGEN stopped the emission here, the
   second app produced no edges from a save state on its own transfer screen. */
static void test_tx_emits_with_carrier_generator_disabled(void) {
    psemu_t *ps = make_ps();
    ir_edge_t edge;

    psemu_bus_write32(&ps->bus, IRDA_MODE, IR_MODE_IFMODE | IR_MODE_BGEN | IR_MODE_BFLT);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1);
    assert(edge.level == 1);

    psemu_bus_write32(&ps->bus, IRDA_DATA, 0);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1);
    assert(edge.level == 0);

    psemu_destroy(ps);
    printf("test_tx_emits_with_carrier_generator_disabled OK\n");
}

static void test_leaving_tx_active_forces_led_off_edge(void) {
    psemu_t *ps = make_ps();
    ir_edge_t edge;

    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED); /* LED on */
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1 && edge.level == 1);

    /* Flip into standby mid-transmission: the LED must not stay stuck on. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE | IR_MODE_STDBY);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1);
    assert(edge.level == 0);

    psemu_destroy(ps);
    printf("test_leaving_tx_active_forces_led_off_edge OK\n");
}

static void test_loopback_edge_asserts_irda(void) {
    psemu_t *ps = make_ps();

    psemu_bus_write32(&ps->bus, IRDA_MODE, RX_ACTIVE_MODE | IR_MODE_BFLT); /* receive, filter disabled */
    assert(!intc_irq_asserted(&ps->intc));

    /* Idle, with no carrier, reads as 1: the demodulated receive line is active low. */
    assert((psemu_bus_read32(&ps->bus, IRDA_DATA) & IR_DATA_LED) != 0);

    ir_push_rx_edge(&ps->ir, ir_get_clock_cycles(&ps->ir), 1);
    ir_tick(&ps->ir, &ps->intc, 0);

    assert(intc_irq_asserted(&ps->intc));
    /* A carrier that is present now reads 0. This assertion expected the opposite value before, when
       the receive polarity was an unconfirmed inference of this emulator. ir.h recorded it as an
       inference. The receive handler of a real IR app gives the answer: it arms itself for level 0, and
       it measures a carrier burst as the low period. See the top comment of ir.h. */
    assert((psemu_bus_read32(&ps->bus, IRDA_DATA) & IR_DATA_LED) == 0);

    psemu_destroy(ps);
    printf("test_loopback_edge_asserts_irda OK\n");
}

static void test_rx_ignored_while_not_listening(void) {
    psemu_t *ps = make_ps();

    /* Transmit mode: a real half-duplex transceiver isn't listening. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    ir_push_rx_edge(&ps->ir, ir_get_clock_cycles(&ps->ir), 1);
    ir_tick(&ps->ir, &ps->intc, 0);
    assert(!intc_irq_asserted(&ps->intc));

    /* Standby: also not listening. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, IR_MODE_STDBY | IR_MODE_BFLT);
    ir_push_rx_edge(&ps->ir, ir_get_clock_cycles(&ps->ir), 1);
    ir_tick(&ps->ir, &ps->intc, 0);
    assert(!intc_irq_asserted(&ps->intc));

    psemu_destroy(ps);
    printf("test_rx_ignored_while_not_listening OK\n");
}

static void test_bflt_disabled_applies_edge_immediately(void) {
    psemu_t *ps = make_ps();

    psemu_bus_write32(&ps->bus, IRDA_MODE, RX_ACTIVE_MODE | IR_MODE_BFLT); /* filter disabled */
    ir_push_rx_edge(&ps->ir, ir_get_clock_cycles(&ps->ir), 1);
    ir_tick(&ps->ir, &ps->intc, 1u); /* one cycle is plenty: no debounce to wait out */
    assert(intc_irq_asserted(&ps->intc));

    psemu_destroy(ps);
    printf("test_bflt_disabled_applies_edge_immediately OK\n");
}

static void test_bflt_enabled_rejects_short_glitch(void) {
    psemu_t *ps = make_ps();
    uint64_t t0;

    psemu_bus_write32(&ps->bus, IRDA_MODE, RX_ACTIVE_MODE); /* BFLT bit clear: filter enabled */
    t0 = ir_get_clock_cycles(&ps->ir);

    /* A short ON pulse, and then an opposite edge immediately after it, must never get to rx_level or
       INT_IRDA. Both edges are inside the debounce window. */
    ir_push_rx_edge(&ps->ir, t0, 1);
    ir_push_rx_edge(&ps->ir, t0 + 1, 0);
    ir_tick(&ps->ir, &ps->intc, 5u);
    assert(!intc_irq_asserted(&ps->intc));

    psemu_destroy(ps);
    printf("test_bflt_enabled_rejects_short_glitch OK\n");
}

static void test_bflt_enabled_confirms_sustained_edge(void) {
    psemu_t *ps = make_ps();
    uint64_t t0;

    psemu_bus_write32(&ps->bus, IRDA_MODE, RX_ACTIVE_MODE); /* filter enabled */
    t0 = ir_get_clock_cycles(&ps->ir);

    ir_push_rx_edge(&ps->ir, t0, 1);
    ir_tick(&ps->ir, &ps->intc, 10u); /* well short of the debounce window: not confirmed yet */
    assert(!intc_irq_asserted(&ps->intc));

    ir_tick(&ps->ir, &ps->intc, 10000u); /* plenty more local time passes with no contradicting edge */
    assert(intc_irq_asserted(&ps->intc));

    psemu_destroy(ps);
    printf("test_bflt_enabled_confirms_sustained_edge OK\n");
}

static void test_full_loopback_tx_to_rx_across_two_instances(void) {
    /* Simulates the full two-instance sequence, with no real transport. Instance A transmits, and
       instance B receives the relayed edge. Instance B has its own psemu_t structure and its own
       clock. */
    psemu_t *tx = make_ps();
    psemu_t *rx = make_ps();
    ir_edge_t edge;

    psemu_bus_write32(&tx->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&rx->bus, IRDA_MODE, RX_ACTIVE_MODE | IR_MODE_BFLT);

    psemu_bus_write32(&tx->bus, IRDA_DATA, IR_DATA_LED);
    assert(ir_pop_tx_edge(&tx->ir, &edge) == 1);
    assert(edge.level == 1);

    /* The rx instance has its own independent clock. Deliver the edge at the current time on that
       timeline. A frontend that relays edges through a shared wall-clock reference does the same
       operation. */
    ir_push_rx_edge(&rx->ir, ir_get_clock_cycles(&rx->ir), edge.level);
    ir_tick(&rx->ir, &rx->intc, 0);

    assert(intc_irq_asserted(&rx->intc));
    assert(!intc_irq_asserted(&tx->intc)); /* the transmitter's own INTC is unaffected */

    psemu_destroy(tx);
    psemu_destroy(rx);
    printf("test_full_loopback_tx_to_rx_across_two_instances OK\n");
}

static void test_irda_misc_is_a_reserved_stub(void) {
    /* IRDA_MISC (+0xC) is unknown or reserved, and it has no known reset value and no known behavior.
       Thus this emulator has no basis to make register state for it. Reads return 0. Writes have no
       effect on this register, and no effect on other registers: mode and data do not change. See the
       top comment of ir.h. */
    psemu_t *ps = make_ps();

    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&ps->bus, PSEMU_IR_BASE + 0xCu, 0xFFFFFFFFu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_IR_BASE + 0xCu) == 0u);
    assert(ps->ir.mode == TX_ACTIVE_MODE); /* the write above did not disturb mode */

    /* The range at +0x8, between IRDA_DATA and IRDA_MISC, gets the same treatment. No available data
       separates it from IRDA_MISC. */
    psemu_bus_write32(&ps->bus, PSEMU_IR_BASE + 0x8u, 0xFFFFFFFFu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_IR_BASE + 0x8u) == 0u);

    psemu_destroy(ps);
    printf("test_irda_misc_is_a_reserved_stub OK\n");
}

static void test_tx_falling_edge_lands_later_than_rising(void) {
    /* IR_TX_FALL_STRETCH_CYCLES (ir.c) is a recorded concession. It is not a model of a physical
       effect. Only the falling edge is delayed. This test confirms the direction of that behavior, and
       that the behavior applies to only one edge. It does not use the exact constant.
       test_bflt_enabled_rejects_short_glitch tests IR_BFLT_DEBOUNCE_CYCLES the same way, by behavior
       and not by value. */
    psemu_t *ps = make_ps();
    ir_edge_t edge;
    uint64_t write_time;

    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    write_time = ir_get_clock_cycles(&ps->ir);

    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED); /* LED on: a rising edge */
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1);
    assert(edge.level == 1);
    assert(edge.timestamp_cycles == write_time); /* rising edges are never stretched */

    /* The pulse needs a real ON duration, because the stretch has a limit of that duration (see
       enqueue_tx_edge). This test writes to the bus directly, thus no CPU steps occur. Without this
       code, the IR clock never advances between the two writes, and the pulse has zero width. The real
       path does not have this condition: psemu_run calls ir_tick one time for each instruction, and
       the narrowest real pulse that this project measured is still 7 cycles wide. */
    ir_tick(&ps->ir, &ps->intc, 32u);

    psemu_bus_write32(&ps->bus, IRDA_DATA, 0); /* LED off: a falling edge */
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1);
    assert(edge.level == 0);
    /* Later than the moment it was written, which is now write_time + 32. */
    assert(edge.timestamp_cycles > write_time + 32u);

    psemu_destroy(ps);
    printf("test_tx_falling_edge_lands_later_than_rising OK\n");
}

static void test_tx_short_pulse_keeps_edges_in_order(void) {
    /* The order guard in enqueue_tx_edge (ir.c). Without that guard, a real pulse that is shorter than
       the stretch lets a delayed falling edge arrive after the rising edge that follows it. That
       result changes the order of the queue. This test causes the worst condition directly: three
       edges in sequence, with no ir_tick call between them. Thus the clock never advances, and the
       stretch of the falling edge has no real gap. */
    psemu_t *ps = make_ps();
    ir_edge_t first, second, third;

    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED); /* rising */
    psemu_bus_write32(&ps->bus, IRDA_DATA, 0);           /* falling, stretched into the future */
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED); /* rising again, still at the true (unstretched) time */

    assert(ir_pop_tx_edge(&ps->ir, &first) == 1 && first.level == 1);
    assert(ir_pop_tx_edge(&ps->ir, &second) == 1 && second.level == 0);
    assert(ir_pop_tx_edge(&ps->ir, &third) == 1 && third.level == 1);

    assert(second.timestamp_cycles >= first.timestamp_cycles);
    assert(third.timestamp_cycles >= second.timestamp_cycles); /* clamped: never lands before the falling edge */

    psemu_destroy(ps);
    printf("test_tx_short_pulse_keeps_edges_in_order OK\n");
}

static void test_tx_narrow_pulse_stretch_never_eats_the_following_gap(void) {
    /* The stretch has a limit of the ON duration of the pulse (enqueue_tx_edge in ir.c). Without that
       limit, a flat IR_TX_FALL_STRETCH_CYCLES applies to pulses that are much narrower than the
       constant. That result does not make a pulse longer. It moves the pulse onto the next pulse.
       One trading-card app is the real condition: pulses of approximately 7 cycles, with a space of
       205 or 406 cycles between them. The gap holds the bit, and not the pulse. A measurement over a
       real transfer showed that the flat stretch changed a 7-cycle ON and 205-cycle OFF pattern into a
       207-cycle ON and 5-cycle OFF pattern. It also made 272 gaps exactly 0 cycles. That result
       combined pairs of pulses, and the bits that those gaps encoded.
       This test causes that shape directly: a pulse that is much shorter than the stretch, and then a
       gap that is much longer than the stretch. The gap must stay a gap, and it must stay longer than
       the pulse. */
    psemu_t *ps = make_ps();
    ir_edge_t rise, fall, next_rise;
    const uint32_t on_cycles = 7u;   /* the real app's measured pulse width */
    const uint32_t off_cycles = 205u; /* and its measured short gap */

    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED); /* rising */
    ir_tick(&ps->ir, &ps->intc, on_cycles);
    psemu_bus_write32(&ps->bus, IRDA_DATA, 0); /* falling, stretch capped at on_cycles */
    ir_tick(&ps->ir, &ps->intc, off_cycles);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED); /* the next pulse's rising edge */

    assert(ir_pop_tx_edge(&ps->ir, &rise) == 1 && rise.level == 1);
    assert(ir_pop_tx_edge(&ps->ir, &fall) == 1 && fall.level == 0);
    assert(ir_pop_tx_edge(&ps->ir, &next_rise) == 1 && next_rise.level == 1);

    {
        uint64_t on_width = fall.timestamp_cycles - rise.timestamp_cycles;
        uint64_t gap = next_rise.timestamp_cycles - fall.timestamp_cycles;
        /* Stretched, but by no more than the pulse itself: never beyond double its true width. */
        assert(on_width >= on_cycles && on_width <= 2u * on_cycles);
        /* The gap keeps almost all of its real duration, and stays far wider than the pulse. */
        assert(gap >= off_cycles - on_cycles);
        assert(gap > on_width);
    }

    psemu_destroy(ps);
    printf("test_tx_narrow_pulse_stretch_never_eats_the_following_gap OK\n");
}

static void test_rx_queue_holds_a_full_message_without_dropping(void) {
    /* A real transmit burst of 41 bytes makes 658 edges (see the transmit-side analysis in
       tools/ir_probe.c). IR_EDGE_QUEUE_CAPACITY was 64 before. At that capacity, a real receiver got
       edges faster than it drained them. The queue filled almost immediately, and the full-queue guard
       in queue_push discarded 594 of the 658 edges before the decode operation. A count of the path of
       each edge through the receive side confirms this. It is not an inference. This test puts more
       than one full message of edges into the queue, with no ir_tick call between them. That is the
       exact condition that filled the real queue. It then drains each edge in one call. The last edge
       is the only edge with a different level. Thus, if this code discards any edge, rx_level has an
       incorrect value at the end. */
    psemu_t *ps = make_ps();
    uint64_t t0;
    const int message_edges = 700;
    int i;

    psemu_bus_write32(&ps->bus, IRDA_MODE, RX_ACTIVE_MODE | IR_MODE_BFLT); /* filter disabled: apply immediately */
    t0 = ir_get_clock_cycles(&ps->ir);

    for (i = 0; i < message_edges - 1; i++) {
        ir_push_rx_edge(&ps->ir, t0, 1); /* redundant after the first: still consumes a queue slot */
    }
    ir_push_rx_edge(&ps->ir, t0, 0); /* the one edge that must survive for this test to catch a drop */

    ir_tick(&ps->ir, &ps->intc, 0); /* drains the entire backlog in one call, exactly like a relay burst */

    assert(ps->ir.rx_level == 0);

    psemu_destroy(ps);
    printf("test_rx_queue_holds_a_full_message_without_dropping OK\n");
}

static void test_rx_status_level_goes_away_when_the_receiver_stops_listening(void) {
    /* The INT_IRDA level in STATUS goes away with the receive period that wrote it, because the
       receive path gives no signal during a transmission or in standby.
       A real app reads that level, and it needs the same condition at the start of each transfer.
       This test uses the value at reset as the reference. That value is 0.
       See ir_tick in core/src/ir.c. */
    psemu_t *ps = make_ps();

    assert((ps->intc.status & INT_IRDA) == 0u); /* the reference condition, before any IR traffic */

    psemu_bus_write32(&ps->bus, IRDA_MODE, RX_ACTIVE_MODE | IR_MODE_BFLT); /* receive, filter disabled */
    ir_push_rx_edge(&ps->ir, ir_get_clock_cycles(&ps->ir), 1); /* the carrier arrives: the level goes to 0 */
    ir_tick(&ps->ir, &ps->intc, 1u);
    assert((ps->intc.status & INT_IRDA) == 0u);

    ir_push_rx_edge(&ps->ir, ir_get_clock_cycles(&ps->ir), 0); /* the carrier goes: the idle level is 1 */
    ir_tick(&ps->ir, &ps->intc, 1u);
    assert((ps->intc.status & INT_IRDA) != 0u);

    /* The app replies. The receive path gives no signal during a transmission. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    ir_tick(&ps->ir, &ps->intc, 1u);
    assert((ps->intc.status & INT_IRDA) == 0u);
    assert((ps->intc.hold & INT_IRDA) != 0u); /* the latched request from the edges above stays */

    /* The next transfer starts in the same condition as the first transfer after a reset. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, RX_ACTIVE_MODE | IR_MODE_BFLT);
    ir_tick(&ps->ir, &ps->intc, 1u);
    assert((ps->intc.status & INT_IRDA) == 0u);

    /* Standby ends the receive period the same way. */
    ir_push_rx_edge(&ps->ir, ir_get_clock_cycles(&ps->ir), 1);
    ir_push_rx_edge(&ps->ir, ir_get_clock_cycles(&ps->ir), 0);
    ir_tick(&ps->ir, &ps->intc, 1u);
    assert((ps->intc.status & INT_IRDA) != 0u);
    psemu_bus_write32(&ps->bus, IRDA_MODE, IR_MODE_STDBY | IR_MODE_BFLT);
    ir_tick(&ps->ir, &ps->intc, 1u);
    assert((ps->intc.status & INT_IRDA) == 0u);

    psemu_destroy(ps);
    printf("test_rx_status_level_goes_away_when_the_receiver_stops_listening OK\n");
}

int main(void) {
    test_tx_write_with_carrier_enabled_enqueues_edges();
    test_tx_write_while_not_driving_produces_no_edge();
    test_tx_emits_with_carrier_generator_disabled();
    test_leaving_tx_active_forces_led_off_edge();
    test_loopback_edge_asserts_irda();
    test_rx_ignored_while_not_listening();
    test_bflt_disabled_applies_edge_immediately();
    test_bflt_enabled_rejects_short_glitch();
    test_bflt_enabled_confirms_sustained_edge();
    test_full_loopback_tx_to_rx_across_two_instances();
    test_irda_misc_is_a_reserved_stub();
    test_tx_falling_edge_lands_later_than_rising();
    test_tx_short_pulse_keeps_edges_in_order();
    test_tx_narrow_pulse_stretch_never_eats_the_following_gap();
    test_rx_queue_holds_a_full_message_without_dropping();
    test_rx_status_level_goes_away_when_the_receiver_stops_listening();
    printf("All IR tests passed.\n");
    return 0;
}
