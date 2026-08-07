/* SPDX-FileCopyrightText: Copyright (c) 2026 Darien Liu (mentalfoundry)
   SPDX-License-Identifier: MIT */

#ifndef POKKETSTATION_MOCK_PS1_H
#define POKKETSTATION_MOCK_PS1_H

#include <stddef.h>
#include <stdint.h>

#include "psemu/psemu.h"

/* A PS1 that speaks the memory card protocol at the emulated machine.

   This module holds the PS1 half of the link. The core holds no protocol, because the kernel of the
   emulated machine holds it. See core/src/com.h. A test needs the other half of the link to send a
   command and to examine the answer. This module is that half.

   This module is also the reference for a port into a PS1 emulator. Such a port drives four
   operations: it docks the device, it exchanges each byte of a command, it releases the select line,
   and it runs the machine between commands. mock_ps1_write_sector below sends the same bytes that a
   port must send.

   THIS MODULE NEEDS A BIOS. Without a BIOS no code answers a transfer. mock_ps1_open gives NULL in
   that condition. A test that gets NULL skips itself.

   THE REPLY IS ONE EXCHANGE BEHIND THE SEND. The communication block is a shift register. The PS1
   receives the byte of exchange N during exchange N+1. Thus the reply array of each function below
   is offset from the send array by one position. See core/src/com.h. */

#define MOCK_PS1_FRAME_SIZE 128u

/* The terminator byte of a command. A published command table gives the first three codes. The
   PocketStation adds the last two codes.
   0xFD refuses a write to the directory entries of a running file.
   0xFE refuses a write to the write-protected broken-sector region, which is sector 16 to 55.
   ComFlags bit 10 enables that second refusal, and that bit is usually clear. */
#define MOCK_PS1_TERM_GOOD 0x47u          /* "G" */
#define MOCK_PS1_TERM_BAD_CHECKSUM 0x4Eu  /* "N" */
#define MOCK_PS1_TERM_BAD_SECTOR 0xFFu
#define MOCK_PS1_TERM_RUNNING_FILE 0xFDu
#define MOCK_PS1_TERM_PROTECTED 0xFEu

/* The first byte of each command selects the memory card of the port. */
#define MOCK_PS1_SEL_CARD 0x81u

/* The FLAG byte. The device gives this byte during the exchange that sends the command byte. Bit 3
   is the "new card" bit. */
#define MOCK_PS1_FLAG_NEW_CARD 0x08u

/* The two identifier bytes of a memory card. They arrive after the FLAG byte in each of the three
   standard commands. A test anchors on this pair, because the pair has a fixed position inside a
   command and the shift of the output register does not move it. */
#define MOCK_PS1_ID1 0x5Au
#define MOCK_PS1_ID2 0x5Du

typedef struct mock_ps1 {
    psemu_t *ps;
    /* The per-byte budget for psemu_com_transfer. mock_ps1_open gives this field
       PSEMU_COM_DEFAULT_TIMEOUT_CYCLES. A command that makes the kernel program flash can need a
       larger budget. */
    uint32_t timeout_cycles;
    /* The frames that mock_ps1_end_command runs while it holds the select line released. */
    unsigned settle_frames;
} mock_ps1_t;

/* Boots a machine, docks it, and waits for the kernel to enable communication.
   `bios_path` is a BIOS dump. `card_path` is a card image or an app, and it accepts NULL.
   It gives NULL if it cannot read the BIOS, if the image is not a BIOS, or if the kernel never
   enables communication. */
mock_ps1_t *mock_ps1_open(const char *bios_path, const char *card_path);

void mock_ps1_close(mock_ps1_t *m);

/* Exchanges `count` bytes. It writes each reply into `reply`.

   It gives the number of exchanges that it made. That number is `count` when the device
   acknowledged each byte. It is smaller when the device stopped. A device gives no acknowledge for
   the last byte of a command, because the command is complete at that point. Thus the last reply of
   a command is at `reply[result - 1]`.

   A real PS1 stops a transfer at a missing acknowledge. This function does the same. */
size_t mock_ps1_exchange(mock_ps1_t *m, const uint8_t *send, uint8_t *reply, size_t count);

/* Ends one command. It releases the select line and runs the machine.
   The kernel learns that a command ended from the release of that line. Without this call the kernel
   answers one command and then answers nothing. See psemu_com_set_selected.

   THE RELEASE IS A LEVEL. A PS1 holds the select line released for the full time between two
   commands, and COM_STAT1 bit 1 stays set for that full time. Thus one call to
   psemu_com_set_selected is sufficient, and the machine must then run so the kernel can find the
   level and leave its wait.

   test_a_second_command_answers in bu_test.c covers this transition. See "The /SEL line" in
   docs/hardware-notes.md for the lifetime of bit 1. */
void mock_ps1_end_command(mock_ps1_t *m);

/* Runs the machine for `frames` frames at the refresh rate of the LCD. */
void mock_ps1_run_frames(mock_ps1_t *m, unsigned frames);

/* The checksum byte of a Read Sector command and of a Write Sector command.
   It is the XOR of the two sector bytes and of each of the 128 data bytes. */
uint8_t mock_ps1_checksum(uint16_t sector, const uint8_t *data);

/* Finds the two identifier bytes in a reply stream.
   It gives the index of MOCK_PS1_ID1, and the byte at the next index is MOCK_PS1_ID2. It gives
   MOCK_PS1_NOT_FOUND if the pair is not in the stream.
   A test uses this index as an anchor. The position of each byte after the pair is fixed against the
   pair, thus a test does not depend on the shift of the output register. */
#define MOCK_PS1_NOT_FOUND ((size_t)-1)
size_t mock_ps1_find_id_pair(const uint8_t *reply, size_t count);

/* The layout of a Read Sector reply, as a distance from the index that mock_ps1_find_id_pair gives.
   +0 and +1 are the two identifier bytes.
   +2 and +3 are two dummy bytes. A PocketStation gives 0x00 here. An original Sony card gives the
   "(pre)" dummies here. This difference is one of the three that separate the two devices at the
   three standard commands.
   +4 and +5 are the two acknowledge bytes, 0x5C and 0x5D.
   +6 and +7 repeat the sector number, with the high byte first.
   +8 begins the 128 data bytes. */
#define MOCK_PS1_READ_DUMMY_OFFSET 2u
#define MOCK_PS1_READ_ACK_OFFSET 4u
#define MOCK_PS1_READ_SECTOR_OFFSET 6u
#define MOCK_PS1_READ_DATA_OFFSET 8u

/* Command 0x53, Get ID.
   `reply` receives 10 bytes. It gives the number of exchanges that it made. */
size_t mock_ps1_get_id(mock_ps1_t *m, uint8_t *reply);

/* Command 0x52, Read Sector.
   `out_data` receives 128 bytes, and it accepts NULL. `out_reply` receives the full reply stream,
   and it accepts NULL. The stream is MOCK_PS1_READ_REPLY_SIZE bytes.
   It gives the number of exchanges that it made. */
#define MOCK_PS1_READ_REPLY_SIZE (10u + MOCK_PS1_FRAME_SIZE + 8u)
size_t mock_ps1_read_sector(mock_ps1_t *m, uint16_t sector, uint8_t *out_data, uint8_t *out_reply);

/* Command 0x57, Write Sector.
   `data` is 128 bytes. `out_term` receives the terminator byte, and it accepts NULL. `out_reply`
   receives the full reply stream, and it accepts NULL. The stream is MOCK_PS1_WRITE_REPLY_SIZE
   bytes.

   `corrupt_checksum` sends an incorrect checksum when it is nonzero. The kernel then gives
   MOCK_PS1_TERM_BAD_CHECKSUM and it writes nothing.

   This function does not end the command. A caller calls mock_ps1_end_command, and then runs the
   machine. The kernel can program flash after the command ends.

   It gives the number of exchanges that it made. */
#define MOCK_PS1_WRITE_REPLY_SIZE (6u + MOCK_PS1_FRAME_SIZE + 1u + 8u)
size_t mock_ps1_write_sector(mock_ps1_t *m, uint16_t sector, const uint8_t *data, int corrupt_checksum,
    uint8_t *out_term, uint8_t *out_reply);

#endif /* POKKETSTATION_MOCK_PS1_H */
