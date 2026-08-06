/* What the kernel does with the communication port, and how it answers the PS1.

   The communication port (core/src/com.h) is the link to a PS1. The link goes through the memory
   card connector. A published register map gives the address of each register in that block. It does
   NOT give the function of the COM_CTRL1 bits or the COM_CTRL2 bits. It also does not give the event
   that clears the Ready bit of COM_STAT2. It records only the values that it observed.

   The kernel of a real BIOS holds the answer to each of those questions, because the kernel operates
   this hardware. This tool reads the answer out of a real BIOS dump. tools/datetime_probe.c uses the
   same method for the date and time settings.

   The model in core/src/com.c makes two inferences. This tool must confirm or replace both of them:
   - A read of COM_DATA clears the Ready bit of COM_STAT2. The other candidate is a write to
     COM_CTRL2.
   - A write to COM_MODE that sets bit 1 (/ACK Output Level) ends one byte exchange.

   Modes:
     dock - boots, docks the machine, and reports each COM register access with its PC. It also
            reports the ComFlags word before and after the transition. Use this mode to find the
            initialization sequence and the address of the COM code of the kernel.
     cmd  - docks, then sends a byte sequence to the machine and reports the reply to each byte.
            Give the bytes in hex. The standard Get ID command is "81 53 00 00 00 00 00 00 00 00".
     wait - docks and reports only how many cycles the kernel needs to answer the first byte. Use
            this mode to size PSEMU_COM_DEFAULT_TIMEOUT_CYCLES.

   usage: com_probe <bios.bin> dock [boot_frames]
          com_probe <bios.bin> cmd <hexbyte>... [--frames N]
          com_probe <bios.bin> wait [boot_frames] */

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
        int ack = psemu_com_transfer(ps, bytes[i], &out, PSEMU_COM_DEFAULT_TIMEOUT_CYCLES);
        printf(" %3u   0x%02X   0x%02X   %s\n", (unsigned)i, (unsigned)bytes[i], (unsigned)out, ack ? "yes" : "NO");
        if (!ack) {
            printf("       no acknowledge. A real console stops the transfer here.\n");
        }
    }
    psemu_com_trace_enabled = 0;

    printf("\n");
    print_comflags("after: ", read_comflags(ps));
    psemu_destroy(ps);
    return 0;
}

static int mode_wait(const char *bios_path, unsigned boot_frames) {
    psemu_t *ps = boot(bios_path, boot_frames);
    unsigned settled;
    uint32_t budget;
    if (!ps) {
        return 1;
    }

    settled = dock_and_settle(ps, 60);
    printf("communication enabled: %s\n", settled ? "yes" : "NO");

    /* Find the smallest budget that still gets an acknowledge. Each attempt uses a new transfer of
       the same first command byte, thus each attempt starts from the same kernel state. */
    for (budget = 32u; budget <= 262144u; budget *= 2u) {
        uint8_t out = 0xFF;
        int ack = psemu_com_transfer(ps, 0x81u, &out, budget);
        printf("budget %7u cycles: ack=%-3s reply=0x%02X\n", budget, ack ? "yes" : "no", (unsigned)out);
        if (ack) {
            break;
        }
    }

    psemu_destroy(ps);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "usage: com_probe <bios.bin> dock [boot_frames]\n"
        "       com_probe <bios.bin> cmd <hexbyte>... [--frames N]\n"
        "       com_probe <bios.bin> wait [boot_frames]\n"
        "\n"
        "  dock  reports each COM register access while the machine docks.\n"
        "  cmd   sends a byte sequence and reports each reply.\n"
        "        The standard Get ID command is: 81 53 00 00 00 00 00 00 00 00\n"
        "  wait  finds the cycle budget that one answer needs.\n"
        "\n"
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

    if (strcmp(mode, "dock") == 0) {
        if (argc >= 4) {
            boot_frames = (unsigned)strtoul(argv[3], NULL, 10);
        }
        return mode_dock(bios_path, boot_frames);
    }
    if (strcmp(mode, "wait") == 0) {
        if (argc >= 4) {
            boot_frames = (unsigned)strtoul(argv[3], NULL, 10);
        }
        return mode_wait(bios_path, boot_frames);
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

    usage();
    return 1;
}
