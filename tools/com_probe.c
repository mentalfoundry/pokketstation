/* What the kernel does with the communication port, and how it answers the PS1.

   The communication port (core/src/com.h) is the link to a PS1. The link goes through the memory
   card connector. A published register map gives the address of each register in that block. It does
   NOT give the function of the COM_CTRL1 bits or the COM_CTRL2 bits. It also does not give the event
   that clears the Ready bit of COM_STAT2. It records only the values that it observed.

   The kernel of a real BIOS holds the answer to each of those questions, because the kernel operates
   this hardware. This tool reads the answer out of a real BIOS dump. tools/datetime_probe.c uses the
   same method for the date and time settings.

   This tool found four behaviors that no source records. Each one is now in core/src/com.c:
   - The block is a shift register. The PS1 receives the byte of exchange N during exchange N+1.
   - COM_STAT1 bit 1 reports a release of the /SEL line. That release ends a command. The selbit mode
     confirms bit 1 and rejects each of bits 2 to 7.
   - COM_STAT1 bit 0 reports an arrived byte. The write path polls this register in place of
     COM_STAT2.
   - The Ready bit of COM_STAT2 clears at a read of COM_DATA, and also at the acknowledge.

   Modes:
     dock   - boots, docks the machine, and reports each COM register access with its PC. It also
              reports the ComFlags word before and after the transition. Use this mode to find the
              initialization sequence and the address of the COM code of the kernel.
     cmd    - docks, then sends a byte sequence to the machine and reports the reply to each byte.
              Give the bytes in hex. The standard Get ID command is "81 53 00 00 00 00 00 00 00 00".
     wait   - measures the cycles that one answer needs, for each byte of a command. Use this mode to
              size PSEMU_COM_DEFAULT_TIMEOUT_CYCLES.
     write  - runs one Write Sector command, then reads the sector back out of flash. It also reports
              each frame of the card that changed, and the number of attempted flash writes.
     selbit - finds the COM_STAT1 bit that ends a command. It sets one candidate bit at a time, and
              it reports which bit lets a second command run.
     func   - runs command 0x5B with FUNC 00h and FUNC 01h. FUNC 01h reads BIOS ROM, and this mode
              compares the reply against the loaded BIOS image.
     cmd58  - sends command 0x58 after docking and prints the reply. A trace build also shows the
              BIOS PCs executed and the bytes written to COM_DATA during the command.
     launch58 - sends command 0x59 to start the app at a given slot, runs frames to let the app
              settle, then sends command 0x58 and prints the reply. Use this mode to test whether
              an app installs a FIQ hook that changes the 0x58 reply.
     autolaunch - runs the machine undocked and reports the first frame where psemu_app_running
              returns true. Then docks and sends command 0x58. Use this mode to measure how many
              undocked frames the kernel needs to start an installed app.
     test59_undock - sends command 0x59 while docked, undocks, runs frames checking
              psemu_app_running, re-docks, then sends command 0x58. Use this mode to test the
              app launch sequence that a PS1 uses when it writes and starts an app.

   usage: com_probe <bios.bin> dock [boot_frames]
          com_probe <bios.bin> cmd <hexbyte>... [--frames N] [--trace] [--card F] [--timeout N]
          com_probe <bios.bin> wait [boot_frames]
          com_probe <bios.bin> write [sector] [--card F] [--badsum] [--writeenable]
          com_probe <bios.bin> selbit [boot_frames]
          com_probe <bios.bin> func [boot_frames]
          com_probe <bios.bin> cmd58 [boot_frames] [--card F]
          com_probe <bios.bin> launch58 [slot] [--frames N] [--run N] [--card F]
          com_probe <bios.bin> autolaunch [max_frames] [--card F]
          com_probe <bios.bin> test59_undock [slot] [--frames N] [--run N] [--card F] */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

/* The per-frame cycle budget of a frontend, at the 32Hz refresh rate of the LCD.
   See PSEMU_ASSUMED_CPU_HZ in core/src/dac.h. */
#define FRAME_CYCLES (PSEMU_ASSUMED_CPU_HZ / 32u)

/* The ComFlags word in kernel RAM. A published register map gives this address. That map gives SWI
   06h (GetPtrToComFlags) as the supported method to find the address. This tool reads the address
   directly, because it must observe the word without a call into the machine.
   Bit 9 is the important bit here. Its name is "Communication Enabled And Docked". The kernel sets
   that bit after it senses the docked condition and enables communication. No code answers a command
   while the bit is clear. */
#define COMFLAGS_ADDR 0x0C0u
#define COMFLAGS_DOCKED_AND_ENABLED (1u << 9)
#define COMFLAGS_IOP_OCCURRED (1u << 8)

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *buf;
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static uint32_t read_comflags(const psemu_t *ps) {
    return (uint32_t)ps->bus.ram[COMFLAGS_ADDR] | ((uint32_t)ps->bus.ram[COMFLAGS_ADDR + 1] << 8) |
        ((uint32_t)ps->bus.ram[COMFLAGS_ADDR + 2] << 16) | ((uint32_t)ps->bus.ram[COMFLAGS_ADDR + 3] << 24);
}

static void print_comflags(const char *label, uint32_t flags) {
    printf("%s ComFlags = 0x%08X  [bit8 iop=%d bit9 docked_enabled=%d bit10 protect=%d bit11 start_file=%d]\n", label,
        flags, (flags >> 8) & 1u, (flags >> 9) & 1u, (flags >> 10) & 1u, (flags >> 11) & 1u);
}

/* The card image that a caller supplies with --card. A Write Sector test needs a real card, because
   the write path of the kernel refuses a write to the directory of a running file. It also refuses a
   write to the broken-sector region. Neither refusal can occur against empty flash. */
static const char *g_card_path = NULL;

static psemu_t *boot(const char *bios_path, unsigned boot_frames) {
    size_t bios_size = 0;
    uint8_t *bios = read_file(bios_path, &bios_size);
    psemu_t *ps;
    unsigned i;

    if (!bios) {
        fprintf(stderr, "com_probe: cannot read %s\n", bios_path);
        return NULL;
    }
    ps = psemu_create();
    if (!ps) {
        free(bios);
        return NULL;
    }
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        fprintf(stderr, "com_probe: %s is not a %d-byte BIOS image\n", bios_path, PSEMU_BIOS_SIZE);
        free(bios);
        psemu_destroy(ps);
        return NULL;
    }
    free(bios);
    psemu_reset(ps);

    if (g_card_path) {
        size_t card_size = 0;
        uint8_t *card = read_file(g_card_path, &card_size);
        if (!card) {
            fprintf(stderr, "com_probe: cannot read %s\n", g_card_path);
            psemu_destroy(ps);
            return NULL;
        }
        if (psemu_load_content(ps, card, card_size) != PSEMU_OK) {
            fprintf(stderr, "com_probe: no loader accepts %s\n", g_card_path);
            free(card);
            psemu_destroy(ps);
            return NULL;
        }
        free(card);
        printf("loaded card %s\n", g_card_path);
    }

    for (i = 0; i < boot_frames; i++) {
        psemu_run(ps, FRAME_CYCLES);
    }
    return ps;
}

/* Docks the machine. It then runs the machine until the kernel reports enabled communication.
   It returns the number of frames that the kernel needed. It returns 0 if the kernel never reported
   that condition.
   The kernel enables communication from its IRQ-11 handler. That handler waits before it reads the
   docking level again. The wait skips the switch-bounce period of a real connector. Thus a caller
   must run the machine after the transition. One frame can be too few. */
static unsigned dock_and_settle(psemu_t *ps, unsigned max_frames) {
    unsigned i;
    psemu_com_set_docked(ps, 1);
    for (i = 1; i <= max_frames; i++) {
        psemu_run(ps, FRAME_CYCLES);
        if (read_comflags(ps) & COMFLAGS_DOCKED_AND_ENABLED) {
            return i;
        }
    }
    return 0;
}

/* Releases the /SEL line, then runs the machine so the kernel can leave its end-of-command wait.
   A PS1 does this between commands. Without it the kernel answers one command and then stops.
   See psemu_com_set_selected. */
static void end_command(psemu_t *ps) {
    unsigned i;
    psemu_com_set_selected(ps, 0);
    for (i = 0; i < 2u; i++) {
        psemu_run(ps, FRAME_CYCLES);
    }
}

static int mode_dock(const char *bios_path, unsigned boot_frames) {
    psemu_t *ps = boot(bios_path, boot_frames);
    unsigned settled;
    if (!ps) {
        return 1;
    }

    printf("=== boot complete, %u frames ===\n", boot_frames);
    print_comflags("before dock:", read_comflags(ps));
    printf("INTC enable=0x%08X status=0x%08X hold=0x%08X\n", ps->intc.enable, ps->intc.status, ps->intc.hold);
    printf("  INT_IOP (bit 11) enabled: %s\n", (ps->intc.enable & INT_IOP) ? "yes" : "no");
    printf("  INT_COM (bit 6, FIQ) enabled: %s\n", (ps->intc.enable & INT_COM) ? "yes" : "no");

    printf("\n=== docking, COM register trace follows ===\n");
    psemu_com_trace_enabled = 1;
    settled = dock_and_settle(ps, 60);
    psemu_com_trace_enabled = 0;

    printf("\n=== after dock ===\n");
    if (settled) {
        printf("communication enabled after %u frame(s)\n", settled);
    } else {
        printf("communication NEVER enabled in 60 frames.\n");
        printf("The kernel did not set ComFlags bit 9. Check that INT_IOP reaches STATUS and HOLD.\n");
    }
    print_comflags("after dock: ", read_comflags(ps));
    printf("INTC enable=0x%08X status=0x%08X hold=0x%08X\n", ps->intc.enable, ps->intc.status, ps->intc.hold);
    printf("  INT_IOP (bit 11) enabled: %s\n", (ps->intc.enable & INT_IOP) ? "yes" : "no");
    printf("  INT_COM (bit 6, FIQ) enabled: %s\n", (ps->intc.enable & INT_COM) ? "yes" : "no");
    printf("COM registers: mode=0x%08X stat1=0x%08X ctrl1=0x%08X ctrl2=0x%08X\n", ps->com.mode, ps->com.stat1,
        ps->com.ctrl1, ps->com.ctrl2);

    psemu_destroy(ps);
    return 0;
}

/* The per-byte cycle budget that the cmd mode and the write mode use.
   PSEMU_COM_DEFAULT_TIMEOUT_CYCLES is the default. The --timeout option changes it. A command that
   makes the kernel program flash can need much more time than a command that only reads. */
static uint32_t g_timeout_cycles = PSEMU_COM_DEFAULT_TIMEOUT_CYCLES;

static int mode_cmd(const char *bios_path, unsigned boot_frames, const uint8_t *bytes, size_t count, int trace) {
    psemu_t *ps = boot(bios_path, boot_frames);
    unsigned settled;
    size_t i;
    if (!ps) {
        return 1;
    }

    settled = dock_and_settle(ps, 60);
    if (!settled) {
        printf("WARNING: the kernel did not enable communication. The replies below are not meaningful.\n");
    } else {
        printf("communication enabled after %u frame(s)\n", settled);
    }
    print_comflags("docked:", read_comflags(ps));

    printf("\n idx  send  reply  ack\n");
    psemu_com_trace_enabled = trace;
    for (i = 0; i < count; i++) {
        uint8_t out = 0xFF;
        int ack = psemu_com_transfer(ps, bytes[i], &out, g_timeout_cycles);
        printf(" %3u   0x%02X   0x%02X   %s\n", (unsigned)i, (unsigned)bytes[i], (unsigned)out, ack ? "yes" : "NO");
        if (!ack) {
            printf("       no acknowledge. A real PS1 stops the transfer here.\n");
        }
    }
    psemu_com_trace_enabled = 0;

    printf("\n");
    print_comflags("after: ", read_comflags(ps));
    psemu_destroy(ps);
    return 0;
}

/* The cost of one answer, in cycles, for each byte of a command.
   This mode does not call psemu_com_transfer. That function advances the machine in steps of
   COM_POLL_CHUNK_CYCLES, thus it cannot resolve a cost below one step. This loop advances the
   machine one call at a time, and it counts the cycles until the kernel acknowledges.
   The result sizes PSEMU_COM_DEFAULT_TIMEOUT_CYCLES. */
static uint32_t measure_one_byte(psemu_t *ps, uint8_t data_in, uint32_t limit, int *out_ack) {
    uint32_t ran = 0;
    com_begin_transfer(&ps->com, &ps->intc, data_in);
    while (ran < limit && !com_transfer_acked(&ps->com)) {
        uint32_t did = psemu_run(ps, 1u);
        if (did == 0u) {
            break;
        }
        ran += did;
    }
    *out_ack = com_transfer_acked(&ps->com);
    com_end_transfer(&ps->com, &ps->intc);
    return ran;
}

static int mode_wait(const char *bios_path, unsigned boot_frames) {
    /* The Get ID command. The first byte needs a full FIQ entry, because the kernel is idle before
       it. Each byte after that arrives while the kernel polls COM_STAT2 inside the same FIQ. Thus
       this one command covers both costs. */
    static const uint8_t GET_ID[] = {0x81u, 0x53u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    psemu_t *ps = boot(bios_path, boot_frames);
    unsigned settled;
    uint32_t worst = 0;
    size_t i;
    if (!ps) {
        return 1;
    }

    settled = dock_and_settle(ps, 60);
    printf("communication enabled: %s\n", settled ? "yes" : "NO");

    printf("\n idx  send  cycles  ack\n");
    for (i = 0; i < sizeof(GET_ID); i++) {
        int ack = 0;
        uint32_t cycles = measure_one_byte(ps, GET_ID[i], 1000000u, &ack);
        printf(" %3u   0x%02X  %6u   %s\n", (unsigned)i, (unsigned)GET_ID[i], cycles, ack ? "yes" : "NO");
        if (ack && cycles > worst) {
            worst = cycles;
        }
    }

    printf("\nworst answer: %u cycles\n", worst);
    printf("PSEMU_COM_DEFAULT_TIMEOUT_CYCLES is %u\n", (unsigned)PSEMU_COM_DEFAULT_TIMEOUT_CYCLES);

    psemu_destroy(ps);
    return 0;
}

/* Runs one Write Sector command (0x57), then reads the sector back out of flash.
   The terminator byte reports the result of the kernel. The published command table gives 0x47 ("G",
   Good) for a successful write. It gives 0x4E ("N") for a bad checksum, and 0xFF for a bad sector.
   The PocketStation adds two more codes. 0xFD refuses a write to the directory entries of a running
   file. 0xFE refuses a write to the write-protected broken-sector region, which is sector 16 to 55.
   ComFlags bit 10 enables that second refusal, and that bit is usually clear. */
#define FRAME_SIZE 128u

/* A copy of flash from before the command. The comparison against it finds each frame that the
   command changed. */
static uint8_t g_flash_before[PSEMU_FLASH_SIZE];

/* Counts each attempted write into the flash regions and into the flash control registers. The
   write-trace hook of the psemu_trace build reports an attempt before the region dispatch. Thus this
   counter also reports a write that flash then discards. See psemu_bus_write_trace_cb in
   core/src/memory.c. */
static struct {
    unsigned long flash2;
    unsigned long flash1;
    unsigned long ctrl;
    uint32_t first_ctrl_pc;
    uint32_t first_flash_pc;
} g_flash_watch;

static void flash_write_watch(uint32_t addr, uint8_t value, uint32_t pc) {
    (void)value;
    if (addr >= PSEMU_FLASH2_BASE && addr < PSEMU_FLASH2_BASE + PSEMU_FLASH_SIZE) {
        if (g_flash_watch.flash2 == 0) {
            g_flash_watch.first_flash_pc = pc;
        }
        g_flash_watch.flash2++;
    } else if (addr >= PSEMU_FLASH1_BASE && addr < PSEMU_FLASH1_BASE + PSEMU_FLASH_SIZE) {
        g_flash_watch.flash1++;
    } else if (addr >= PSEMU_FLASH_CTRL_BASE && addr < PSEMU_FLASH_CTRL_BASE + FLASH_CTRL_SPAN) {
        if (g_flash_watch.ctrl == 0) {
            g_flash_watch.first_ctrl_pc = pc;
        }
        g_flash_watch.ctrl++;
    }
}

static int mode_write(const char *bios_path, unsigned boot_frames, uint16_t sector, int bad_checksum,
    int enable_writes) {
    psemu_t *ps = boot(bios_path, boot_frames);
    uint8_t data[FRAME_SIZE];
    uint8_t checksum;
    unsigned settled;
    unsigned i;
    uint8_t out = 0xFF;
    int ack;
    int mismatch = 0;
    const uint8_t *flash;

    if (!ps) {
        return 1;
    }
    settled = dock_and_settle(ps, 60);
    printf("communication enabled: %s\n", settled ? "yes" : "NO");

    /* Command 0x5F sends a new value for bit 0 of the status word.

       THIS COMMAND IS NOT NECESSARY FOR A WRITE ON THIS BIOS REVISION. A test confirms that a Write
       Sector command succeeds without it. This option only shows that the command operates.

       An official kernel specification gives bit 0 as "Write to flash memory (0=Enabled,
       1=Disabled)". It records that the kernel disables bits 0 to 3 when a user puts the device in
       the PS1. The observed status word after docking is 0x0007020F, and bits 0 to 3 are set.
       That combination does not block a write on this revision. A published register map records
       that the two layouts of this word come from different BIOS revisions, and that neither layout
       supersedes the other. The J110 revision follows the reverse-engineered layout, where bits 0
       to 3 have no recorded meaning. */
    if (enable_writes) {
        /* Exactly four bytes. The published command table gives 0x81, then 0x5F, then a dummy byte
           that receives the length 0x01, then the new value of bit 0. A fifth byte arrives after the
           command ends, and it leaves the kernel out of step for the next command. */
        static const uint8_t ENABLE[] = {0x81u, 0x5Fu, 0x00u, 0x00u};
        printf("\nsending command 0x5F to enable flash writes\n");
        for (i = 0; i < sizeof(ENABLE); i++) {
            ack = psemu_com_transfer(ps, ENABLE[i], &out, g_timeout_cycles);
            printf("  0x%02X -> 0x%02X ack=%s\n", (unsigned)ENABLE[i], (unsigned)out, ack ? "yes" : "NO");
        }
        print_comflags("after 0x5F:", read_comflags(ps));
        end_command(ps);
    }

    memcpy(g_flash_before, psemu_flash_data(ps), PSEMU_FLASH_SIZE);
    memset(&g_flash_watch, 0, sizeof(g_flash_watch));
    psemu_bus_write_trace_cb = flash_write_watch;

    /* A pattern that no empty card holds, thus a read back cannot pass by accident. */
    for (i = 0; i < FRAME_SIZE; i++) {
        data[i] = (uint8_t)(0xA5u ^ (i * 7u));
    }
    checksum = (uint8_t)((sector >> 8) & 0xFFu) ^ (uint8_t)(sector & 0xFFu);
    for (i = 0; i < FRAME_SIZE; i++) {
        checksum ^= data[i];
    }
    if (bad_checksum) {
        checksum ^= 0xFFu;
        printf("sending a deliberately incorrect checksum\n");
    }

    printf("writing sector %u (0x%04X)\n", (unsigned)sector, (unsigned)sector);

    ack = psemu_com_transfer(ps, 0x81u, &out, g_timeout_cycles);
    printf("  0x81 -> 0x%02X ack=%s\n", (unsigned)out, ack ? "yes" : "NO");
    ack = psemu_com_transfer(ps, 0x57u, &out, g_timeout_cycles);
    printf("  0x57 -> 0x%02X (FLAG) ack=%s\n", (unsigned)out, ack ? "yes" : "NO");
    (void)psemu_com_transfer(ps, 0x00u, &out, g_timeout_cycles);
    printf("  ID1  -> 0x%02X\n", (unsigned)out);
    (void)psemu_com_transfer(ps, 0x00u, &out, g_timeout_cycles);
    printf("  ID2  -> 0x%02X\n", (unsigned)out);
    (void)psemu_com_transfer(ps, (uint8_t)((sector >> 8) & 0xFFu), &out, g_timeout_cycles);
    (void)psemu_com_transfer(ps, (uint8_t)(sector & 0xFFu), &out, g_timeout_cycles);

    for (i = 0; i < FRAME_SIZE; i++) {
        (void)psemu_com_transfer(ps, data[i], &out, g_timeout_cycles);
    }
    (void)psemu_com_transfer(ps, checksum, &out, g_timeout_cycles);

    /* The tail of the command is the two acknowledge bytes and then the terminator. The exact
       position of the terminator depends on the shift of the output register. Thus this loop sends
       dummy bytes and reports each reply, until the kernel gives no acknowledge. That last reply is
       the terminator. */
    for (i = 0; i < 8u; i++) {
        ack = psemu_com_transfer(ps, 0x00u, &out, g_timeout_cycles);
        printf("  tail %u -> 0x%02X ack=%s\n", i, (unsigned)out, ack ? "yes" : "NO");
        if (!ack) {
            break;
        }
    }
    printf("  terminator 0x%02X:", (unsigned)out);
    switch (out) {
    case 0x47u:
        printf("   (\"G\", Good)\n");
        break;
    case 0x4Eu:
        printf("   (\"N\", bad checksum)\n");
        break;
    case 0xFDu:
        printf("   (refused: directory of a running file)\n");
        break;
    case 0xFEu:
        printf("   (refused: write-protected broken-sector region)\n");
        break;
    default:
        printf("   (unrecognized)\n");
        break;
    }

    end_command(ps);

    /* The kernel can program flash after the command ends. Run the machine before the check. */
    for (i = 0; i < 30u; i++) {
        psemu_run(ps, FRAME_CYCLES);
    }

    /* The command reports its own result. Flash is the independent check. */
    flash = psemu_flash_data(ps);
    for (i = 0; i < FRAME_SIZE; i++) {
        if (flash[(size_t)sector * FRAME_SIZE + i] != data[i]) {
            mismatch++;
        }
    }
    printf("sector %u: %u of %u bytes differ from the sent data\n", (unsigned)sector, (unsigned)mismatch, FRAME_SIZE);
    if (mismatch == 0) {
        printf("the sector holds the sent data\n");
    }

    /* A search of the full card finds the frame that the kernel wrote. The kernel can remap a
       logical sector to a replacement frame. Thus the data can land at a different frame number. */
    {
        size_t frame;
        int found = 0;
        for (frame = 0; frame < PSEMU_FLASH_SIZE / FRAME_SIZE; frame++) {
            if (memcmp(&flash[frame * FRAME_SIZE], data, FRAME_SIZE) == 0) {
                printf("the sent data is at frame %u (0x%04X)\n", (unsigned)frame, (unsigned)frame);
                found = 1;
            }
        }
        if (!found) {
            printf("the sent data is at no frame of the card\n");
        }
    }
    {
        size_t changed = 0;
        size_t frame;
        for (frame = 0; frame < PSEMU_FLASH_SIZE / FRAME_SIZE; frame++) {
            if (memcmp(&flash[frame * FRAME_SIZE], &g_flash_before[frame * FRAME_SIZE], FRAME_SIZE) != 0) {
                printf("frame %u (0x%04X) changed\n", (unsigned)frame, (unsigned)frame);
                changed++;
            }
        }
        printf("%u frame(s) changed during this run\n", (unsigned)changed);
    }
    psemu_bus_write_trace_cb = NULL;
    printf("attempted writes: FLASH2=%lu (first pc=0x%08X) FLASH1=%lu FLASH_CTRL=%lu (first pc=0x%08X)\n",
        g_flash_watch.flash2, g_flash_watch.first_flash_pc, g_flash_watch.flash1, g_flash_watch.ctrl,
        g_flash_watch.first_ctrl_pc);

    psemu_destroy(ps);
    return 0;
}

/* Finds the COM_STAT1 bit that reports the end of a command.
   The kernel waits at 0x040007B8 after the last byte of a command. It polls COM_STAT1 there, and it
   does not accept bit 0 in either state. A real PS1 deasserts the /SEL line between commands,
   thus a bit that reports that line is the candidate. This mode sets one candidate bit at a time,
   and it reports which bit lets a second command run. */
static int mode_selbit(const char *bios_path, unsigned boot_frames) {
    static const uint8_t GET_ID[] = {0x81u, 0x53u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    unsigned bit;

    for (bit = 1u; bit < 8u; bit++) {
        psemu_t *ps = boot(bios_path, boot_frames);
        uint8_t out = 0xFF;
        unsigned i;
        int second_ok = 0;
        if (!ps) {
            return 1;
        }
        if (!dock_and_settle(ps, 60)) {
            printf("bit %u: communication never enabled\n", bit);
            psemu_destroy(ps);
            continue;
        }
        for (i = 0; i < sizeof(GET_ID); i++) {
            (void)psemu_com_transfer(ps, GET_ID[i], &out, g_timeout_cycles);
        }
        /* Report the candidate bit, then run the machine so the kernel can leave its wait. */
        ps->com.stat1 = (1u << bit);
        for (i = 0; i < 4u; i++) {
            psemu_run(ps, FRAME_CYCLES);
        }
        ps->com.stat1 = 0u;
        ps->com.sel_drop_latch = 0;
        for (i = 0; i < sizeof(GET_ID); i++) {
            int ack = psemu_com_transfer(ps, GET_ID[i], &out, g_timeout_cycles);
            if (i == 1u && ack && out == 0x08u) {
                second_ok = 1; /* the second command answered with FLAG */
            }
        }
        printf("STAT1 bit %u: second command %s\n", bit, second_ok ? "ANSWERED" : "no answer");
        psemu_destroy(ps);
    }
    return 0;
}

/* Runs command 0x5B, which executes a function and moves data to the PS1.
   This command is the reason to model the hardware and not the protocol. The function numbers 0x80
   to 0xFF resolve through a function table in the header of the app file. Only the app holds that
   code. Thus no protocol code outside the emulated machine can answer them. The transport below is
   the same transport that those app functions use. Only the table lookup is different.

   FUNC 00h gets the date and the time. It needs no parameters, and it returns 8 bytes.
   FUNC 01h reads memory. It needs 5 parameter bytes: a 32-bit address, and then a length. This mode
   reads the first bytes of BIOS ROM, and it compares them against the loaded BIOS image. That
   comparison is an independent check: the reply must agree with a file that this tool already
   holds. */
#define FUNC_READ_ADDR 0x04000000u /* BIOS ROM. psemu_load_bios put a known image here. */
#define FUNC_READ_LEN 16u

static int mode_func(const char *bios_path, unsigned boot_frames) {
    psemu_t *ps = boot(bios_path, boot_frames);
    uint8_t out = 0xFF;
    uint8_t got[FUNC_READ_LEN];
    unsigned i;
    unsigned mismatch = 0;

    if (!ps) {
        return 1;
    }
    if (!dock_and_settle(ps, 60)) {
        printf("communication never enabled\n");
        psemu_destroy(ps);
        return 1;
    }

    /* FUNC 00h. The replies are one exchange behind the sends, thus the marker for a variable length
       arrives with the send after the function number. See com.h. */
    printf("=== command 0x5B, FUNC 00h (get date and time) ===\n");
    {
        static const uint8_t SEQ[] = {0x81u, 0x5Bu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
            0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
        static const char *const NAME[] = {"", "", "FF marker", "LEN1", "LEN2", "BCD day",
            "BCD month", "BCD year", "BCD century", "BCD second", "BCD minute", "BCD hour",
            "BCD day of week", "FF end"};
        for (i = 0; i < sizeof(SEQ); i++) {
            int ack = psemu_com_transfer(ps, SEQ[i], &out, g_timeout_cycles);
            printf("  send 0x%02X -> 0x%02X %-16s ack=%s\n", (unsigned)SEQ[i], (unsigned)out, NAME[i],
                ack ? "yes" : "NO");
        }
    }
    end_command(ps);

    /* FUNC 01h. The address goes out with its low byte first. */
    printf("\n=== command 0x5B, FUNC 01h (read %u bytes at 0x%08X) ===\n", FUNC_READ_LEN,
        (unsigned)FUNC_READ_ADDR);
    {
        uint8_t seq[10 + FUNC_READ_LEN + 2];
        size_t n = 0;
        seq[n++] = 0x81u;
        seq[n++] = 0x5Bu;
        seq[n++] = 0x01u; /* the function number */
        seq[n++] = 0x00u; /* this send receives LEN1 */
        seq[n++] = (uint8_t)(FUNC_READ_ADDR & 0xFFu);
        seq[n++] = (uint8_t)((FUNC_READ_ADDR >> 8) & 0xFFu);
        seq[n++] = (uint8_t)((FUNC_READ_ADDR >> 16) & 0xFFu);
        seq[n++] = (uint8_t)((FUNC_READ_ADDR >> 24) & 0xFFu);
        seq[n++] = (uint8_t)FUNC_READ_LEN;
        while (n < sizeof(seq)) {
            seq[n++] = 0x00u;
        }
        for (i = 0; i < n; i++) {
            int ack = psemu_com_transfer(ps, seq[i], &out, g_timeout_cycles);
            if (i == 3u) {
                printf("  LEN1 = 0x%02X (expects 0x05)\n", (unsigned)out);
            } else if (i == 9u) {
                printf("  LEN2 = 0x%02X (expects 0x%02X)\n", (unsigned)out, FUNC_READ_LEN);
            } else if (i >= 10u && i < 10u + FUNC_READ_LEN) {
                got[i - 10u] = out;
            }
            (void)ack;
        }
    }
    end_command(ps);

    printf("  read back:");
    for (i = 0; i < FUNC_READ_LEN; i++) {
        printf(" %02X", (unsigned)got[i]);
    }
    printf("\n  BIOS file:");
    for (i = 0; i < FUNC_READ_LEN; i++) {
        printf(" %02X", (unsigned)ps->bus.bios[i]);
        if (got[i] != ps->bus.bios[i]) {
            mismatch++;
        }
    }
    printf("\n  %u of %u bytes differ\n", mismatch, FUNC_READ_LEN);
    if (mismatch == 0) {
        printf("  the kernel executed the function and returned the correct memory\n");
    }

    psemu_destroy(ps);
    return mismatch ? 1 : 0;
}

/* Collected COM_DATA write events during a 0x58 run. Each write puts one byte into
   the output shift register. The PS1 receives that byte in the NEXT exchange. */
#define CMD58_TRACE_MAX 64
static struct {
    uint32_t pc;
    uint8_t value;
} g_comdata_log[CMD58_TRACE_MAX];
static unsigned g_comdata_log_count;
static int g_write_trace_active;

#define COM_DATA_ADDR (0x0C000000u + COM_DATA_OFFSET)

/* Collected unique PC values executed during 0x58 (deduped). */
#define EXEC_LOG_MAX 512
static uint32_t g_exec_log[EXEC_LOG_MAX];
static unsigned g_exec_log_count;
static int g_exec_trace_active;

#ifdef PSEMU_TRACE_HOOKS
static void cmd58_write_trace(uint32_t addr, uint8_t value, uint32_t pc) {
    if (!g_write_trace_active) {
        return;
    }
    if (addr == COM_DATA_ADDR && g_comdata_log_count < CMD58_TRACE_MAX) {
        g_comdata_log[g_comdata_log_count].pc = pc;
        g_comdata_log[g_comdata_log_count].value = value;
        g_comdata_log_count++;
    }
}

static void cmd58_exec_trace(uint32_t pc, uint32_t cpsr) {
    (void)cpsr;
    if (!g_exec_trace_active) {
        return;
    }
    /* Only BIOS ROM range (0x04000000-0x04007FFF). Deduplicate. */
    if (pc >= 0x04000000u && pc < 0x04008000u) {
        unsigned j;
        for (j = 0; j < g_exec_log_count; j++) {
            if (g_exec_log[j] == pc) {
                return;
            }
        }
        if (g_exec_log_count < EXEC_LOG_MAX) {
            g_exec_log[g_exec_log_count++] = pc;
        }
    }
}
#endif

/* Sends command 0x58 after docking and prints the reply. With PSEMU_TRACE_HOOKS, also traces
   the BIOS PCs that execute during the command and the bytes written to COM_DATA. */
static int mode_cmd58(const char *bios_path, unsigned boot_frames) {
    static const uint8_t CMD58[] = {0x81u, 0x58u, 0x00u, 0x00u, 0x00u};
    psemu_t *ps = boot(bios_path, boot_frames);
    uint8_t ram_snap[PSEMU_RAM_SIZE];
    unsigned settled;
    uint8_t out;
    size_t i;

    if (!ps) {
        return 1;
    }
    settled = dock_and_settle(ps, 60);
    if (!settled) {
        printf("communication never enabled\n");
        psemu_destroy(ps);
        return 1;
    }
    printf("communication enabled after %u frame(s)\n", settled);
    print_comflags("docked: ", read_comflags(ps));
    memcpy(ram_snap, ps->bus.ram, PSEMU_RAM_SIZE);

#ifdef PSEMU_TRACE_HOOKS
    /* Arm both traces and run one 0x58 to find the setup code. */
    g_comdata_log_count = 0;
    g_exec_log_count = 0;
    g_write_trace_active = 0;
    g_exec_trace_active = 0;
    psemu_bus_write_trace_cb = cmd58_write_trace;
    psemu_exec_trace_cb = cmd58_exec_trace;

    {
        uint8_t r0;
        psemu_com_transfer(ps, 0x81u, &r0, g_timeout_cycles);
        g_write_trace_active = 1;
        g_exec_trace_active = 1;
        for (i = 1; i < sizeof(CMD58); i++) {
            psemu_com_transfer(ps, CMD58[i], &out, g_timeout_cycles);
        }
        g_write_trace_active = 0;
        g_exec_trace_active = 0;
        psemu_bus_write_trace_cb = NULL;
        psemu_exec_trace_cb = NULL;
        end_command(ps);
    }

    printf("\nCOM_DATA writes during 0x58 (%u total):\n", g_comdata_log_count);
    for (i = 0; i < g_comdata_log_count; i++) {
        printf("  PC=0x%08X  wrote 0x%02X\n",
            (unsigned)g_comdata_log[i].pc, (unsigned)g_comdata_log[i].value);
    }
    printf("\nUnique BIOS PCs executed during 0x58 (%u):\n", g_exec_log_count);
    for (i = 0; i < g_exec_log_count; i++) {
        printf("  0x%08X\n", (unsigned)g_exec_log[i]);
    }
#endif

    /* Restore and run the byte-zeroing scan to confirm which reads are load-bearing. */
    memcpy(ps->bus.ram, ram_snap, PSEMU_RAM_SIZE);

    printf("\nbaseline 0x58:\n");
    for (i = 0; i < sizeof(CMD58); i++) {
        int ack = psemu_com_transfer(ps, CMD58[i], &out, g_timeout_cycles);
        printf("  send 0x%02X -> 0x%02X  ack=%s\n", (unsigned)CMD58[i], (unsigned)out,
            ack ? "yes" : "NO");
    }
    end_command(ps);

    psemu_destroy(ps);
    return 0;
}

/* Boots, docks, sends command 0x59 to start the app at the given slot, runs run_frames frames
   to let the app install its FIQ hooks, then sends command 0x58 and reports the reply.
   This tests whether an app-specific 0x58 handler differs from the BIOS default. */
static int mode_launch58(const char *bios_path, unsigned boot_frames, unsigned slot,
                         unsigned run_frames) {
    static const uint8_t CMD58[] = {0x81u, 0x58u, 0x00u, 0x00u, 0x00u};
    uint8_t cmd59[9];
    uint8_t ram_before[PSEMU_RAM_SIZE];
    psemu_t *ps = boot(bios_path, boot_frames);
    unsigned settled;
    unsigned i;
    uint8_t out;

    if (!ps) {
        return 1;
    }
    settled = dock_and_settle(ps, 60);
    if (!settled) {
        printf("communication never enabled\n");
        psemu_destroy(ps);
        return 1;
    }
    printf("communication enabled after %u frame(s)\n", settled);
    print_comflags("after dock:", read_comflags(ps));
    memcpy(ram_before, ps->bus.ram, PSEMU_RAM_SIZE);

    /* Send 0x59 to start the app at the given slot.
       Format per psx-spx: 81 59 00(dummy) dir_hi dir_lo param0 param1 param2 param3.
       Byte 2 is a dummy zero: the BIOS replies with the data length (06h) and exits the handler
       early when it receives anything other than zero here. dir_index hi byte goes first. */
    cmd59[0] = 0x81u;
    cmd59[1] = 0x59u;
    cmd59[2] = 0x00u;
    cmd59[3] = (uint8_t)((slot >> 8) & 0xFFu);
    cmd59[4] = (uint8_t)(slot & 0xFFu);
    cmd59[5] = 0x00u;
    cmd59[6] = 0x00u;
    cmd59[7] = 0x00u;
    cmd59[8] = 0x00u;
    printf("\nsending 0x59 to start app at slot %u:\n", slot);
    printf(" idx  send  reply  ack\n");
    for (i = 0; i < sizeof(cmd59); i++) {
        int ack = psemu_com_transfer(ps, cmd59[i], &out, g_timeout_cycles);
        printf("   %u   0x%02X   0x%02X   %s\n", i, (unsigned)cmd59[i], (unsigned)out,
               ack ? "yes" : "NO");
    }
    end_command(ps);
    print_comflags("after 0x59:", read_comflags(ps));

    /* Report RAM bytes that 0x59 changed. */
    printf("\nRAM bytes changed by 0x59:\n");
    {
        unsigned changed = 0;
        for (i = 0; i < PSEMU_RAM_SIZE; i++) {
            if (ps->bus.ram[i] != ram_before[i]) {
                printf("  RAM[0x%03X]  0x%02X -> 0x%02X\n", i,
                       (unsigned)ram_before[i], (unsigned)ps->bus.ram[i]);
                changed++;
            }
        }
        if (!changed) printf("  (none)\n");
    }

    /* Run frames to give the BIOS time to start the app and for the app to install FIQ hooks. */
    memcpy(ram_before, ps->bus.ram, PSEMU_RAM_SIZE);
    printf("\nrunning %u frames (checking psemu_app_running each frame)...\n", run_frames);
    {
        unsigned first_app_frame = 0;
        for (i = 0; i < run_frames; i++) {
            psemu_run(ps, FRAME_CYCLES);
            if (!first_app_frame && psemu_app_running(ps)) {
                first_app_frame = i + 1;
                printf("  app started running at frame %u\n", first_app_frame);
            }
        }
        if (!first_app_frame) {
            printf("  app did NOT start running in %u frames\n", run_frames);
        }
    }
    print_comflags("after run:", read_comflags(ps));

    /* Report RAM bytes that changed during the run. */
    printf("\nRAM bytes changed during run:\n");
    {
        unsigned changed = 0;
        for (i = 0; i < PSEMU_RAM_SIZE; i++) {
            if (ps->bus.ram[i] != ram_before[i]) {
                printf("  RAM[0x%03X]  0x%02X -> 0x%02X\n", i,
                       (unsigned)ram_before[i], (unsigned)ps->bus.ram[i]);
                changed++;
            }
        }
        if (!changed) printf("  (none)\n");
    }

    printf("\n0x58 after app launch:\n");
    printf(" idx  send  reply  ack\n");
    for (i = 0; i < sizeof(CMD58); i++) {
        int ack = psemu_com_transfer(ps, CMD58[i], &out, g_timeout_cycles);
        printf("   %u   0x%02X   0x%02X   %s\n", i, (unsigned)CMD58[i], (unsigned)out,
               ack ? "yes" : "NO");
    }
    end_command(ps);

    psemu_destroy(ps);
    return 0;
}

/* Boots undocked for up to max_frames, checking psemu_app_running each frame. Reports the frame
   where the app first runs. Then docks and sends command 0x58 to read the status byte. Use this
   mode to measure how many undocked frames the BIOS needs before it launches the installed app. */
static int mode_autolaunch(const char *bios_path, unsigned max_frames) {
    static const uint8_t CMD58[] = {0x81u, 0x58u, 0x00u, 0x00u, 0x00u};
    psemu_t *ps = boot(bios_path, 0u);
    unsigned first_app_frame = 0;
    unsigned settled;
    unsigned i;
    uint8_t out;

    if (!ps) {
        return 1;
    }
    printf("running up to %u undocked frames, checking psemu_app_running each frame...\n", max_frames);
    for (i = 0; i < max_frames; i++) {
        psemu_run(ps, FRAME_CYCLES);
        if (!first_app_frame && psemu_app_running(ps)) {
            first_app_frame = i + 1;
            printf("  app started running at frame %u\n", first_app_frame);
            break;
        }
    }
    if (!first_app_frame) {
        printf("  app did NOT start running in %u frames\n", max_frames);
    }

    printf("\ndocking...\n");
    settled = dock_and_settle(ps, 60);
    if (!settled) {
        printf("communication never enabled\n");
        psemu_destroy(ps);
        return 1;
    }
    printf("communication enabled after %u frame(s)\n", settled);
    print_comflags("after dock:", read_comflags(ps));

    printf("\n0x58 after undocked run:\n");
    printf(" idx  send  reply  ack\n");
    for (i = 0; i < sizeof(CMD58); i++) {
        int ack = psemu_com_transfer(ps, CMD58[i], &out, g_timeout_cycles);
        printf("   %u   0x%02X   0x%02X   %s\n", (unsigned)i, (unsigned)CMD58[i], (unsigned)out,
               ack ? "yes" : "NO");
    }
    end_command(ps);

    psemu_destroy(ps);
    return 0;
}

/* Boots, docks, sends 0x59 (Prepare File Execution at the given slot), undocks, runs up to
   run_frames checking psemu_app_running, re-docks, then sends 0x58 to see whether the app's
   FIQ hook changed the status byte from the BIOS default of 0x02. */
static int mode_test59_undock(const char *bios_path, unsigned boot_frames, unsigned slot,
                               unsigned run_frames) {
    static const uint8_t CMD58[] = {0x81u, 0x58u, 0x00u, 0x00u, 0x00u};
    uint8_t cmd59[9];
    psemu_t *ps = boot(bios_path, boot_frames);
    unsigned settled;
    unsigned first_app_frame = 0;
    unsigned i;
    uint8_t out;

    if (!ps) {
        return 1;
    }
    settled = dock_and_settle(ps, 60);
    if (!settled) {
        printf("communication never enabled after initial dock\n");
        psemu_destroy(ps);
        return 1;
    }
    printf("communication enabled after %u frame(s)\n", settled);
    print_comflags("after dock:", read_comflags(ps));

    cmd59[0] = 0x81u;
    cmd59[1] = 0x59u;
    cmd59[2] = 0x00u;
    cmd59[3] = (uint8_t)((slot >> 8) & 0xFFu);
    cmd59[4] = (uint8_t)(slot & 0xFFu);
    cmd59[5] = 0x00u; cmd59[6] = 0x00u; cmd59[7] = 0x00u; cmd59[8] = 0x00u;
    printf("\nsending 0x59 for slot %u:\n", slot);
    printf(" idx  send  reply  ack\n");
    for (i = 0; i < sizeof(cmd59); i++) {
        int ack = psemu_com_transfer(ps, cmd59[i], &out, g_timeout_cycles);
        printf("   %u   0x%02X   0x%02X   %s\n", i, (unsigned)cmd59[i], (unsigned)out,
               ack ? "yes" : "NO");
    }
    end_command(ps);
    print_comflags("after 0x59:", read_comflags(ps));

    /* Undock, then run frames undocked so the BIOS can launch the app. */
    printf("\nundocking and running %u frames (checking psemu_app_running each frame)...\n", run_frames);
    psemu_com_set_docked(ps, 0);
    for (i = 0; i < run_frames; i++) {
        psemu_run(ps, FRAME_CYCLES);
        if (!first_app_frame && psemu_app_running(ps)) {
            first_app_frame = i + 1;
            printf("  app started running at frame %u\n", first_app_frame);
        }
    }
    if (!first_app_frame) {
        printf("  app did NOT start running in %u undocked frames\n", run_frames);
    }
    print_comflags("after undocked run:", read_comflags(ps));

    /* Re-dock and settle. */
    printf("\nre-docking...\n");
    settled = dock_and_settle(ps, 60);
    if (!settled) {
        printf("communication never re-enabled after re-dock\n");
        psemu_destroy(ps);
        return 1;
    }
    printf("communication re-enabled after %u frame(s)\n", settled);
    print_comflags("after re-dock:", read_comflags(ps));

    printf("\n0x58 after undocked run:\n");
    printf(" idx  send  reply  ack\n");
    for (i = 0; i < sizeof(CMD58); i++) {
        int ack = psemu_com_transfer(ps, CMD58[i], &out, g_timeout_cycles);
        printf("   %u   0x%02X   0x%02X   %s\n", (unsigned)i, (unsigned)CMD58[i], (unsigned)out,
               ack ? "yes" : "NO");
    }
    end_command(ps);

    psemu_destroy(ps);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "usage: com_probe <bios.bin> dock [boot_frames]\n"
        "       com_probe <bios.bin> cmd <hexbyte>... [--frames N] [--trace] [--card F]\n"
        "       com_probe <bios.bin> wait [boot_frames]\n"
        "       com_probe <bios.bin> write [sector] [--card F] [--badsum] [--writeenable]\n"
        "       com_probe <bios.bin> selbit [boot_frames]\n"
        "       com_probe <bios.bin> cmd58 [boot_frames] [--card F]\n"
        "       com_probe <bios.bin> launch58 [slot] [--frames N] [--run N] [--card F]\n"
        "       com_probe <bios.bin> autolaunch [max_frames] [--card F]\n"
        "       com_probe <bios.bin> test59_undock [slot] [--frames N] [--run N] [--card F]\n"
        "\n"
        "  dock   reports each COM register access while the machine docks.\n"
        "  cmd    sends a byte sequence and reports each reply.\n"
        "         The standard Get ID command is: 81 53 00 00 00 00 00 00 00 00\n"
        "  wait   measures the cycles that one answer needs, for each byte of a command.\n"
        "  write  runs one Write Sector command, then reads the sector back out of flash.\n"
        "         sector defaults to 64, which is a data frame and not a directory frame.\n"
        "  selbit finds the COM_STAT1 bit that ends a command.\n"
        "  cmd58  sends command 0x58 after docking and prints the reply. With a trace build,\n"
        "         also shows the BIOS PCs executed and bytes written to COM_DATA.\n"
        "  launch58 sends 0x59 to start the app at <slot> (default 8), runs --run frames,\n"
        "         then sends 0x58 and reports the reply.\n"
        "  autolaunch runs up to max_frames undocked (default 3000), reports at which frame\n"
        "         psemu_app_running first returns true, then docks and sends 0x58.\n"
        "  test59_undock boots, docks, sends 0x59 at <slot> (default 8), undocks, runs --run\n"
        "         frames undocked checking psemu_app_running, re-docks, then sends 0x58.\n"
        "\n"
        "  --card F loads a card image or an app before the boot.\n"
        "  boot_frames defaults to 200. That is enough frames for the BIOS to get to its shell.\n");
}

int main(int argc, char **argv) {
    const char *bios_path;
    const char *mode;
    unsigned boot_frames = 200u;

    if (argc < 3) {
        usage();
        return 1;
    }
    bios_path = argv[1];
    mode = argv[2];

    /* --card applies to each mode, thus this loop runs before the mode dispatch. */
    {
        int i;
        for (i = 3; i < argc - 1; i++) {
            if (strcmp(argv[i], "--card") == 0) {
                g_card_path = argv[i + 1];
            } else if (strcmp(argv[i], "--timeout") == 0) {
                g_timeout_cycles = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            }
        }
    }

    if (strcmp(mode, "dock") == 0) {
        if (argc >= 4 && argv[3][0] != '-') {
            boot_frames = (unsigned)strtoul(argv[3], NULL, 10);
        }
        return mode_dock(bios_path, boot_frames);
    }
    if (strcmp(mode, "func") == 0) {
        if (argc >= 4 && argv[3][0] != '-') {
            boot_frames = (unsigned)strtoul(argv[3], NULL, 10);
        }
        return mode_func(bios_path, boot_frames);
    }
    if (strcmp(mode, "selbit") == 0) {
        if (argc >= 4 && argv[3][0] != '-') {
            boot_frames = (unsigned)strtoul(argv[3], NULL, 10);
        }
        return mode_selbit(bios_path, boot_frames);
    }
    if (strcmp(mode, "wait") == 0) {
        if (argc >= 4 && argv[3][0] != '-') {
            boot_frames = (unsigned)strtoul(argv[3], NULL, 10);
        }
        return mode_wait(bios_path, boot_frames);
    }
    if (strcmp(mode, "write") == 0) {
        uint16_t sector = 64u;
        int bad_checksum = 0;
        int enable_writes = 0;
        int i;
        if (argc >= 4 && argv[3][0] != '-') {
            sector = (uint16_t)strtoul(argv[3], NULL, 0);
        }
        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--badsum") == 0) {
                bad_checksum = 1;
            } else if (strcmp(argv[i], "--writeenable") == 0) {
                enable_writes = 1;
            } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
                boot_frames = (unsigned)strtoul(argv[++i], NULL, 10);
            }
        }
        return mode_write(bios_path, boot_frames, sector, bad_checksum, enable_writes);
    }
    if (strcmp(mode, "cmd") == 0) {
        uint8_t bytes[256];
        size_t count = 0;
        int trace = 0;
        int i;
        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
                boot_frames = (unsigned)strtoul(argv[++i], NULL, 10);
                continue;
            }
            if ((strcmp(argv[i], "--card") == 0 || strcmp(argv[i], "--timeout") == 0) && i + 1 < argc) {
                i++; /* the loop above already recorded the value */
                continue;
            }
            if (strcmp(argv[i], "--trace") == 0) {
                trace = 1;
                continue;
            }
            if (count >= sizeof(bytes)) {
                fprintf(stderr, "com_probe: too many bytes, the maximum is %u\n", (unsigned)sizeof(bytes));
                return 1;
            }
            bytes[count++] = (uint8_t)strtoul(argv[i], NULL, 16);
        }
        if (count == 0) {
            usage();
            return 1;
        }
        return mode_cmd(bios_path, boot_frames, bytes, count, trace);
    }
    if (strcmp(mode, "cmd58") == 0) {
        if (argc >= 4 && argv[3][0] != '-') {
            boot_frames = (unsigned)strtoul(argv[3], NULL, 10);
        }
        return mode_cmd58(bios_path, boot_frames);
    }
    if (strcmp(mode, "launch58") == 0) {
        unsigned slot = 8u;
        unsigned run_frames = 100u;
        int i;
        if (argc >= 4 && argv[3][0] != '-') {
            slot = (unsigned)strtoul(argv[3], NULL, 0);
        }
        for (i = 3; i < argc - 1; i++) {
            if (strcmp(argv[i], "--frames") == 0) {
                boot_frames = (unsigned)strtoul(argv[i + 1], NULL, 10);
            } else if (strcmp(argv[i], "--run") == 0) {
                run_frames = (unsigned)strtoul(argv[i + 1], NULL, 10);
            }
        }
        return mode_launch58(bios_path, boot_frames, slot, run_frames);
    }
    if (strcmp(mode, "autolaunch") == 0) {
        unsigned max_frames = 3000u;
        if (argc >= 4 && argv[3][0] != '-') {
            max_frames = (unsigned)strtoul(argv[3], NULL, 10);
        }
        return mode_autolaunch(bios_path, max_frames);
    }
    if (strcmp(mode, "test59_undock") == 0) {
        unsigned slot = 8u;
        unsigned run_frames = 500u;
        int i;
        if (argc >= 4 && argv[3][0] != '-') {
            slot = (unsigned)strtoul(argv[3], NULL, 0);
        }
        for (i = 3; i < argc - 1; i++) {
            if (strcmp(argv[i], "--frames") == 0) {
                boot_frames = (unsigned)strtoul(argv[i + 1], NULL, 10);
            } else if (strcmp(argv[i], "--run") == 0) {
                run_frames = (unsigned)strtoul(argv[i + 1], NULL, 10);
            }
        }
        return mode_test59_undock(bios_path, boot_frames, slot, run_frames);
    }

    usage();
    return 1;
}
