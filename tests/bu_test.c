/* SPDX-FileCopyrightText: Copyright (c) 2026 Darien Liu (mentalfoundry)
   SPDX-License-Identifier: MIT */

/* See the comment at the top of cpu_test.c. NDEBUG in a Release build removes each assert() call,
   thus this test suite must keep them. This code must come before <assert.h>. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mock_ps1.h"

/* These tests cover the memory card protocol at the connector. com_test.c cannot cover the protocol,
   because the kernel of the emulated machine holds it. tests/mock_ps1.c gives the PS1 half of the
   link, thus these tests can send a command and examine the answer.

   THESE TESTS NEED A BIOS DUMP. The repository excludes each dump (see testdata/). This suite skips
   itself with exit code 77 when it finds no dump. CTest reports that code as a skip. Set
   PSEMU_TEST_BIOS to use a different path.

   Each test prints the reply stream that it examined. A command has a fixed layout, but the shift of
   the output register moves the whole stream by one position. Thus each check anchors on the two
   identifier bytes, and the printed stream shows the anchor.

   Three behaviors separate a PocketStation from an original Sony memory card at the three standard
   commands. Two of them are here: the 0x00 dummy bytes of a read, and the refusal codes of a write.
   The third is the timing of a read, which a PS1 emulator observes and this suite does not. */

#define SKIP_EXIT_CODE 77

/* A data frame of the card. It is not a directory frame, thus a write to it needs no card image and
   the kernel refuses nothing. */
#define DATA_SECTOR 64u

/* The frames that the kernel needs to program flash after a Write Sector command ends. */
#define FLASH_SETTLE_FRAMES 30u

static const char *bios_path(void) {
    const char *env = getenv("PSEMU_TEST_BIOS");

    if (env && env[0]) {
        return env;
    }
    return PSEMU_TESTDATA_DIR "/J110.bin";
}

static void print_stream(const char *label, const uint8_t *reply, size_t count) {
    size_t i;

    printf("  %s (%u bytes):", label, (unsigned)count);
    for (i = 0; i < count; i++) {
        if ((i % 16u) == 0u) {
            printf("\n   %3u:", (unsigned)i);
        }
        printf(" %02X", (unsigned)reply[i]);
    }
    printf("\n");
}

/* A pattern that empty flash does not hold, thus a read back cannot pass by accident. */
static void fill_pattern(uint8_t *data, uint8_t seed) {
    unsigned i;

    for (i = 0; i < MOCK_PS1_FRAME_SIZE; i++) {
        data[i] = (uint8_t)(seed ^ (i * 7u));
    }
}

/* Command 0x53 gives exactly the values of an original Sony memory card. A published note records
   that this command needs no special handling on a PocketStation. Thus this test is the check that
   the device stays compatible with a PS1 program that knows nothing about a PocketStation. */
static void test_get_id_gives_the_values_of_a_sony_card(mock_ps1_t *m) {
    uint8_t reply[10];
    size_t n;
    size_t id;

    memset(reply, 0, sizeof(reply));
    n = mock_ps1_get_id(m, reply);
    mock_ps1_end_command(m);

    print_stream("Get ID reply", reply, n);

    id = mock_ps1_find_id_pair(reply, n);
    assert(id != MOCK_PS1_NOT_FOUND);
    assert(id + 8u <= n);

    /* The two acknowledge bytes, and then the four identifier bytes of a 128KB card. */
    assert(reply[id + 2u] == 0x5Cu);
    assert(reply[id + 3u] == 0x5Du);
    assert(reply[id + 4u] == 0x04u);
    assert(reply[id + 5u] == 0x00u);
    assert(reply[id + 6u] == 0x00u);
    assert(reply[id + 7u] == 0x80u);

    printf("test_get_id_gives_the_values_of_a_sony_card OK\n");
}

/* A PS1 sends many commands in sequence. The kernel learns that a command ended from the release of
   the select line, thus a caller must release that line and then run the machine. Without the
   release the kernel answers one command and then answers nothing.
   This test sends the same command two times. It is the check that mock_ps1_end_command gives the
   kernel what it needs. A port that fails this check answers its first command only. */
static void test_a_second_command_answers(mock_ps1_t *m) {
    uint8_t first[10];
    uint8_t second[10];
    size_t n1;
    size_t n2;

    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));

    n1 = mock_ps1_get_id(m, first);
    mock_ps1_end_command(m);
    n2 = mock_ps1_get_id(m, second);
    mock_ps1_end_command(m);

    print_stream("first Get ID", first, n1);
    print_stream("second Get ID", second, n2);

    assert(mock_ps1_find_id_pair(first, n1) != MOCK_PS1_NOT_FOUND);
    assert(mock_ps1_find_id_pair(second, n2) != MOCK_PS1_NOT_FOUND);

    printf("test_a_second_command_answers OK\n");
}

/* Command 0x57 writes one frame. The terminator reports the result, and flash is the independent
   check. A PS1 emulator makes this command more than each other command, thus it is the command
   that a port must get correct first. */
static void test_write_sector_puts_the_data_in_flash(mock_ps1_t *m) {
    uint8_t data[MOCK_PS1_FRAME_SIZE];
    uint8_t reply[MOCK_PS1_WRITE_REPLY_SIZE];
    uint8_t term = 0xFFu;
    const uint8_t *flash;
    size_t n;

    fill_pattern(data, 0xA5u);
    memset(reply, 0, sizeof(reply));

    n = mock_ps1_write_sector(m, DATA_SECTOR, data, 0, &term, reply);
    mock_ps1_end_command(m);
    mock_ps1_run_frames(m, FLASH_SETTLE_FRAMES);

    print_stream("Write Sector reply tail", &reply[n > 12u ? n - 12u : 0u], n > 12u ? 12u : n);
    printf("  terminator 0x%02X\n", (unsigned)term);

    assert(term == MOCK_PS1_TERM_GOOD);

    flash = psemu_flash_data(m->ps);
    assert(memcmp(&flash[(size_t)DATA_SECTOR * MOCK_PS1_FRAME_SIZE], data, MOCK_PS1_FRAME_SIZE) == 0);

    printf("test_write_sector_puts_the_data_in_flash OK\n");
}

/* An incorrect checksum makes the kernel refuse the write. The terminator gives "N", and flash does
   not change. */
static void test_write_sector_refuses_a_bad_checksum(mock_ps1_t *m) {
    static uint8_t before[PSEMU_FLASH_SIZE];
    uint8_t data[MOCK_PS1_FRAME_SIZE];
    uint8_t term = 0xFFu;
    const uint8_t *flash;

    fill_pattern(data, 0x3Cu);
    memcpy(before, psemu_flash_data(m->ps), PSEMU_FLASH_SIZE);

    (void)mock_ps1_write_sector(m, DATA_SECTOR, data, 1, &term, NULL);
    mock_ps1_end_command(m);
    mock_ps1_run_frames(m, FLASH_SETTLE_FRAMES);

    printf("  terminator 0x%02X\n", (unsigned)term);
    assert(term == MOCK_PS1_TERM_BAD_CHECKSUM);

    flash = psemu_flash_data(m->ps);
    assert(memcmp(flash, before, PSEMU_FLASH_SIZE) == 0);

    printf("test_write_sector_refuses_a_bad_checksum OK\n");
}

/* Command 0x52 gives the data that command 0x57 wrote. This test runs after the write test, thus the
   sector holds the pattern of that test. */
static void test_read_sector_gives_the_written_data(mock_ps1_t *m) {
    uint8_t expect[MOCK_PS1_FRAME_SIZE];
    uint8_t got[MOCK_PS1_FRAME_SIZE];
    uint8_t reply[MOCK_PS1_READ_REPLY_SIZE];
    size_t n;

    fill_pattern(expect, 0xA5u);
    memset(reply, 0, sizeof(reply));

    n = mock_ps1_read_sector(m, DATA_SECTOR, got, reply);
    mock_ps1_end_command(m);

    print_stream("Read Sector reply head", reply, n < 16u ? n : 16u);

    assert(mock_ps1_find_id_pair(reply, n) != MOCK_PS1_NOT_FOUND);
    assert(memcmp(got, expect, MOCK_PS1_FRAME_SIZE) == 0);

    printf("test_read_sector_gives_the_written_data OK\n");
}

/* A PocketStation gives 0x00 in the two dummy positions of a read. An original Sony card gives the
   "(pre)" dummies there. A port must not send the Sony dummies from a PocketStation. */
static void test_read_sector_dummy_bytes_are_zero(mock_ps1_t *m) {
    uint8_t reply[MOCK_PS1_READ_REPLY_SIZE];
    size_t n;
    size_t id;

    memset(reply, 0, sizeof(reply));
    n = mock_ps1_read_sector(m, DATA_SECTOR, NULL, reply);
    mock_ps1_end_command(m);

    id = mock_ps1_find_id_pair(reply, n);
    assert(id != MOCK_PS1_NOT_FOUND);
    assert(id + MOCK_PS1_READ_DUMMY_OFFSET + 2u <= n);

    printf("  dummy bytes 0x%02X 0x%02X\n", (unsigned)reply[id + MOCK_PS1_READ_DUMMY_OFFSET],
        (unsigned)reply[id + MOCK_PS1_READ_DUMMY_OFFSET + 1u]);

    assert(reply[id + MOCK_PS1_READ_DUMMY_OFFSET] == 0x00u);
    assert(reply[id + MOCK_PS1_READ_DUMMY_OFFSET + 1u] == 0x00u);

    /* The two acknowledge bytes follow the dummy bytes. This check confirms the anchor, thus a
       failure above is a real difference and not a moved stream. */
    assert(reply[id + MOCK_PS1_READ_ACK_OFFSET] == 0x5Cu);
    assert(reply[id + MOCK_PS1_READ_ACK_OFFSET + 1u] == 0x5Du);

    printf("test_read_sector_dummy_bytes_are_zero OK\n");
}

/* The two refusal codes of a write need a card that this suite does not make.
   0xFD needs a running file, because the kernel refuses a write to the directory entries of that
   file. A test for it must load an app, start it with command 0x59, and then write to the directory
   frame of that app.
   0xFE needs ComFlags bit 10, because that bit enables the refusal for the broken-sector region.
   Command 0x5E sends that bit. The region is sector 16 to 55.
   tools/com_probe.c write mode already reports both codes against a real card. */

/* psemu_com_transfer_timed gives the reference cycles that each exchange ran. A caller that keeps
   the machine at the time of a host clock needs that number, and a caller that times the
   acknowledge for the host needs it too.

   This test sends Get ID two times. An answer costs much less than the budget, and a missing
   acknowledge on the last byte costs the full budget. The two commands are identical, thus the
   cost of each byte must agree between them, to one reference cycle: the number comes from the
   machine, and not from the host. */
static void test_a_timed_transfer_reports_the_cost_of_each_answer(void) {
    mock_ps1_t *m = mock_ps1_open(bios_path(), NULL);
    const uint8_t send[10] = {0x81u, 0x53u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint32_t cost[2][10];
    unsigned pass, i;

    assert(m != NULL);
    for (pass = 0; pass < 2u; pass++) {
        for (i = 0; i < 10u; i++) {
            uint8_t out = 0u;
            int ack = psemu_com_transfer_timed(m->ps, send[i], &out, PSEMU_COM_DEFAULT_TIMEOUT_CYCLES,
                                               &cost[pass][i]);
            if (i < 9u) {
                assert(ack);
                assert(cost[pass][i] > 0u && cost[pass][i] < 1024u);
            } else {
                assert(!ack);
                assert(cost[pass][i] >= PSEMU_COM_DEFAULT_TIMEOUT_CYCLES);
            }
        }
        mock_ps1_end_command(m);
    }

    printf("  cost of each byte:");
    for (i = 0; i < 10u; i++) {
        printf(" %u", (unsigned)cost[0][i]);
        /* psemu_run counts the reference-cycle boundaries that the clock crosses, and an exchange can
           start between two boundaries. Thus the same exchange can report one cycle more or less. */
        assert(cost[0][i] + 1u >= cost[1][i] && cost[1][i] + 1u >= cost[0][i]);
    }
    printf("\n");

    mock_ps1_close(m);
    printf("test_a_timed_transfer_reports_the_cost_of_each_answer OK\n");
}

/* The time of one byte on the link of a PS1, in units of PSEMU_TIME_HZ. A PS1 shifts 8 bits at
   JOY_BAUD 0x88, thus one byte is 8 x 136 = 1088 ticks of its 33,868,800Hz master clock, and 441
   ticks are 13,312,640 units. A PS1 cannot send the next byte in less time than this. */
#define PS1_BYTE_UNITS ((1088ull * 13312640ull) / 441ull)

/* The time from the selection to the end of the first byte, in units. See PSEMU_COM_SELECT_LEAD_CYCLES. */
#define SELECT_LEAD_UNITS ((uint64_t)PSEMU_COM_SELECT_LEAD_CYCLES * PSEMU_TIME_PER_REFERENCE_CYCLE)

/* Selects the device the way a PS1 does: the hold of /SEL, and then the time until the first byte is
   complete. The kernel takes its interrupt at the hold. */
static void select_like_a_ps1(psemu_t *ps) {
    psemu_com_set_selected(ps, 1);
    (void)psemu_run_time(ps, SELECT_LEAD_UNITS);
}

/* The budget for one answer, in units: the default budget of psemu_com_transfer. */
#define ANSWER_BUDGET_UNITS ((uint64_t)PSEMU_COM_DEFAULT_TIMEOUT_CYCLES * PSEMU_TIME_PER_REFERENCE_CYCLE)

static int same_machine(const psemu_t *a, const psemu_t *b) {
    size_t size = psemu_state_size(a);
    uint8_t *sa = (uint8_t *)malloc(size);
    uint8_t *sb = (uint8_t *)malloc(size);
    int same;
    assert(sa != NULL && sb != NULL);
    assert(psemu_save_state(a, sa, size) == PSEMU_OK);
    assert(psemu_save_state(b, sb, size) == PSEMU_OK);
    same = (memcmp(sa, sb, size) == 0);
    if (!same) {
        size_t k, n = 0;
        for (k = 0; k < size && n < 12u; k++) {
            if (sa[k] != sb[k]) {
                printf("  DIFF at %u: %02X %02X\n", (unsigned)k, sa[k], sb[k]);
                n++;
            }
        }
    }
    free(sa);
    free(sb);
    return same;
}

/* psemu_run_time_to_ack stops after the instruction that acknowledges the byte, thus its return value
   is the exact time of the answer. This test selects the device as a PS1 does, and sends Get ID with
   the gap of a real PS1 between the bytes. The time of each answer must be less than the poll step of
   psemu_com_transfer_timed (64 reference cycles). The last byte has no acknowledge, and the run then
   ends at the budget. */
static void test_the_time_to_ack_is_the_exact_time_of_the_answer(void) {
    mock_ps1_t *m = mock_ps1_open(bios_path(), NULL);
    const uint8_t send[10] = {0x81u, 0x53u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint8_t reply[10];
    uint64_t cost[10];
    unsigned i;

    assert(m != NULL);
    select_like_a_ps1(m->ps);
    for (i = 0; i < 10u; i++) {
        reply[i] = psemu_com_send(m->ps, send[i]);
        cost[i] = psemu_run_time_to_ack(m->ps, ANSWER_BUDGET_UNITS);
        if (i < 9u) {
            assert(psemu_com_acked(m->ps));
            assert(cost[i] > 0u && cost[i] < 64u * PSEMU_TIME_PER_REFERENCE_CYCLE);
            /* The PS1 sends the next byte one byte time after the acknowledge. */
            (void)psemu_run_time(m->ps, PS1_BYTE_UNITS);
        } else {
            assert(!psemu_com_acked(m->ps));
            assert(cost[i] >= ANSWER_BUDGET_UNITS);
        }
    }
    mock_ps1_end_command(m);

    print_stream("Get ID", reply, 10);
    printf("  time of each answer, in reference cycles:");
    for (i = 0; i < 9u; i++) {
        printf(" %.2f", (double)cost[i] / (double)PSEMU_TIME_PER_REFERENCE_CYCLE);
    }
    printf("\n");

    /* The same identifier bytes as the answer of psemu_com_transfer. See
       test_get_id_gives_the_values_of_a_sony_card. */
    {
        size_t id = mock_ps1_find_id_pair(reply, 10);
        assert(id != MOCK_PS1_NOT_FOUND && id + 8u <= 10u);
        assert(reply[id + 4u] == 0x04u && reply[id + 7u] == 0x80u);
    }

    mock_ps1_close(m);
    printf("test_the_time_to_ack_is_the_exact_time_of_the_answer OK\n");
}

/* The clock is exact, thus one long run and a sequence of short runs of different lengths to the same
   end give the same machine, bit for bit. A host that runs the machine ahead, and then goes back to a
   saved state to give an input at an earlier time, depends on this. The third machine here does
   that: it runs ahead past an exchange, loads its saved state, and runs again to the same end. The
   fourth machine does the same with a snapshot, and a snapshot of it does not load into the third. */
static void test_split_runs_give_the_same_machine(void) {
    mock_ps1_t *a = mock_ps1_open(bios_path(), NULL);
    mock_ps1_t *b = mock_ps1_open(bios_path(), NULL);
    mock_ps1_t *c = mock_ps1_open(bios_path(), NULL);
    mock_ps1_t *d = mock_ps1_open(bios_path(), NULL);
    const uint64_t total = 3ull * (PSEMU_REFERENCE_CLOCK_HZ / 32u) * PSEMU_TIME_PER_REFERENCE_CYCLE;
    uint64_t ran_a, ran_b = 0, ran_c, ran_d;
    unsigned i = 0;
    size_t size;
    uint8_t *saved;

    assert(a != NULL && b != NULL && c != NULL && d != NULL);
    assert(same_machine(a->ps, b->ps) && same_machine(a->ps, c->ps));

    ran_a = psemu_run_time(a->ps, total);

    while (ran_b < total) {
        /* Lengths that are not a multiple of a reference cycle or of a CPU cycle. */
        uint64_t chunk = ((uint64_t)(i * 7919u) % 100000u + 1u) * 1000u + 17u;
        if (chunk > total - ran_b) {
            chunk = total - ran_b;
        }
        ran_b += psemu_run_time(b->ps, chunk);
        i++;
    }

    size = psemu_state_size(c->ps);
    saved = (uint8_t *)malloc(size);
    assert(saved != NULL);
    assert(psemu_save_state(c->ps, saved, size) == PSEMU_OK);
    (void)psemu_com_send(c->ps, 0x81u);
    (void)psemu_run_time_to_ack(c->ps, ANSWER_BUDGET_UNITS);
    (void)psemu_run_time(c->ps, total);
    assert(psemu_load_state(c->ps, saved, size) == PSEMU_OK);
    ran_c = psemu_run_time(c->ps, total);
    free(saved);

    saved = (uint8_t *)malloc(psemu_snapshot_size());
    assert(saved != NULL);
    psemu_snapshot_save(d->ps, saved);
    (void)psemu_com_send(d->ps, 0x81u);
    (void)psemu_run_time_to_ack(d->ps, ANSWER_BUDGET_UNITS);
    (void)psemu_run_time(d->ps, total);
    assert(psemu_snapshot_load(c->ps, saved) == PSEMU_ERR_BAD_FORMAT);
    assert(psemu_snapshot_load(d->ps, saved) == PSEMU_OK);
    ran_d = psemu_run_time(d->ps, total);
    free(saved);

    printf("  one run %llu units in %u short runs, end %llu / %llu / %llu\n", (unsigned long long)ran_a, i,
        (unsigned long long)psemu_time(a->ps), (unsigned long long)psemu_time(b->ps),
        (unsigned long long)psemu_time(c->ps));
    assert(psemu_time(a->ps) == psemu_time(b->ps));
    assert(ran_a == ran_b && ran_a == ran_c && ran_a == ran_d);
    assert(same_machine(a->ps, b->ps));
    assert(same_machine(a->ps, c->ps));
    assert(same_machine(a->ps, d->ps));

    mock_ps1_close(a);
    mock_ps1_close(b);
    mock_ps1_close(c);
    mock_ps1_close(d);
    printf("test_split_runs_give_the_same_machine OK\n");
}

/* Sends Get ID with the byte gap of a real PS1, releases the select line, and gives the kernel 1/128 s
   to finish. */
static void get_id_at_ps1_pace(psemu_t *ps) {
    static const uint8_t send[10] = {0x81u, 0x53u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    unsigned i;
    select_like_a_ps1(ps);
    for (i = 0; i < 10u; i++) {
        (void)psemu_com_send(ps, send[i]);
        (void)psemu_run_time_to_ack(ps, ANSWER_BUDGET_UNITS);
        (void)psemu_run_time(ps, PS1_BYTE_UNITS);
    }
    psemu_com_set_selected(ps, 0);
    (void)psemu_run_time(ps, 600ull * 13312640ull);
}

/* Runs `seconds` of device time in runs of 1/128 s, and gives the host
   time that it took, in clock() ticks. */
static clock_t run_seconds(psemu_t *ps, unsigned seconds) {
    clock_t t0 = clock();
    unsigned i;
    for (i = 0; i < seconds * 128u; i++) {
        (void)psemu_run_time(ps, 600ull * 13312640ull);
    }
    return clock() - t0;
}

/* The idle-loop skip advances the machine past the iterations of a wait loop in one step. The machine
   after the skip must be the same, bit for bit, as the machine that executed each iteration: the
   registers, the step count, each timer, the RTC, the DAC samples and the IR clock. This test runs
   two machines, one with the skip and one without, through the conditions of a docked session: a
   docked wait, commands at the pace of a PS1, and an undocked period. It compares them after each
   part. The undocked part is also where an app runs. */
static void test_idle_skip_gives_the_same_machine(void) {
    mock_ps1_t *a = mock_ps1_open(bios_path(), NULL);
    mock_ps1_t *b = mock_ps1_open(bios_path(), NULL);
    clock_t ta = 0, tb = 0;
    unsigned i;

    assert(a != NULL && b != NULL);
    psemu_set_idle_skip(b->ps, 0);
    assert(same_machine(a->ps, b->ps));

    ta += run_seconds(a->ps, 10u);
    tb += run_seconds(b->ps, 10u);
    assert(same_machine(a->ps, b->ps));

    for (i = 0; i < 20u; i++) {
        get_id_at_ps1_pace(a->ps);
        get_id_at_ps1_pace(b->ps);
        ta += run_seconds(a->ps, 1u);
        tb += run_seconds(b->ps, 1u);
    }
    assert(same_machine(a->ps, b->ps));

    /* A selection with no byte, as for each read of the controller: the kernel waits in its FIQ for the
       release, in a loop that reads COM_STAT1 and INT_INPUT. */
    for (i = 0; i < 60u; i++) {
        psemu_com_set_selected(a->ps, 1);
        psemu_com_set_selected(b->ps, 1);
        (void)psemu_run_time(a->ps, 500ull * PSEMU_TIME_PER_REFERENCE_CYCLE + 12345u);
        (void)psemu_run_time(b->ps, 500ull * PSEMU_TIME_PER_REFERENCE_CYCLE + 12345u);
        psemu_com_set_selected(a->ps, 0);
        psemu_com_set_selected(b->ps, 0);
        (void)psemu_run_time(a->ps, 600ull * 13312640ull);
        (void)psemu_run_time(b->ps, 600ull * 13312640ull);
    }
    assert(same_machine(a->ps, b->ps));

    psemu_com_set_docked(a->ps, 0);
    psemu_com_set_docked(b->ps, 0);
    ta += run_seconds(a->ps, 10u);
    tb += run_seconds(b->ps, 10u);
    assert(same_machine(a->ps, b->ps));

    printf("  40 s of device time: %ld ms with the skip, %ld ms without\n", (long)(ta * 1000 / CLOCKS_PER_SEC),
        (long)(tb * 1000 / CLOCKS_PER_SEC));

    mock_ps1_close(a);
    mock_ps1_close(b);
    printf("test_idle_skip_gives_the_same_machine OK\n");
}

/* A selection with no byte for the device leaves the kernel ready. A PS1 selects the port also to
   read the controller, and the device sees that selection. The selection starts the FIQ of the kernel
   (see PSEMU_COM_SELECT_LEAD_CYCLES), the kernel waits for the release, and it then ends the command.
   The next command must answer. */
static void test_a_selection_with_no_byte_leaves_the_kernel_ready(void) {
    mock_ps1_t *m = mock_ps1_open(bios_path(), NULL);
    uint8_t reply[10];
    size_t n, id;
    unsigned i;

    assert(m != NULL);
    for (i = 0; i < 5u; i++) {
        psemu_com_set_selected(m->ps, 1);
        (void)psemu_run(m->ps, 500u);
        psemu_com_set_selected(m->ps, 0);
        mock_ps1_run_frames(m, 1u);
    }

    n = mock_ps1_get_id(m, reply);
    mock_ps1_end_command(m);
    print_stream("Get ID after 5 selections with no byte", reply, n);
    id = mock_ps1_find_id_pair(reply, n);
    assert(id != MOCK_PS1_NOT_FOUND && id + 8u <= n);
    assert(reply[id + 4u] == 0x04u && reply[id + 7u] == 0x80u);

    mock_ps1_close(m);
    printf("test_a_selection_with_no_byte_leaves_the_kernel_ready OK\n");
}

/* The idle-loop skip must also give the same machine from creation: through boot, docking and docked
   frames. Boot has loops that read the interrupt controller, for example the waits for an edge of the
   RTC at 0x04000546 and 0x04000650 in J110. An edge can come after the read of an iteration and before
   its branch, thus the skip must also require that the interrupt controller does not change inside the
   iteration that it measures. The runs here are 100 reference cycles each, thus each one ends inside a
   different iteration. */
static psemu_t *boot_machine(int skip) {
    FILE *f = fopen(bios_path(), "rb");
    uint8_t bios[PSEMU_BIOS_SIZE];
    size_t n;
    psemu_t *ps = psemu_create();
    assert(f != NULL && ps != NULL);
    n = fread(bios, 1, sizeof(bios), f);
    fclose(f);
    psemu_set_idle_skip(ps, skip);
    assert(psemu_load_bios(ps, bios, n) == PSEMU_OK);
    psemu_reset(ps);
    return ps;
}

static void test_idle_skip_gives_the_same_boot(void) {
    psemu_t *a = boot_machine(1);
    psemu_t *b = boot_machine(0);
    unsigned i;

    for (i = 0; i < 200u * 330u; i++) {
        (void)psemu_run(a, 100u);
        (void)psemu_run(b, 100u);
    }
    assert(same_machine(a, b));

    psemu_com_set_docked(a, 1);
    psemu_com_set_docked(b, 1);
    for (i = 0; i < 60u * 330u; i++) {
        (void)psemu_run(a, 100u);
        (void)psemu_run(b, 100u);
    }
    assert(psemu_com_is_enabled(a));
    assert(same_machine(a, b));

    for (i = 0; i < 300u; i++) {
        (void)psemu_run(a, 33000u);
        (void)psemu_run(b, 33000u);
    }
    assert(same_machine(a, b));

    psemu_destroy(a);
    psemu_destroy(b);
    printf("test_idle_skip_gives_the_same_boot OK\n");
}

int main(void) {
    mock_ps1_t *m;
    const char *path = bios_path();

    /* A failed assert() calls abort(), and abort() discards a buffered stream. The printed streams
       below are the evidence for a failure, thus this suite must not buffer them. */
    setvbuf(stdout, NULL, _IONBF, 0);

    m = mock_ps1_open(path, NULL);
    if (!m) {
        printf("bu_test: no usable BIOS at %s, skipping\n", path);
        printf("bu_test: set PSEMU_TEST_BIOS to a %d-byte dump to run this suite\n", PSEMU_BIOS_SIZE);
        return SKIP_EXIT_CODE;
    }
    printf("bu_test: BIOS %s, communication enabled\n", path);

    test_get_id_gives_the_values_of_a_sony_card(m);
    test_a_second_command_answers(m);
    test_write_sector_puts_the_data_in_flash(m);
    test_read_sector_gives_the_written_data(m);
    test_read_sector_dummy_bytes_are_zero(m);
    test_write_sector_refuses_a_bad_checksum(m);
    test_a_timed_transfer_reports_the_cost_of_each_answer();
    test_the_time_to_ack_is_the_exact_time_of_the_answer();
    test_split_runs_give_the_same_machine();
    test_idle_skip_gives_the_same_machine();
    test_idle_skip_gives_the_same_boot();
    test_a_selection_with_no_byte_leaves_the_kernel_ready();

    mock_ps1_close(m);
    printf("bu_test: all tests OK\n");
    return 0;
}
