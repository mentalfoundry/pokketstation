/* Checks whether the Chocobo World app writes to flash when the user selects Exit from the
   continue/exit screen.

   The tool boots the app from cold, runs the standard BIOS navigation sequence to launch it,
   holds the action button until the exit prompt appears, selects Exit, and watches for any
   flash writes using the psemu_trace write hook.

   Output per run:
     - The LCD screen at boot-navigation end, after the hold, and after the exit sequence.
     - Every attempted flash write (address, value, PC) — first FLASH_WATCH_SAMPLES printed in
       full; total count for each region printed at the end.
     - Card blocks that changed relative to the state at launch (per-block byte count).

   usage: choco_exit_probe <bios.bin> <choco.mcs> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psemu_internal.h"

#define FRAME_CYCLES  33000u
#define BOOT_FRAMES   600     /* standard bench_probe boot + app launch sequence */
#define HOLD_FRAMES   300     /* hold action button; long enough to clear any hold threshold */
#define SETTLE_FRAMES 120     /* frames after the exit confirm to let the BIOS run */

#define FLASH_WATCH_SAMPLES 8u

static struct {
    unsigned long flash1_writes;
    unsigned long flash2_writes;
    unsigned long ctrl_writes;
    unsigned long samples_printed;
} fw;

static void flash_write_cb(uint32_t addr, uint8_t value, uint32_t pc)
{
    int is1 = addr >= PSEMU_FLASH1_BASE && addr < PSEMU_FLASH1_BASE + PSEMU_FLASH_SIZE;
    int is2 = addr >= PSEMU_FLASH2_BASE && addr < PSEMU_FLASH2_BASE + PSEMU_FLASH_SIZE;
    int isc = addr >= PSEMU_FLASH_CTRL_BASE && addr < PSEMU_FLASH_CTRL_BASE + FLASH_CTRL_SPAN;
    const char *region;
    uint32_t offset;

    if (!is1 && !is2 && !isc)
        return;

    if (is1)      { fw.flash1_writes++; region = "FLASH1"; offset = addr - PSEMU_FLASH1_BASE; }
    else if (is2) { fw.flash2_writes++; region = "FLASH2"; offset = addr - PSEMU_FLASH2_BASE; }
    else          { fw.ctrl_writes++;   region = "FLASH_CTRL"; offset = addr - PSEMU_FLASH_CTRL_BASE; }

    /* Only print FLASH2 data writes — those are the save-data writes we care about.
       FLASH_CTRL bank-select writes are counted but not printed; they are BIOS housekeeping
       and would drown out the data writes. */
    if (!isc && fw.samples_printed < FLASH_WATCH_SAMPLES) {
        printf("  [flash write] %s +0x%05X (block %u) = 0x%02X from pc=0x%08X\n",
               region, (unsigned)offset, (unsigned)(offset / 0x2000u), value, pc);
        fw.samples_printed++;
    }
}

static void print_framebuffer(const psemu_t *ps, const char *label)
{
    const uint8_t *fb = psemu_get_framebuffer(ps);
    int row, col;
    printf("--- %s ---\n", label);
    for (row = 0; row < PSEMU_LCD_HEIGHT; row++) {
        for (col = 0; col < PSEMU_LCD_WIDTH; col++) {
            int bi = row * PSEMU_LCD_STRIDE + col / 8;
            int bn = col % 8;
            putchar((fb[bi] >> bn) & 1 ? '#' : '.');
        }
        putchar('\n');
    }
}

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

int main(int argc, char **argv)
{
    size_t bios_size = 0, app_size = 0;
    uint8_t *bios, *app;
    psemu_t *ps;
    uint8_t *flash_at_launch;
    long f;
    int pc_in_bios;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <bios.bin> <choco.mcs>\n", argv[0]);
        return 1;
    }
    bios = read_file(argv[1], &bios_size);
    app  = read_file(argv[2], &app_size);
    if (!bios || !app) {
        fprintf(stderr, "failed to read input files\n");
        return 1;
    }

    ps = psemu_create();
    if (psemu_load_bios(ps, bios, bios_size) != PSEMU_OK ||
        psemu_load_content(ps, app, app_size) != PSEMU_OK) {
        fprintf(stderr, "bad BIOS or app image\n");
        return 1;
    }
    psemu_reset(ps);

    /* Standard BIOS navigation: Down, Fire, Right, Fire (repeating 240-frame cycle).
       Repeated twice so a press that misses one animation cycle lands on the next. */
    printf("booting and launching app (%d frames)...\n", BOOT_FRAMES);
    for (f = 0; f < BOOT_FRAMES; f++) {
        long phase = f % 240;
        uint32_t buttons = 0;
        if      (phase >= 20  && phase < 36)  buttons = PSEMU_BUTTON_DOWN;
        else if (phase >= 50  && phase < 66)  buttons = PSEMU_BUTTON_FIRE;
        else if (phase >= 90  && phase < 106) buttons = PSEMU_BUTTON_RIGHT;
        else if (phase >= 130 && phase < 146) buttons = PSEMU_BUTTON_FIRE;
        psemu_set_buttons(ps, buttons);
        psemu_run(ps, FRAME_CYCLES);
    }
    if (psemu_cpu_faulted(ps)) {
        fprintf(stderr, "FAIL: CPU faulted during boot/launch, pc=0x%08X\n", ps->cpu.r[15]);
        return 1;
    }
    print_framebuffer(ps, "screen after boot navigation");

    /* Snapshot flash here — this is the baseline for the change report below. */
    flash_at_launch = (uint8_t *)malloc(PSEMU_FLASH_SIZE);
    if (!flash_at_launch) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    memcpy(flash_at_launch, ps->flash.data, PSEMU_FLASH_SIZE);

    /* Arm the write hook now so it only fires during and after the exit sequence. */
    psemu_bus_write_trace_cb = flash_write_cb;

    /* Hold the action button. The app shows the continue/exit prompt after a sustained press. */
    printf("holding action button (%d frames)...\n", HOLD_FRAMES);
    psemu_set_buttons(ps, PSEMU_BUTTON_FIRE);
    for (f = 0; f < HOLD_FRAMES; f++)
        psemu_run(ps, FRAME_CYCLES);
    if (psemu_cpu_faulted(ps)) {
        fprintf(stderr, "FAIL: CPU faulted while holding action, pc=0x%08X\n", ps->cpu.r[15]);
        return 1;
    }
    print_framebuffer(ps, "screen after hold (should be exit prompt)");

    /* Release, then press Down to select Exit (Continue is the default first option), then
       confirm with Fire. */
    printf("selecting Exit...\n");
    psemu_set_buttons(ps, 0);
    psemu_run(ps, FRAME_CYCLES);
    psemu_set_buttons(ps, PSEMU_BUTTON_DOWN);
    psemu_run(ps, FRAME_CYCLES);
    psemu_set_buttons(ps, 0);
    psemu_run(ps, FRAME_CYCLES);
    psemu_set_buttons(ps, PSEMU_BUTTON_FIRE);
    psemu_run(ps, FRAME_CYCLES);
    psemu_set_buttons(ps, 0);

    /* Run for settle frames, watching for the PC to return to BIOS space. */
    printf("settling (%d frames)...\n", SETTLE_FRAMES);
    pc_in_bios = 0;
    for (f = 0; f < SETTLE_FRAMES; f++) {
        psemu_run(ps, FRAME_CYCLES);
        if ((ps->cpu.r[15] >> 24) == 0x04u) {
            printf("  PC entered BIOS space at frame %ld (pc=0x%08X)\n", f, ps->cpu.r[15]);
            pc_in_bios = 1;
            break;
        }
    }
    if (!pc_in_bios)
        printf("  PC never reached BIOS space (pc=0x%08X) — exit may not have completed\n", ps->cpu.r[15]);
    if (psemu_cpu_faulted(ps))
        printf("  CPU faulted (pc=0x%08X)\n", ps->cpu.r[15]);

    print_framebuffer(ps, "screen after exit");

    /* Flash write summary. */
    printf("\nflash writes attempted during hold+exit sequence:\n");
    printf("  FLASH1 (app code region): %lu\n", fw.flash1_writes);
    printf("  FLASH2 (full card):       %lu\n", fw.flash2_writes);
    printf("  FLASH_CTRL (bank select): %lu\n", fw.ctrl_writes);
    if (fw.flash1_writes + fw.flash2_writes + fw.ctrl_writes == 0)
        printf("  none — app did not attempt any flash write during this sequence\n");

    /* Per-block diff against the flash snapshot taken at launch. */
    {
        const uint8_t *now = ps->flash.data;
        uint32_t block;
        int any = 0;
        printf("\ncard blocks changed since app launch: ");
        for (block = 0; block < PSEMU_FLASH_SIZE / 0x2000u; block++) {
            size_t base = (size_t)block * 0x2000u;
            size_t i;
            uint32_t changed = 0;
            for (i = 0; i < 0x2000u; i++) {
                if (now[base + i] != flash_at_launch[base + i])
                    changed++;
            }
            if (changed) {
                printf("%s%u (%u bytes)", any ? ", " : "", block, changed);
                any = 1;
            }
        }
        printf("%s\n", any ? "" : "none");
    }

    free(flash_at_launch);
    psemu_destroy(ps);
    free(bios);
    free(app);
    return 0;
}
