/* SPDX-FileCopyrightText: Copyright (c) 2026 Darien Liu (mentalfoundry)
   SPDX-License-Identifier: MIT */

/* See the comment at the top of cpu_test.c. NDEBUG in a Release build removes each assert() call,
   thus this test suite must keep them. This code must come before <assert.h>. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    mock_ps1_close(m);
    printf("bu_test: all tests OK\n");
    return 0;
}
