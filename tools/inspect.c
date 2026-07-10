#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
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

static const char *mode_name(uint32_t cpsr) {
    switch (cpsr & CPSR_MODE_MASK) {
    case ARM_MODE_USR:
        return "USR";
    case ARM_MODE_FIQ:
        return "FIQ";
    case ARM_MODE_IRQ:
        return "IRQ";
    case ARM_MODE_SVC:
        return "SVC";
    case ARM_MODE_ABT:
        return "ABT";
    case ARM_MODE_UND:
        return "UND";
    case ARM_MODE_SYS:
        return "SYS";
    default:
        return "???";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <bios.bin> [app.bin] [max_instructions]\n", argv[0]);
        return 1;
    }

    size_t bios_size = 0;
    uint8_t *bios = read_file(argv[1], &bios_size);
    if (!bios) {
        fprintf(stderr, "failed to read bios %s\n", argv[1]);
        return 1;
    }

    psemu_t *ps = psemu_create();
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK) {
        fprintf(stderr, "bad bios size: %zu (need %d)\n", bios_size, PSEMU_BIOS_SIZE);
        return 1;
    }
    free(bios);

    if (argc >= 3) {
        size_t app_size = 0;
        uint8_t *app = read_file(argv[2], &app_size);
        if (!app) {
            fprintf(stderr, "failed to read app %s\n", argv[2]);
            return 1;
        }
        psemu_status st = psemu_load_app(ps, app, app_size);
        printf(st == PSEMU_OK ? "app loaded ok\n" : "app load failed: %d\n", st);
        free(app);
    }

    psemu_reset(ps);

    long max_instr = argc >= 4 ? atol(argv[3]) : 2000000;
    uint32_t last_mode = ps->cpu.cpsr & CPSR_MODE_MASK;
    uint32_t last_pc = ps->cpu.r[15];
    long same_pc_count = 0;

    for (long i = 0; i < max_instr; i++) {
        uint32_t pc_before = ps->cpu.r[15];
        uint32_t cpsr_before = ps->cpu.cpsr;

        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        if (timer_tick(&ps->timer, step_cycles)) {
            arm_request_irq(&ps->cpu);
        }

        if (ps->cpu.unimplemented) {
            printf(
                "UNIMPLEMENTED opcode at instr #%ld, pc=0x%08X mode=%s cpsr=0x%08X\n", i, pc_before,
                mode_name(cpsr_before), cpsr_before);
            uint32_t raw = (cpsr_before & CPSR_T) ? psemu_bus_read16(&ps->bus, pc_before)
                                                   : psemu_bus_read32(&ps->bus, pc_before);
            printf("  raw opcode: 0x%08X (%s)\n", raw, (cpsr_before & CPSR_T) ? "thumb" : "arm");
            break;
        }

        uint32_t new_mode = ps->cpu.cpsr & CPSR_MODE_MASK;
        if (new_mode != last_mode) {
            printf(
                "instr #%ld: mode %s -> %s at pc=0x%08X (came from pc=0x%08X)\n", i, mode_name(cpsr_before),
                mode_name(ps->cpu.cpsr), ps->cpu.r[15], pc_before);
            last_mode = new_mode;
        }

        if (ps->cpu.r[15] == last_pc) {
            same_pc_count++;
            if (same_pc_count == 1000) {
                printf(
                    "instr #%ld: stuck at pc=0x%08X for 1000+ consecutive steps (tight loop or halt)\n", i,
                    ps->cpu.r[15]);
            }
        } else {
            same_pc_count = 0;
        }
        last_pc = ps->cpu.r[15];

        if (i % 200000 == 0) {
            printf(
                "instr #%ld: pc=0x%08X mode=%s cpsr=0x%08X r0=0x%08X r13=0x%08X r14=0x%08X\n", i, ps->cpu.r[15],
                mode_name(ps->cpu.cpsr), ps->cpu.cpsr, ps->cpu.r[0], ps->cpu.r[13], ps->cpu.r[14]);
        }
    }

    printf("\nfinal state: pc=0x%08X mode=%s cpsr=0x%08X\n", ps->cpu.r[15], mode_name(ps->cpu.cpsr), ps->cpu.cpsr);
    for (int i = 0; i < 16; i++) {
        printf("  r%-2d = 0x%08X\n", i, ps->cpu.r[i]);
    }

    int fb_changed = psemu_framebuffer_dirty(ps);
    printf("\nLCD framebuffer %s. Final contents:\n", fb_changed ? "changed at least once" : "never changed");
    const uint8_t *fb = psemu_get_framebuffer(ps);
    for (int row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (int col = 0; col < PSEMU_LCD_WIDTH; col++) {
            int byte_index = row * PSEMU_LCD_STRIDE + col / 8;
            int bit_index = col % 8;
            int on = (fb[byte_index] >> bit_index) & 1;
            putchar(on ? '#' : '.');
        }
        putchar('\n');
    }

    psemu_destroy(ps);
    return 0;
}
