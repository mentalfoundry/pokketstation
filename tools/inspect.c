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

static void print_framebuffer(const psemu_t *ps) {
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
        fprintf(
            stderr, "usage: %s <bios.bin> [app.bin] [max_instructions] [button_sim] [select_block] [raw]\n",
            argv[0]);
        fprintf(
            stderr, "  select_block: pokes RAM u16 @0x00D0 to this value after reset (app-slot selector)\n");
        fprintf(
            stderr, "  raw: if \"raw\", app.bin is memcpy'd directly into flash (bypassing psemu_load_app's\n"
                    "       Title Sector validation) - use for a full directory+data flash image\n");
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

    int raw_flash = argc >= 7 && strcmp(argv[6], "raw") == 0;

    if (argc >= 3) {
        size_t app_size = 0;
        uint8_t *app = read_file(argv[2], &app_size);
        if (!app) {
            fprintf(stderr, "failed to read app %s\n", argv[2]);
            return 1;
        }
        if (raw_flash) {
            size_t copy_size = app_size < sizeof(ps->flash.data) ? app_size : sizeof(ps->flash.data);
            memcpy(ps->flash.data, app, copy_size);
            printf("raw flash image loaded: %zu bytes\n", copy_size);
        } else {
            psemu_status st = psemu_load_app(ps, app, app_size);
            printf(st == PSEMU_OK ? "app loaded ok\n" : "app load failed: %d\n", st);
        }
        free(app);
    }

    psemu_reset(ps);

    int select_block = argc >= 6 ? atoi(argv[5]) : 0;
    if (select_block > 0) {
        printf("forcing RAM u16 @0x00D0 = %d every instruction (diagnostic)\n", select_block);
    }

    long max_instr = argc >= 4 ? atol(argv[3]) : 2000000;
    int button_sim = argc >= 5 && atoi(argv[4]) != 0;
    uint32_t last_mode = ps->cpu.cpsr & CPSR_MODE_MASK;
    uint32_t last_pc = ps->cpu.r[15];
    long same_pc_count = 0;
    long fb_changes = 0;
    long fb_prints = 0;

    for (long i = 0; i < max_instr; i++) {
        uint32_t pc_before = ps->cpu.r[15];
        uint32_t cpsr_before = ps->cpu.cpsr;

        if (button_sim) {
            /* Hold Fire for 1000 instructions out of every 20000, so the app
               has a chance to see both an edge (press) and a held state. */
            long phase = i % 20000;
            psemu_set_buttons(ps, phase < 1000 ? PSEMU_BUTTON_FIRE : 0);
        }

        if (select_block > 0) {
            ps->bus.ram[0xD0] = (uint8_t)(select_block & 0xFF);
            ps->bus.ram[0xD1] = (uint8_t)((select_block >> 8) & 0xFF);
        }

        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        if (timer_tick(&ps->timer, step_cycles)) {
            arm_request_irq(&ps->cpu);
        }

        if (psemu_framebuffer_dirty(ps)) {
            fb_changes++;
            if (fb_prints < 20) {
                printf("instr #%ld: LCD framebuffer changed:\n", i);
                print_framebuffer(ps);
                fb_prints++;
            }
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

    printf("\nLCD framebuffer changed %ld time(s) total (shown %ld above). Final contents:\n", fb_changes, fb_prints);
    print_framebuffer(ps);

    psemu_destroy(ps);
    return 0;
}
