/* See the comment at the top of cpu_test.c. NDEBUG in a Release build removes each assert() call, thus
   this test suite must keep them. This code must come before <assert.h>. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>

#include "psemu_internal.h"

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
    psemu_bus_write32(&ps->bus, COM_MODE, 0x06u);
    psemu_bus_write32(&ps->bus, COM_CTRL1, 0x02u);
    com_begin_transfer(&ps->com, &ps->intc, 0x81u);

    psemu_reset(ps);

    assert(psemu_bus_read32(&ps->bus, COM_MODE) == 0u);
    assert(psemu_bus_read32(&ps->bus, COM_CTRL1) == 0u);
    assert(psemu_bus_read32(&ps->bus, COM_STAT2) == 0u);
    assert(psemu_com_get_docked(ps) == 0);
    assert(com_transfer_acked(&ps->com) == 0);

    psemu_destroy(ps);
    printf("test_reset_clears_the_port OK\n");
}

int main(void) {
    test_registers_read_back();
    test_unknown_offsets_are_reserved_stubs();
    test_docking_drives_a_level_in_status();
    test_transfer_shifts_the_held_byte_out();
    test_ready_bit_reports_an_arrived_byte();
    test_acknowledge_ends_the_exchange_without_a_data_read();
    test_transfer_without_a_bios_reports_no_acknowledge();
    test_reset_clears_the_port();
    printf("All COM tests passed.\n");
    return 0;
}
