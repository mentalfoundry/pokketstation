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
            stderr,
            "usage: %s <bios.bin> [app.bin] [max_instructions] [button_sim] [select_block] [raw] [dock]\n",
            argv[0]);
        fprintf(
            stderr,
            "  button_sim: 1 = periodic Fire only; 2 = Down-then-Fire navigation sequence;\n"
            "              3 = real-hardware-confirmed power-on sequence: HELLO animation ->\n"
            "              heart -> beep -> time-setting screen, then Down once then Action\n");
        fprintf(
            stderr, "  select_block: pokes RAM u16 @0x00D0 to this value after reset (app-slot selector)\n");
        fprintf(
            stderr, "  raw: if \"raw\", app.bin is memcpy'd directly into flash (bypassing psemu_load_app's\n"
                    "       Title Sector validation) - use for a full directory+data flash image\n");
        fprintf(
            stderr, "  dock: if \"dock\", asserts INT_IOP (docked-to-PSX sensing) once at instr #5000 -\n"
                    "        This is a real wake/launch trigger, separate from buttons\n");
        fprintf(
            stderr, "  intctrace: if \"intctrace\", logs every real INTC register access with its real PC\n"
                    "        for instr #0-60000 (one full button_sim=2 navigation cycle)\n");
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
    int dock_sim = argc >= 8 && strcmp(argv[7], "dock") == 0;
    int intc_trace_sim = argc >= 9 && strcmp(argv[8], "intctrace") == 0;

    if (argc >= 3) {
        size_t app_size = 0;
        uint8_t *app = read_file(argv[2], &app_size);
        if (!app) {
            fprintf(stderr, "failed to read app %s\n", argv[2]);
            return 1;
        }
        if (raw_flash) {
            psemu_status st = psemu_load_flash_image(ps, app, app_size);
            if (st == PSEMU_OK) {
                printf("raw flash image loaded: %zu bytes\n", app_size);
            } else {
                printf("raw flash image load failed: %d\n", st);
            }
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
    } else if (select_block < 0) {
        /* Negative: poke once at reset instead of every instruction - the
           real BIOS may use 0x00D0 as scratch for something unrelated
           later in execution, and forcing it every step could corrupt
           that instead of just selecting an app slot. */
        int slot = -select_block;
        ps->bus.ram[0xD0] = (uint8_t)(slot & 0xFF);
        ps->bus.ram[0xD1] = (uint8_t)((slot >> 8) & 0xFF);
        printf("poked RAM u16 @0x00D0 = %d once at reset (diagnostic)\n", slot);
    }

    long max_instr = argc >= 4 ? atol(argv[3]) : 2000000;
    int button_sim = argc >= 5 ? atoi(argv[4]) : 0;
    uint32_t last_mode = ps->cpu.cpsr & CPSR_MODE_MASK;
    uint32_t last_pc = ps->cpu.r[15];
    long same_pc_count = 0;
    long fb_changes = 0;
    long fb_prints = 0;
    int dumped_vectors = 0;
    long fb_ever_nonzero_at = -1;
    long fb_nonzero_events = 0;
    int fb_was_nonzero = 0;
    long flash1_first_pc_at = -1;
    long flash1_hit_count = 0;
    long dispatch_first_pc_at = -1;
    long dispatch_hit_count = 0;

#define TRACE_SIZE 4000
    static uint32_t trace_pc[TRACE_SIZE];
    static uint32_t trace_cpsr[TRACE_SIZE];
    long trace_pos = 0;
    int dumped_halt_trace = 0;

    for (long i = 0; i < max_instr; i++) {
        uint32_t pc_before = ps->cpu.r[15];
        uint32_t cpsr_before = ps->cpu.cpsr;

        if (intc_trace_sim) {
            psemu_intc_trace_enabled = (i < 60000);
        }

        trace_pc[trace_pos % TRACE_SIZE] = pc_before;
        trace_cpsr[trace_pos % TRACE_SIZE] = cpsr_before;
        trace_pos++;

        if (pc_before == 0x020015DAu) {
            printf("instr #%ld: about to 'bx lr' at 0x15DA, lr=0x%08X\n", i, ps->cpu.r[14]);
        }

        if (pc_before == 0x040001C8u && !dumped_halt_trace) {
            dumped_halt_trace = 1;
            long count = trace_pos < TRACE_SIZE ? trace_pos : TRACE_SIZE;
            long start = trace_pos < TRACE_SIZE ? 0 : trace_pos % TRACE_SIZE;
            printf("=== entered halt trap at instr #%ld; last %ld PCs leading up to it ===\n", i, count);
            for (long k = 0; k < count; k++) {
                long idx = (start + k) % TRACE_SIZE;
                printf(
                    "  [%ld] pc=0x%08X %s\n", i - count + k + 1, trace_pc[idx],
                    (trace_cpsr[idx] & CPSR_T) ? "(thumb)" : "(arm)");
            }
        }

        if (button_sim == 1) {
            /* Hold Fire for 1000 instructions out of every 20000, so the app
               has a chance to see both an edge (press) and a held state. */
            long phase = i % 20000;
            psemu_set_buttons(ps, phase < 1000 ? PSEMU_BUTTON_FIRE : 0);
        } else if (button_sim == 2) {
            /* Navigation sequence: Down (move selection), then Fire
               (confirm/launch) - the system-tick callback only ever
               tests the Action-button hold bit, never Up/Right/Down/Left,
               so plain repeated Fire presses may never actually navigate
               the real menu. 60000-instruction phase: Down for 1000,
               gap, Fire for 1000, gap. */
            long phase = i % 60000;
            uint32_t buttons = 0;
            if (phase < 1000) {
                buttons = PSEMU_BUTTON_DOWN;
            } else if (phase >= 30000 && phase < 31000) {
                buttons = PSEMU_BUTTON_FIRE;
            }
            psemu_set_buttons(ps, buttons);
        } else if (button_sim == 3) {
            /* Real-hardware-confirmed power-on sequence (from directly
               testing a real PocketStation with a fresh battery insert):
               1) "HELLO" renders one letter at a time, 2) a heart icon
               shows for a bit, 3) a beep, 4) the time-setting screen
               appears - press Down once, then Action, to continue, which
               5) lands on a clock screen - Right from there moves to the
               first app in the list (then presumably Action launches it -
               not yet confirmed on real hardware, but the natural next
               step). Each real tap is ~40ms, which at the real ~4MHz
               clock is roughly 160000 cycles - a much earlier version of
               this simulation held each button for only 500 instructions
               (~300x too short given the 1-cycle-per-instruction
               approximation), which could fail to register at all if the
               BIOS debounces or requires a minimum press duration.
               2500000-instruction phase, generously spaced so each stage
               of the animation/screen transition has time to settle
               before the next press, repeating so a later cycle still
               lands correctly if an earlier one is too early: Down
               (200000-350000), Action (500000-650000), Right
               (900000-1050000), Action (1300000-1450000), gap. */
            long phase = i % 2500000;
            uint32_t buttons = 0;
            if (phase >= 200000 && phase < 350000) {
                buttons = PSEMU_BUTTON_DOWN;
            } else if (phase >= 500000 && phase < 650000) {
                buttons = PSEMU_BUTTON_FIRE;
            } else if (phase >= 900000 && phase < 1050000) {
                buttons = PSEMU_BUTTON_RIGHT;
            } else if (phase >= 1300000 && phase < 1450000) {
                buttons = PSEMU_BUTTON_FIRE;
            }
            psemu_set_buttons(ps, buttons);
        } else if (button_sim == 4) {
            /* Diagnostic: hold Fire down continuously from instr #0 for
               the entire run, with no release - matching a user
               physically holding a key down in the interactive desktop
               frontend, to check whether a genuinely sustained level on
               the INTC hold bit causes the CPU to livelock (immediately
               re-entering the IRQ handler on every subsequent step,
               never letting the resumed USR instruction execute) rather
               than letting real forward progress continue while held. */
            psemu_set_buttons(ps, PSEMU_BUTTON_FIRE);
        }

        if (select_block > 0) {
            ps->bus.ram[0xD0] = (uint8_t)(select_block & 0xFF);
            ps->bus.ram[0xD1] = (uint8_t)((select_block >> 8) & 0xFF);
        }

        if (dock_sim && i == 5000) {
            printf("instr #%ld: simulating dock-to-PSX (asserting INT_IOP)\n", i);
            intc_set_line(&ps->intc, INT_IOP, 1);
        }

        if (pc_before == 0x04001CF4u) {
            printf("instr #%ld: docking IRQ handler (0x4001cf4) entered\n", i);
        }

        {
            /* BIOS code coverage: which addresses actually get executed
               across the whole run, to sanity-check how far execution
               has really gotten relative to the dispatch function.
               Bucketed at 16 bytes so nearby instructions merge into
               visible code regions instead of one line per instruction. */
#define COVERAGE_BUCKET 16u
            static uint8_t covered[PSEMU_BIOS_SIZE / COVERAGE_BUCKET + 1];
            if (pc_before >= PSEMU_BIOS_BASE && pc_before < PSEMU_BIOS_BASE + PSEMU_BIOS_SIZE) {
                covered[(pc_before - PSEMU_BIOS_BASE) / COVERAGE_BUCKET] = 1;
            }
            if (i == max_instr - 1) {
                long num_buckets = (long)(PSEMU_BIOS_SIZE / COVERAGE_BUCKET);
                long run_start = -1;
                printf("=== BIOS code coverage ranges (16-byte buckets) ===\n");
                for (long b = 0; b <= num_buckets; b++) {
                    int hit = (b < num_buckets) && covered[b];
                    if (hit && run_start < 0) {
                        run_start = b;
                    } else if (!hit && run_start >= 0) {
                        printf(
                            "  0x%08X - 0x%08X\n", (unsigned)(PSEMU_BIOS_BASE + run_start * COVERAGE_BUCKET),
                            (unsigned)(PSEMU_BIOS_BASE + b * COVERAGE_BUCKET - 1));
                        run_start = -1;
                    }
                }
            }
        }

        {
            /* Watch the RTC's mode/control registers for ANY change, to
               see whether button presses are actually reaching the
               real clock-setup wizard's input processing at all. */
            static uint32_t last_mode = 0xFFFFFFFFu, last_control = 0xFFFFFFFFu;
            if (ps->rtc.mode != last_mode) {
                printf("instr #%ld: RTC mode changed 0x%08X -> 0x%08X, pc=0x%08X\n", i, last_mode, ps->rtc.mode,
                       ps->cpu.r[15]);
                last_mode = ps->rtc.mode;
            }
            if (ps->rtc.control != last_control) {
                printf(
                    "instr #%ld: RTC control changed 0x%08X -> 0x%08X, pc=0x%08X\n", i, last_control,
                    ps->rtc.control, ps->cpu.r[15]);
                last_control = ps->rtc.control;
            }
        }

        if (pc_before == 0x04001AF8u) {
            static long count = 0;
            count++;
            if (count <= 10) {
                printf(
                    "instr #%ld: dispatch-function tail (0x4001af8) reached, count=%ld, r3(entry pt)=0x%08X\n", i,
                    count, ps->cpu.r[3]);
            }
        }

        if (pc_before == 0x04003784u) {
            printf("instr #%ld: button-action hold-bit handler (0x4003784) entered\n", i);
        }
        if (pc_before == 0x04000672u) {
            printf("instr #%ld: RTC hold-bit ack/bookkeeping handler (0x4000672) entered\n", i);
        }

        if (pc_before == 0x04001414u) {
            static uint32_t last_cb1 = 0xFFFFFFFFu, last_cb2 = 0xFFFFFFFFu;
            uint32_t cb1 = psemu_bus_read32(&ps->bus, 0xFCu);
            uint32_t cb2 = psemu_bus_read32(&ps->bus, 0x100u);
            if (cb1 != last_cb1 || cb2 != last_cb2) {
                printf("instr #%ld: IRQ callback hooks: [0xFC]=0x%08X [0x100]=0x%08X\n", i, cb1, cb2);
                last_cb1 = cb1;
                last_cb2 = cb2;
            }
        }

        {
            static uint32_t last_enable = 0;
            if (ps->intc.enable != last_enable) {
                printf(
                    "instr #%ld: INTC enable changed: 0x%08X -> 0x%08X (IOP bit %s)\n", i, last_enable,
                    ps->intc.enable, (ps->intc.enable & INT_IOP) ? "SET" : "clear");
                last_enable = ps->intc.enable;
            }
        }

        if (pc_before >= 0x02000000u && pc_before < 0x03000000u) {
            flash1_hit_count++;
            if (flash1_first_pc_at < 0) {
                flash1_first_pc_at = i;
                printf("instr #%ld: FIRST execution inside FLASH1 (app code), pc=0x%08X\n", i, pc_before);
            }
        }

        if (pc_before >= 0x04001900u && pc_before < 0x04001C00u && !(cpsr_before & CPSR_T)) {
            dispatch_hit_count++;
            if (dispatch_first_pc_at < 0) {
                dispatch_first_pc_at = i;
                printf("instr #%ld: FIRST execution inside app-dispatch routine, pc=0x%08X\n", i, pc_before);
            }
            printf(
                "  [dispatch] instr #%ld: pc=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X lr=0x%08X\n", i,
                pc_before, ps->cpu.r[0], ps->cpu.r[1], ps->cpu.r[2], ps->cpu.r[3], ps->cpu.r[14]);
        }

        uint32_t step_cycles = arm7tdmi_step(&ps->cpu);
        timer_tick(&ps->timer, &ps->intc, step_cycles);
        rtc_tick(&ps->rtc, &ps->intc, step_cycles);

        {
            /* Watch the menu loop's action-dispatch value ([r4+0x24] with
               r4=0x230 -> abs 0x254) and its other dispatch source
               ([r7+0x20] with r7=0x3D0 -> abs 0x3F0) for ANY write, to
               find what actually drives menu navigation empirically
               rather than by continuing to hand-trace disassembly. */
            static uint32_t last_254 = 0xFFFFFFFFu, last_3f0 = 0xFFFFFFFFu;
            uint32_t v254 = psemu_bus_read32(&ps->bus, 0x254u);
            uint32_t v3f0 = psemu_bus_read32(&ps->bus, 0x3F0u);
            if (v254 != last_254) {
                printf("instr #%ld: [0x254] changed 0x%08X -> 0x%08X, pc(after)=0x%08X\n", i, last_254, v254,
                       ps->cpu.r[15]);
                last_254 = v254;
            }
            if (v3f0 != last_3f0) {
                printf("instr #%ld: [0x3F0] changed 0x%08X -> 0x%08X, pc(after)=0x%08X\n", i, last_3f0, v3f0,
                       ps->cpu.r[15]);
                last_3f0 = v3f0;
            }
        }

        if (i % 2000000 == 0 || i == 5000 || i == 10000 || i == 20000 || i == 50000 || i == 100000 ||
            i == 200000 || i == 500000 || i == 1000000) {
            printf("instr #%ld: periodic framebuffer snapshot:\n", i);
            print_framebuffer(ps);
        }

        if (psemu_framebuffer_dirty(ps)) {
            fb_changes++;
            if (fb_prints < 20) {
                printf("instr #%ld: LCD framebuffer changed:\n", i);
                print_framebuffer(ps);
                fb_prints++;
            }
            {
                const uint8_t *fb = psemu_get_framebuffer(ps);
                int is_nonzero = 0;
                for (int b = 0; b < PSEMU_LCD_HEIGHT * PSEMU_LCD_STRIDE; b++) {
                    if (fb[b] != 0) {
                        is_nonzero = 1;
                        break;
                    }
                }
                if (is_nonzero) {
                    fb_nonzero_events++;
                    if (fb_ever_nonzero_at < 0) {
                        fb_ever_nonzero_at = i;
                        printf("instr #%ld: FIRST NONZERO framebuffer byte, pc=0x%08X:\n", i, ps->cpu.r[15]);
                        print_framebuffer(ps);
                    }
                    if (!fb_was_nonzero && fb_nonzero_events > 1) {
                        printf("instr #%ld: framebuffer went nonzero again after being cleared, pc=0x%08X\n", i, ps->cpu.r[15]);
                    }
                } else if (fb_was_nonzero) {
                    printf("instr #%ld: framebuffer CLEARED back to all-zero, pc=0x%08X\n", i, ps->cpu.r[15]);
                }
                fb_was_nonzero = is_nonzero;
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

        /* An SWI vector entry (pc==8) reached from inside FLASH1 is the app
           issuing a real syscall - decode the syscall number and dispatch
           table entry the same way the real BIOS handler does. */
        if (pc_before == 8 && ps->cpu.r[14] >= 0x02000000u && ps->cpu.r[14] < 0x03000000u) {
            uint32_t caller_cpsr = ps->cpu.spsr_bank[arm_current_bank(&ps->cpu)];
            uint32_t swi_addr = (caller_cpsr & CPSR_T) ? ps->cpu.r[14] - 2u : ps->cpu.r[14] - 4u;
            uint32_t swi_word = (caller_cpsr & CPSR_T) ? psemu_bus_read16(&ps->bus, swi_addr)
                                                        : psemu_bus_read32(&ps->bus, swi_addr);
            uint32_t syscall_num = swi_word & 0xFFu;
            uint32_t table_base = psemu_bus_read32(&ps->bus, 0xE0u);
            uint32_t handler = psemu_bus_read32(&ps->bus, table_base + syscall_num * 4u);
            printf(
                "  app SWI from 0x%08X: syscall #0x%02X, table_base=0x%08X, handler=0x%08X\n", swi_addr,
                syscall_num, table_base, handler);
        }

        if (pc_before == 8 && !dumped_vectors) {
            dumped_vectors = 1;
            printf("vector table @ instr #%ld:\n", i);
            for (int v = 0; v < 16; v++) {
                printf("  0x%02X: 0x%08X\n", v * 4, psemu_bus_read32(&ps->bus, (uint32_t)v * 4));
            }
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
    printf("\nFLASH1 (app code) executed %ld instruction(s) total; first at #%ld\n", flash1_hit_count, flash1_first_pc_at);
    printf("app-dispatch routine executed %ld instruction(s) total; first at #%ld\n", dispatch_hit_count, dispatch_first_pc_at);
    if (fb_ever_nonzero_at >= 0) {
        printf(
            "\nfirst nonzero framebuffer byte observed at instr #%ld (nonzero at %ld of %ld dirty events)\n",
            fb_ever_nonzero_at, fb_nonzero_events, fb_changes);
    } else {
        printf("\nframebuffer was ALL ZERO for the entire run (every dirty event wrote all-zero data)\n");
    }

    psemu_destroy(ps);
    return 0;
}
