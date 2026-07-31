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

/* IFMODE=1 selects transmit. STDBY=0 keeps it active. BGEN=0 enables the carrier, because BGEN uses inverted
   logic. BFLT=1 disables the filter, which does not matter while transmitting. */
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

static void test_tx_write_without_carrier_produces_no_edge(void) {
    psemu_t *ps = make_ps();
    ir_edge_t edge;

    /* Standby set: no emission even though IFMODE=transmit. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, IR_MODE_IFMODE | IR_MODE_STDBY);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 0);

    /* Carrier generator disabled (BGEN=1): no emission either. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, IR_MODE_IFMODE | IR_MODE_BGEN);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 0);

    /* Receive mode (IFMODE=0): writes to DATA don't drive a TX LED at all. */
    psemu_bus_write32(&ps->bus, IRDA_MODE, 0);
    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED);
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 0);

    psemu_destroy(ps);
    printf("test_tx_write_without_carrier_produces_no_edge OK\n");
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

    ir_push_rx_edge(&ps->ir, ir_get_clock_cycles(&ps->ir), 1);
    ir_tick(&ps->ir, &ps->intc, 0);

    assert(intc_irq_asserted(&ps->intc));
    assert((psemu_bus_read32(&ps->bus, IRDA_DATA) & IR_DATA_LED) != 0); /* rx_level surfaced on DATA bit0 */

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

    /* A brief on-pulse immediately followed by a contradicting edge, both well inside the debounce window,
       must never reach rx_level/INT_IRDA. */
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
    /* Simulates the actual two-instance flow end to end, without any real transport: instance A transmits,
       instance B (its own separate psemu_t, own separate clock) receives the relayed edge. */
    psemu_t *tx = make_ps();
    psemu_t *rx = make_ps();
    ir_edge_t edge;

    psemu_bus_write32(&tx->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&rx->bus, IRDA_MODE, RX_ACTIVE_MODE | IR_MODE_BFLT);

    psemu_bus_write32(&tx->bus, IRDA_DATA, IR_DATA_LED);
    assert(ir_pop_tx_edge(&tx->ir, &edge) == 1);
    assert(edge.level == 1);

    /* rx has its own independent clock; deliver "now" on its own timeline, exactly as a frontend relaying
       via a shared wall-clock reference would. */
    ir_push_rx_edge(&rx->ir, ir_get_clock_cycles(&rx->ir), edge.level);
    ir_tick(&rx->ir, &rx->intc, 0);

    assert(intc_irq_asserted(&rx->intc));
    assert(!intc_irq_asserted(&tx->intc)); /* the transmitter's own INTC is unaffected */

    psemu_destroy(tx);
    psemu_destroy(rx);
    printf("test_full_loopback_tx_to_rx_across_two_instances OK\n");
}

static void test_irda_misc_is_a_reserved_stub(void) {
    /* IRDA_MISC (+0xC) is unknown or reserved in an external reference, with no documented reset value or
       behavior. This emulator has no basis to invent register state for it. Reads return 0. Writes have no
       effect on it, or on anything else: mode and data stay exactly as they were. See ir.h's top comment. */
    psemu_t *ps = make_ps();

    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    psemu_bus_write32(&ps->bus, PSEMU_IR_BASE + 0xCu, 0xFFFFFFFFu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_IR_BASE + 0xCu) == 0u);
    assert(ps->ir.mode == TX_ACTIVE_MODE); /* the write above did not disturb mode */

    /* The gap at +0x8, between IRDA_DATA and IRDA_MISC, gets the same stub treatment. Nothing distinguishes
       it from IRDA_MISC itself, externally or in this emulator. */
    psemu_bus_write32(&ps->bus, PSEMU_IR_BASE + 0x8u, 0xFFFFFFFFu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_IR_BASE + 0x8u) == 0u);

    psemu_destroy(ps);
    printf("test_irda_misc_is_a_reserved_stub OK\n");
}

static void test_tx_falling_edge_lands_later_than_rising(void) {
    /* IR_TX_FALL_STRETCH_CYCLES (ir.c): a documented concession, not a modeled physical effect. Only the
       falling edge is delayed. This checks the direction and asymmetry of that behavior without hardcoding
       the exact constant, the same way test_bflt_enabled_rejects_short_glitch checks IR_BFLT_DEBOUNCE_CYCLES
       by behavior instead of by value. */
    psemu_t *ps = make_ps();
    ir_edge_t edge;
    uint64_t write_time;

    psemu_bus_write32(&ps->bus, IRDA_MODE, TX_ACTIVE_MODE);
    write_time = ir_get_clock_cycles(&ps->ir);

    psemu_bus_write32(&ps->bus, IRDA_DATA, IR_DATA_LED); /* LED on: a rising edge */
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1);
    assert(edge.level == 1);
    assert(edge.timestamp_cycles == write_time); /* rising edges are never stretched */

    psemu_bus_write32(&ps->bus, IRDA_DATA, 0); /* LED off: a falling edge, at the same clock_cycles */
    assert(ir_pop_tx_edge(&ps->ir, &edge) == 1);
    assert(edge.level == 0);
    assert(edge.timestamp_cycles > write_time); /* falling edges land later than when they were written */

    psemu_destroy(ps);
    printf("test_tx_falling_edge_lands_later_than_rising OK\n");
}

static void test_tx_short_pulse_keeps_edges_in_order(void) {
    /* The monotonicity guard in enqueue_tx_edge (ir.c): a real pulse shorter than the stretch would
       otherwise let a delayed falling edge land after the rising edge that follows it, reordering the
       queue. This drives the worst case directly: three edges written back to back, with no ir_tick
       between any of them, so the clock never advances and the falling edge's stretch has zero real gap
       to work with. */
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

int main(void) {
    test_tx_write_with_carrier_enabled_enqueues_edges();
    test_tx_write_without_carrier_produces_no_edge();
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
    printf("All IR tests passed.\n");
    return 0;
}
