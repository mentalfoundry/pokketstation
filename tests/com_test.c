/* See the comment at the top of cpu_test.c. NDEBUG in a Release build removes each assert() call, thus
   this test suite must keep them. This code must come before <assert.h>. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>

#include "psemu_internal.h"
#include "state.h"

#include <stdlib.h>

/* These tests cover the register model of the communication port (core/src/com.h). They do not cover
   the memory card protocol, because this repository does not contain the protocol. The BIOS of the
   emulated machine holds it. .gitignore also excludes each BIOS dump (see testdata/).

   tools/com_probe.c verifies the protocol against a real dump. That tool confirms the full Get ID
   command (0x53), the full Read Sector command (0x52), and the PocketStation command 0x5A. Each
   behavior below comes from that verification. Thus these tests keep the findings that a machine
   without a BIOS can still test. See docs/hardware-notes.md, "Communication port". */

#define COM_MODE (PSEMU_COM_BASE + COM_MODE_OFFSET)
#define COM_STAT1 (PSEMU_COM_BASE + COM_STAT1_OFFSET)
#define COM_DATA (PSEMU_COM_BASE + COM_DATA_OFFSET)
#define COM_CTRL1 (PSEMU_COM_BASE + COM_CTRL1_OFFSET)
#define COM_STAT2 (PSEMU_COM_BASE + COM_STAT2_OFFSET)
#define COM_CTRL2 (PSEMU_COM_BASE + COM_CTRL2_OFFSET)

static void test_registers_read_back(void) {
    psemu_t *ps = psemu_create();

    psemu_bus_write32(&ps->bus, COM_MODE, 0x06u);
    psemu_bus_write32(&ps->bus, COM_CTRL1, 0x02u);
    psemu_bus_write32(&ps->bus, COM_CTRL2, 0x03u);
    assert(psemu_bus_read32(&ps->bus, COM_MODE) == 0x06u);
    assert(psemu_bus_read32(&ps->bus, COM_CTRL1) == 0x02u);
    assert(psemu_bus_read32(&ps->bus, COM_CTRL2) == 0x03u);

    /* Both status registers refuse a write. */
    psemu_bus_write32(&ps->bus, COM_STAT1, 0xFFu);
    assert(psemu_bus_read32(&ps->bus, COM_STAT1) == 0u);

    psemu_destroy(ps);
    printf("test_registers_read_back OK\n");
}

static void test_unknown_offsets_are_reserved_stubs(void) {
    psemu_t *ps = psemu_create();

    /* A published register map records +0x0C and +0x1C as zero. ir_read gives IRDA_MISC the same
       treatment. */
    psemu_bus_write32(&ps->bus, PSEMU_COM_BASE + 0x0Cu, 0xFFu);
    psemu_bus_write32(&ps->bus, PSEMU_COM_BASE + 0x1Cu, 0xFFu);
    assert(psemu_bus_read32(&ps->bus, PSEMU_COM_BASE + 0x0Cu) == 0u);
    assert(psemu_bus_read32(&ps->bus, PSEMU_COM_BASE + 0x1Cu) == 0u);

    psemu_destroy(ps);
    printf("test_unknown_offsets_are_reserved_stubs OK\n");
}

static void test_docking_drives_a_level_in_status(void) {
    psemu_t *ps = psemu_create();

    assert(psemu_com_get_docked(ps) == 0);
    assert((ps->intc.status & INT_IOP) == 0u);

    psemu_com_set_docked(ps, 1);
    assert(psemu_com_get_docked(ps) == 1);
    /* The kernel reads this live level from INT_INPUT during a transfer, to find an undock event
       while that transfer is in progress. Thus the source must reach STATUS, and not only HOLD. */
    assert((ps->intc.status & INT_IOP) != 0u);
    assert((ps->intc.hold & INT_IOP) != 0u);

    /* An acknowledge must not clear a level. If it did, each dock event would read back as an
       undock event. INT_IOP is in INT_LEVEL_MASK for this reason. */
    ps->intc.enable |= INT_IOP;
    psemu_bus_write32(&ps->bus, PSEMU_INTC_BASE + 0x10u, INT_IOP);
    assert((ps->intc.status & INT_IOP) != 0u);

    psemu_com_set_docked(ps, 0);
    assert(psemu_com_get_docked(ps) == 0);
    assert((ps->intc.status & INT_IOP) == 0u);

    psemu_destroy(ps);
    printf("test_docking_drives_a_level_in_status OK\n");
}

static void test_transfer_shifts_the_held_byte_out(void) {
    psemu_t *ps = psemu_create();
    uint8_t out = 0x00;

    /* This block is a shift register. The console receives the byte of exchange N during exchange
       N+1. A trace of a real BIOS confirms this behavior. The kernel writes FLAG while it processes
       0x81. A published command table gives the reply to 0x81 as "N/A". It gives the reply to 0x53
       as FLAG.
       This test loads no BIOS, thus no code answers. The held value must still go out. */
    psemu_bus_write32(&ps->bus, COM_DATA, 0x5Au);
    com_begin_transfer(&ps->com, &ps->intc, 0x81u);
    out = com_take_reply(&ps->com);
    assert(out == 0x5Au);

    /* The byte from the console is available at COM_DATA. */
    assert(psemu_bus_read32(&ps->bus, COM_DATA) == 0x81u);

    com_end_transfer(&ps->com, &ps->intc);
    psemu_destroy(ps);
    printf("test_transfer_shifts_the_held_byte_out OK\n");
}

static void test_ready_bit_reports_an_arrived_byte(void) {
    psemu_t *ps = psemu_create();

    assert(psemu_bus_read32(&ps->bus, COM_STAT2) == 0u);

    com_begin_transfer(&ps->com, &ps->intc, 0x52u);
    assert((psemu_bus_read32(&ps->bus, COM_STAT2) & COM_STAT2_READY) != 0u);
    assert((ps->intc.hold & INT_COM) != 0u);

    /* A read of COM_DATA consumes the byte. The Ready bit and the interrupt request both clear. */
    (void)psemu_bus_read32(&ps->bus, COM_DATA);
    assert(psemu_bus_read32(&ps->bus, COM_STAT2) == 0u);
    assert((ps->intc.hold & INT_COM) == 0u);

    com_end_transfer(&ps->com, &ps->intc);
    psemu_destroy(ps);
    printf("test_ready_bit_reports_an_arrived_byte OK\n");
}

static void test_acknowledge_ends_the_exchange_without_a_data_read(void) {
    psemu_t *ps = psemu_create();

    /* A trace of a real BIOS found this case. No other event can replace it. The kernel handles a
       full command inside one FIQ. It polls COM_STAT2 for each byte after the first byte. The data
       phase of a command receives only dummy bytes. Thus the kernel never reads COM_DATA during that
       phase. The Ready bit must still clear at the acknowledge.
       Without this behavior, the poll loop of the kernel read a byte that never arrived. It then ran
       through the remainder of the command inside one exchange. */
    com_begin_transfer(&ps->com, &ps->intc, 0x00u);
    assert((psemu_bus_read32(&ps->bus, COM_STAT2) & COM_STAT2_READY) != 0u);
    assert(com_transfer_acked(&ps->com) == 0);

    /* The kernel drives the data line, and then it pulls /ACK LOW. This is the sequence that the
       real handler uses: COM_MODE = 1, and then COM_MODE = 3. */
    psemu_bus_write32(&ps->bus, COM_MODE, COM_MODE_OUT_ENABLE);
    assert(com_transfer_acked(&ps->com) == 0);
    psemu_bus_write32(&ps->bus, COM_MODE, COM_MODE_OUT_ENABLE | COM_MODE_ACK_LOW);
    assert(com_transfer_acked(&ps->com) == 1);

    /* No read of COM_DATA occurred, and the Ready bit is clear. */
    assert(psemu_bus_read32(&ps->bus, COM_STAT2) == 0u);
    assert((ps->intc.hold & INT_COM) == 0u);

    com_end_transfer(&ps->com, &ps->intc);
    assert(com_transfer_acked(&ps->com) == 0);

    psemu_destroy(ps);
    printf("test_acknowledge_ends_the_exchange_without_a_data_read OK\n");
}

static void test_sel_release_sets_the_end_of_command_bit(void) {
    psemu_t *ps = psemu_create();

    /* The /SEL line of the connector. A console holds it for one command, and it releases the line
       between commands. The kernel waits for that release after the last byte of a command, at
       0x040007B8 in the J110 revision. A test against a real dump confirms COM_STAT1 bit 1, and it
       rejects each of bits 2 to 7. Without this signal the kernel answers one command and then
       answers nothing. See tools/com_probe.c, the selbit mode. */
    psemu_com_set_selected(ps, 1);
    assert((psemu_bus_read32(&ps->bus, COM_STAT1) & COM_STAT1_ERROR) == 0u);

    psemu_com_set_selected(ps, 0);
    assert((psemu_bus_read32(&ps->bus, COM_STAT1) & COM_STAT1_ERROR) != 0u);

    /* A read of the register clears the latch. The published register map records a dummy read of
       this register by the kernel at the end of each transfer. It gives a hardware clear at a read
       as one candidate reason for that dummy read. */
    assert((psemu_bus_read32(&ps->bus, COM_STAT1) & COM_STAT1_ERROR) == 0u);

    /* A release with no hold before it sets nothing. */
    psemu_com_set_selected(ps, 0);
    assert((psemu_bus_read32(&ps->bus, COM_STAT1) & COM_STAT1_ERROR) == 0u);

    psemu_destroy(ps);
    printf("test_sel_release_sets_the_end_of_command_bit OK\n");
}

static void test_stat1_bit0_follows_an_arrived_byte(void) {
    psemu_t *ps = psemu_create();

    /* The write path of the kernel polls COM_STAT1 in place of COM_STAT2, at 0x040015D6. Thus bit 0
       must give the same condition as the Ready bit of COM_STAT2. A model that held the bit clear
       stopped that path after one data byte. A model that held the bit always set made the kernel
       read one byte many times, and Write Sector then failed with 0x4E. */
    assert((psemu_bus_read32(&ps->bus, COM_STAT1) & 1u) == 0u);

    com_begin_transfer(&ps->com, &ps->intc, 0x5Au);
    assert((psemu_bus_read32(&ps->bus, COM_STAT1) & 1u) != 0u);
    assert((psemu_bus_read32(&ps->bus, COM_STAT2) & COM_STAT2_READY) != 0u);

    (void)psemu_bus_read32(&ps->bus, COM_DATA);
    assert((psemu_bus_read32(&ps->bus, COM_STAT1) & 1u) == 0u);

    com_end_transfer(&ps->com, &ps->intc);
    psemu_destroy(ps);
    printf("test_stat1_bit0_follows_an_arrived_byte OK\n");
}

static void test_transfer_without_a_bios_reports_no_acknowledge(void) {
    psemu_t *ps = psemu_create();
    uint8_t out = 0x00;

    /* No code answers without a BIOS. A caller must get the same result that a console gets for an
       empty slot. The caller must also not wait without a limit. */
    int ack = psemu_com_transfer(ps, 0x81u, &out, 4096u);
    assert(ack == 0);
    assert(out == 0xFFu);

    psemu_destroy(ps);
    printf("test_transfer_without_a_bios_reports_no_acknowledge OK\n");
}

static void test_reset_clears_the_port(void) {
    psemu_t *ps = psemu_create();

    psemu_com_set_docked(ps, 1);
    psemu_com_set_selected(ps, 1);
    psemu_bus_write32(&ps->bus, COM_MODE, 0x06u);
    psemu_bus_write32(&ps->bus, COM_CTRL1, 0x02u);
    com_begin_transfer(&ps->com, &ps->intc, 0x81u);

    psemu_reset(ps);

    assert(psemu_bus_read32(&ps->bus, COM_MODE) == 0u);
    assert(psemu_bus_read32(&ps->bus, COM_CTRL1) == 0u);
    assert(psemu_bus_read32(&ps->bus, COM_STAT2) == 0u);
    assert(psemu_com_get_docked(ps) == 0);
    assert(com_transfer_acked(&ps->com) == 0);
    assert(ps->com.selected == 0);
    assert((psemu_bus_read32(&ps->bus, COM_STAT1) & COM_STAT1_ERROR) == 0u);

    psemu_destroy(ps);
    printf("test_reset_clears_the_port OK\n");
}

/* The save-state format. These tests are here because com_test already reaches the internal
   structure of the machine. See core/src/state.c for the format. */

static void test_state_round_trip_keeps_the_machine(void) {
    psemu_t *a = psemu_create();
    psemu_t *b = psemu_create();
    size_t size = psemu_state_size(a);
    uint8_t *buf = (uint8_t *)malloc(size);
    unsigned i;

    assert(buf != NULL);

    /* Put a value into each region that the format covers. A round trip must return each one. */
    a->cpu.r[3] = 0xDEADBEEFu;
    a->cpu.r13_bank[2] = 0x11112222u;
    a->cpu.r8_12_bank[1][4] = 0x33334444u;
    a->cpu.total_steps = 1234567890123ull;
    a->bus.ram[0x123] = 0x5Au;
    a->lcd.vram[7] = 0xA5u;
    a->intc.enable = 0x00001234u;
    psemu_flash_data(a)[0x4321] = 0x77u;
    a->flash.f_sn_lo = 0xBEEFu;
    psemu_com_set_docked(a, 1);
    psemu_com_set_selected(a, 1);
    a->com.tx_data = 0x5Cu;
    a->ir.clock_cycles = 999999ull;
    ir_push_rx_edge(&a->ir, 4242ull, 1);
    ir_push_rx_edge(&a->ir, 4343ull, 0);
    a->timer.timers[2].period = 0x0353u;
    a->rtc.date = 0x20260806u;
    a->dac.current_sample = -1234;
    a->clk.mode = 7u;
    a->iop.data = 0x20u;
    a->buttons = PSEMU_BUTTON_FIRE;
    a->app_running = 1;
    a->real_time_cycle_carry = 0.5;

    assert(psemu_save_state(a, buf, size) == PSEMU_OK);
    assert(psemu_load_state(b, buf, size) == PSEMU_OK);

    assert(b->cpu.r[3] == 0xDEADBEEFu);
    assert(b->cpu.r13_bank[2] == 0x11112222u);
    assert(b->cpu.r8_12_bank[1][4] == 0x33334444u);
    assert(b->cpu.total_steps == 1234567890123ull);
    assert(b->bus.ram[0x123] == 0x5Au);
    assert(b->lcd.vram[7] == 0xA5u);
    assert(b->intc.enable == 0x00001234u);
    assert(psemu_flash_data(b)[0x4321] == 0x77u);
    assert(b->flash.f_sn_lo == 0xBEEFu);
    assert(psemu_com_get_docked(b) == 1);
    assert(b->com.selected == 1);
    assert(b->com.tx_data == 0x5Cu);
    assert(b->ir.clock_cycles == 999999ull);
    assert(b->ir.rx_queue.count == 2);
    assert(b->ir.rx_queue.entries[0].timestamp_cycles == 4242ull);
    assert(b->ir.rx_queue.entries[0].level == 1);
    assert(b->ir.rx_queue.entries[1].timestamp_cycles == 4343ull);
    assert(b->ir.rx_queue.entries[1].level == 0);
    assert(b->timer.timers[2].period == 0x0353u);
    assert(b->rtc.date == 0x20260806u);
    assert(b->dac.current_sample == -1234);
    assert(b->clk.mode == 7u);
    assert(b->iop.data == 0x20u);
    assert(b->buttons == PSEMU_BUTTON_FIRE);
    assert(b->app_running == 1);
    assert(b->real_time_cycle_carry > 0.49 && b->real_time_cycle_carry < 0.51);

    /* The bus pointers of the loaded instance must address that instance, and not the instance that
       supplied the state. */
    assert(b->bus.flash == &b->flash);
    assert(b->bus.com == &b->com);
    assert(b->cpu.bus == &b->bus);

    /* A write through the loaded instance must not reach the instance that supplied the state. */
    for (i = 0; i < 8u; i++) {
        psemu_bus_write8(&b->bus, 0x200u + i, (uint8_t)(0xC0u + i));
    }
    assert(a->bus.ram[0x200] != 0xC0u);

    free(buf);
    psemu_destroy(a);
    psemu_destroy(b);
    printf("test_state_round_trip_keeps_the_machine OK\n");
}

static void test_state_refuses_a_bad_header(void) {
    psemu_t *ps = psemu_create();
    size_t size = psemu_state_size(ps);
    uint8_t *buf = (uint8_t *)malloc(size);

    assert(buf != NULL);
    assert(psemu_save_state(ps, buf, size) == PSEMU_OK);

    /* A file that carries a different magic number. */
    buf[0] = 'X';
    assert(psemu_load_state(ps, buf, size) == PSEMU_ERR_BAD_FORMAT);
    buf[0] = 'P';

    /* A file that carries a different version. The core refuses it, thus a frontend does not have to
       track the layout of psemu_t. */
    buf[4] = (uint8_t)(PSEMU_STATE_VERSION + 1u);
    assert(psemu_load_state(ps, buf, size) == PSEMU_ERR_BAD_FORMAT);
    buf[4] = (uint8_t)PSEMU_STATE_VERSION;

    /* A file that is too short. */
    assert(psemu_load_state(ps, buf, size - 1u) == PSEMU_ERR_BAD_SIZE);
    assert(psemu_save_state(ps, buf, size - 1u) == PSEMU_ERR_BAD_SIZE);

    /* The correct file still loads after each refusal above. */
    assert(psemu_load_state(ps, buf, size) == PSEMU_OK);

    free(buf);
    psemu_destroy(ps);
    printf("test_state_refuses_a_bad_header OK\n");
}

static void test_state_holds_no_bios_and_is_smaller_than_the_structure(void) {
    psemu_t *ps = psemu_create();
    size_t size = psemu_state_size(ps);
    uint8_t *buf = (uint8_t *)malloc(size);
    uint8_t bios[PSEMU_BIOS_SIZE];
    size_t i;
    size_t run = 0;
    size_t longest = 0;

    assert(buf != NULL);
    /* A BIOS of one repeated byte. A search of the state for a long run of that byte then finds a
       copy of the BIOS, if the format holds one. */
    memset(bios, 0x6Du, sizeof(bios));
    assert(psemu_load_bios(ps, bios, sizeof(bios)) == PSEMU_OK);
    assert(psemu_save_state(ps, buf, size) == PSEMU_OK);

    for (i = 0; i < size; i++) {
        run = (buf[i] == 0x6Du) ? run + 1 : 0;
        if (run > longest) {
            longest = run;
        }
    }
    /* A state that holds the BIOS has a run of PSEMU_BIOS_SIZE bytes. This format holds none. */
    assert(longest < PSEMU_BIOS_SIZE);

    /* The format is also much smaller than the structure. The structure carries the BIOS, the
       diagnostic trace ring of the CPU, and both IR queues at their full capacity. */
    assert(size < sizeof(psemu_t));

    free(buf);
    psemu_destroy(ps);
    printf("test_state_holds_no_bios_and_is_smaller_than_the_structure OK\n");
}

int main(void) {
    test_registers_read_back();
    test_unknown_offsets_are_reserved_stubs();
    test_docking_drives_a_level_in_status();
    test_transfer_shifts_the_held_byte_out();
    test_ready_bit_reports_an_arrived_byte();
    test_acknowledge_ends_the_exchange_without_a_data_read();
    test_sel_release_sets_the_end_of_command_bit();
    test_stat1_bit0_follows_an_arrived_byte();
    test_transfer_without_a_bios_reports_no_acknowledge();
    test_reset_clears_the_port();
    test_state_round_trip_keeps_the_machine();
    test_state_refuses_a_bad_header();
    test_state_holds_no_bios_and_is_smaller_than_the_structure();
    printf("All COM tests passed.\n");
    return 0;
}
