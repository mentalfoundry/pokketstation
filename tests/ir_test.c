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

/* IFMODE=1 (transmit), STDBY=0 (active), BGEN=0 (carrier enabled - inverted logic), BFLT=1 (filter disabled,
   irrelevant on TX). */
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
    printf("All IR tests passed.\n");
    return 0;
}
