#include "memory.h"

#include <stdio.h>
#include <string.h>

#include "clk.h"
#include "cpu.h"
#include "dac.h"
#include "flash.h"
#include "intc.h"
#include "iop.h"
#include "ir.h"
#include "lcd.h"
#include "rtc.h"
#include "timer.h"

/* Diagnostic flag - see intc.c's psemu_intc_trace_enabled for the same
   pattern. tools/inspect.c's `clktrace` flag turns this on to log every
   real CLK_MODE and DAC_CTRL write with its real PC (see docs/hardware-
   notes.md, "CLK_MODE"). Kept as permanent diagnostic infrastructure. */
int psemu_clk_trace_enabled = 0;

void psemu_bus_init(
    psemu_bus_t *bus, struct lcd *lcd, struct intc *intc, struct flash *flash, struct ir *ir, struct timer *timer,
    struct rtc *rtc, struct dac *dac, struct clk *clk, struct iop *iop) {
    memset(bus->ram, 0, sizeof(bus->ram));
    memset(bus->bios, 0, sizeof(bus->bios));
    bus->lcd = lcd;
    bus->intc = intc;
    bus->flash = flash;
    bus->ir = ir;
    bus->timer = timer;
    bus->rtc = rtc;
    bus->dac = dac;
    bus->clk = clk;
    bus->iop = iop;
    bus->pending_cycles = 0;
}

/* Real per-region data-access wait-state cost (the documented "Memory
   Access Time for Data Read/Write" table - see docs/hardware-notes.md,
   "Memory access timing"). Confirmed: cost doesn't depend on 8/16/32-bit
   width or sequential/non-sequential access, so this is added exactly once
   per logical psemu_bus_read/write* call below, never per constituent
   byte. */
static uint32_t psemu_region_data_cycles(uint32_t addr) {
    if (addr < PSEMU_RAM_SIZE) {
        return 1u; /* WRAM */
    }
    /* FLASH_CTRL (the F_xxx bank-select/F_WAIT/F_SN registers) gets WRAM's
       fast 1-cycle rate - confirmed on real retail hardware via this
       project's own homebrew timing-benchmark app: a tight loop reading
       FLASH_CTRL+0x100 (F_BANK_VAL[0]) 30000 times came back with the exact
       same elapsed-tick count as an identical loop reading plain WRAM, on
       real silicon. The documented table only says "WRAM (and SOME F_xxx
       ports)" get the fast rate without naming which ports; this had
       previously been guessed the OTHER way (slow, matching FLASH/BIOS)
       based on disassembling an independent third-party emulator's own
       source - real hardware now overrides that guess, per this project's
       own trust order (real hardware over independent-emulator
       disassembly). See docs/hardware-notes.md's "Memory access timing"
       section and docs/app-notes.md for the full real-hardware result. */
    if (addr >= PSEMU_FLASH_CTRL_BASE && addr < PSEMU_FLASH_CTRL_BASE + FLASH_CTRL_SPAN) {
        return 1u;
    }
    /* VIRT/PHYS/XTRA_FLASH, BIOS, VRAM, I/O = 2 cycles. */
    return 2u;
}

uint32_t psemu_region_fetch_cycles(uint32_t addr, int thumb) {
    if (addr < PSEMU_RAM_SIZE) {
        return 1u; /* WRAM: 1 cycle, ARM or Thumb alike */
    }
    if ((addr >= PSEMU_FLASH1_BASE && addr < PSEMU_FLASH1_BASE + PSEMU_FLASH_SIZE) ||
        (addr >= PSEMU_FLASH2_BASE && addr < PSEMU_FLASH2_BASE + PSEMU_FLASH_SIZE)) {
        return thumb ? 1u : 2u; /* FLASH: 2 cycles ARM, 1 cycle Thumb */
    }
    if (addr >= PSEMU_BIOS_BASE && addr < PSEMU_BIOS_BASE + PSEMU_BIOS_SIZE) {
        /* The real kernel executes directly out of BIOS
           ROM constantly, so this can't be left uncosted. Equal to
           FLASH's rate INCLUDING its ARM/Thumb split (2 cycles ARM, 1 cycle
           Thumb), not a flat 2 for both: external reference data-access table
           groups BIOS with VIRT/PHYS/XTRA_FLASH as one slow-rate entry, and
           its bus-width section separately says "FLASH and BIOS ROM seem to
           be allowed to be read only in 16bit and 32bit units... RAM can be
           freely read/written in 8bit, 16bit, and 32bit units" - both
           mentions pair BIOS with FLASH's bus behavior, never WRAM's, so
           FLASH's full rate makes sense. Now also backed by real retail 
           hardware, not just documentation inference: this project's own 
           pk_timing_bench homebrew timed a real BIOS ARM helper against a 
           WRAM-copied version (~1.2x slower, consistent with BIOS paying FLASH's
           2-cycle ARM rate against WRAM's 1-cycle rate) and a real BIOS
           Thumb helper the same way (~1.02x, consistent with BIOS's Thumb
           rate matching WRAM's 1-cycle rate, same as FLASH's own Thumb
           rate) - both point the same direction as the guess above. Not as
           strong as a direct BIOS-disassembly trace, since loop/timer
           overhead dilutes the pure fetch-cost signal, but real,
           independent, hardware-level support. See "Memory access timing"
           in docs/hardware-notes.md and pk_timing_bench/README.md. */
        return thumb ? 1u : 2u;
    }
    /* Everything else (VRAM, I/O) = 2 cycles. */
    return 2u;
}

/* Raw, uncosted 8-bit access - the actual region-routing logic. Only called
   from within this file, by the costed psemu_bus_* functions below (which
   add the region's wait-state cost exactly once per logical call) - never
   call this directly, or accesses go uncounted. */
static uint8_t bus_read8_raw(psemu_bus_t *bus, uint32_t addr) {
    if (addr < PSEMU_RAM_SIZE) {
        return bus->ram[addr];
    }
    if (addr >= PSEMU_BIOS_BASE && addr < PSEMU_BIOS_BASE + PSEMU_BIOS_SIZE) {
        return bus->bios[addr - PSEMU_BIOS_BASE];
    }
    /* FLASH1 is a banked window onto FLASH2, offset by the block selected
       via FLASH_CTRL (see docs/hardware-notes.md). */
    if (addr >= PSEMU_FLASH1_BASE && addr < PSEMU_FLASH1_BASE + PSEMU_FLASH_SIZE) {
        return flash1_read8(bus->flash, addr - PSEMU_FLASH1_BASE);
    }
    if (addr >= PSEMU_FLASH2_BASE && addr < PSEMU_FLASH2_BASE + PSEMU_FLASH_SIZE) {
        return flash_read8(bus->flash, addr - PSEMU_FLASH2_BASE);
    }
    if (addr >= PSEMU_FLASH_CTRL_BASE && addr < PSEMU_FLASH_CTRL_BASE + FLASH_CTRL_SPAN) {
        return flash_ctrl_read8(bus->flash, addr - PSEMU_FLASH_CTRL_BASE);
    }
    if (addr >= PSEMU_LCD_VRAM_BASE && addr < PSEMU_LCD_VRAM_BASE + LCD_VRAM_SIZE) {
        return lcd_read8(bus->lcd, addr - PSEMU_LCD_VRAM_BASE);
    }
    if (addr >= PSEMU_LCD_MODE_BASE && addr < PSEMU_LCD_MODE_BASE + LCD_MODE_REG_SPAN) {
        return lcd_mode_read8(bus->lcd, addr - PSEMU_LCD_MODE_BASE);
    }
    if (addr >= PSEMU_CLK_BASE && addr < PSEMU_CLK_BASE + CLK_REG_SPAN) {
        return clk_read8(bus->clk, addr - PSEMU_CLK_BASE);
    }
    if (addr >= PSEMU_RTC_BASE && addr < PSEMU_RTC_BASE + RTC_REG_SPAN) {
        return rtc_read8(bus->rtc, addr - PSEMU_RTC_BASE);
    }
    if (addr >= PSEMU_INTC_BASE && addr < PSEMU_INTC_BASE + INTC_REG_SPAN) {
        return intc_read8(bus->intc, addr - PSEMU_INTC_BASE);
    }
    if (addr >= PSEMU_IR_BASE && addr < PSEMU_IR_BASE + IR_REG_SPAN) {
        return (uint8_t)ir_read(bus->ir, addr - PSEMU_IR_BASE);
    }
    if (addr >= PSEMU_TIMER_BASE && addr < PSEMU_TIMER_BASE + TIMER_REG_SPAN) {
        return timer_read8(bus->timer, addr - PSEMU_TIMER_BASE);
    }
    if (addr >= PSEMU_DAC_BASE && addr < PSEMU_DAC_BASE + DAC_REG_SPAN) {
        return dac_read8(bus->dac, addr - PSEMU_DAC_BASE);
    }
    if (addr >= PSEMU_IOP_BASE && addr < PSEMU_IOP_BASE + IOP_REG_SPAN) {
        return iop_read8(bus->iop, addr - PSEMU_IOP_BASE);
    }
    return 0;
}

/* Raw, uncosted 8-bit write - see bus_read8_raw. */
static void bus_write8_raw(psemu_bus_t *bus, uint32_t addr, uint8_t value) {
    if (addr < PSEMU_RAM_SIZE) {
        bus->ram[addr] = value;
        return;
    }
    if (addr >= PSEMU_FLASH1_BASE && addr < PSEMU_FLASH1_BASE + PSEMU_FLASH_SIZE) {
        flash1_write8(bus->flash, addr - PSEMU_FLASH1_BASE, value);
        return;
    }
    if (addr >= PSEMU_FLASH2_BASE && addr < PSEMU_FLASH2_BASE + PSEMU_FLASH_SIZE) {
        flash_write8(bus->flash, addr - PSEMU_FLASH2_BASE, value);
        return;
    }
    if (addr >= PSEMU_FLASH_CTRL_BASE && addr < PSEMU_FLASH_CTRL_BASE + FLASH_CTRL_SPAN) {
        flash_ctrl_write8(bus->flash, addr - PSEMU_FLASH_CTRL_BASE, value);
        return;
    }
    if (addr >= PSEMU_LCD_VRAM_BASE && addr < PSEMU_LCD_VRAM_BASE + LCD_VRAM_SIZE) {
        lcd_write8(bus->lcd, addr - PSEMU_LCD_VRAM_BASE, value);
        return;
    }
    if (addr >= PSEMU_LCD_MODE_BASE && addr < PSEMU_LCD_MODE_BASE + LCD_MODE_REG_SPAN) {
        lcd_mode_write8(bus->lcd, addr - PSEMU_LCD_MODE_BASE, value);
        return;
    }
    if (addr >= PSEMU_CLK_BASE && addr < PSEMU_CLK_BASE + CLK_REG_SPAN) {
        if (psemu_clk_trace_enabled) {
            printf(
                "[clk trace] pc=0x%08X WRITE CLK_MODE (+0x%X) = 0x%02X\n", psemu_debug_current_pc,
                (unsigned)(addr - PSEMU_CLK_BASE), (unsigned)value);
        }
        clk_write8(bus->clk, addr - PSEMU_CLK_BASE, value);
        return;
    }
    if (addr >= PSEMU_RTC_BASE && addr < PSEMU_RTC_BASE + RTC_REG_SPAN) {
        rtc_write8(bus->rtc, addr - PSEMU_RTC_BASE, value);
        return;
    }
    if (addr >= PSEMU_INTC_BASE && addr < PSEMU_INTC_BASE + INTC_REG_SPAN) {
        intc_write8(bus->intc, addr - PSEMU_INTC_BASE, value);
        return;
    }
    if (addr >= PSEMU_IR_BASE && addr < PSEMU_IR_BASE + IR_REG_SPAN) {
        ir_write(bus->ir, addr - PSEMU_IR_BASE, value);
        return;
    }
    if (addr >= PSEMU_TIMER_BASE && addr < PSEMU_TIMER_BASE + TIMER_REG_SPAN) {
        timer_write8(bus->timer, addr - PSEMU_TIMER_BASE, value);
        return;
    }
    if (addr >= PSEMU_DAC_BASE && addr < PSEMU_DAC_BASE + DAC_REG_SPAN) {
        if (psemu_clk_trace_enabled && addr == PSEMU_DAC_BASE) {
            printf(
                "[clk trace] pc=0x%08X WRITE DAC_CTRL = 0x%02X (enable=%d)\n", psemu_debug_current_pc,
                (unsigned)value, value & 1);
        }
        dac_write8(bus->dac, addr - PSEMU_DAC_BASE, value);
        return;
    }
    if (addr >= PSEMU_IOP_BASE && addr < PSEMU_IOP_BASE + IOP_REG_SPAN) {
        iop_write8(bus->iop, addr - PSEMU_IOP_BASE, value);
        /* Mirror the sound-enable gate into the DAC directly - both it
           and DAC_CTRL's own enable bit must be set for audio to play
           (confirmed against real hardware, see iop.h/dac.h). */
        dac_set_iop_muted(bus->dac, !iop_sound_enabled(bus->iop));
        return;
    }
}

/* Costed public accessors below - each adds its region's wait-state cost
   exactly once (see psemu_region_data_cycles/psemu_region_fetch_cycles),
   regardless of access width, then defers to the raw/uncosted routing
   logic above. Fetch variants are used only by arm7tdmi_step's opcode
   fetch; every instruction-handler data access (arm_exec.c, thumb_exec.c)
   goes through the data-access variants instead. */

uint8_t psemu_bus_read8(psemu_bus_t *bus, uint32_t addr) {
    bus->pending_cycles += psemu_region_data_cycles(addr);
    return bus_read8_raw(bus, addr);
}

void psemu_bus_write8(psemu_bus_t *bus, uint32_t addr, uint8_t value) {
    bus->pending_cycles += psemu_region_data_cycles(addr);
    bus_write8_raw(bus, addr, value);
}

uint16_t psemu_bus_read16(psemu_bus_t *bus, uint32_t addr) {
    bus->pending_cycles += psemu_region_data_cycles(addr);
    return (uint16_t)(bus_read8_raw(bus, addr) | ((uint16_t)bus_read8_raw(bus, addr + 1) << 8));
}

uint32_t psemu_bus_read32(psemu_bus_t *bus, uint32_t addr) {
    bus->pending_cycles += psemu_region_data_cycles(addr);
    return (uint32_t)bus_read8_raw(bus, addr) | ((uint32_t)bus_read8_raw(bus, addr + 1) << 8) |
           ((uint32_t)bus_read8_raw(bus, addr + 2) << 16) | ((uint32_t)bus_read8_raw(bus, addr + 3) << 24);
}

void psemu_bus_write16(psemu_bus_t *bus, uint32_t addr, uint16_t value) {
    bus->pending_cycles += psemu_region_data_cycles(addr);
    bus_write8_raw(bus, addr, (uint8_t)value);
    bus_write8_raw(bus, addr + 1, (uint8_t)(value >> 8));
}

void psemu_bus_write32(psemu_bus_t *bus, uint32_t addr, uint32_t value) {
    bus->pending_cycles += psemu_region_data_cycles(addr);
    bus_write8_raw(bus, addr, (uint8_t)value);
    bus_write8_raw(bus, addr + 1, (uint8_t)(value >> 8));
    bus_write8_raw(bus, addr + 2, (uint8_t)(value >> 16));
    bus_write8_raw(bus, addr + 3, (uint8_t)(value >> 24));
}

uint16_t psemu_bus_fetch16(psemu_bus_t *bus, uint32_t addr) {
    bus->pending_cycles += psemu_region_fetch_cycles(addr, 1);
    return (uint16_t)(bus_read8_raw(bus, addr) | ((uint16_t)bus_read8_raw(bus, addr + 1) << 8));
}

uint32_t psemu_bus_fetch32(psemu_bus_t *bus, uint32_t addr) {
    bus->pending_cycles += psemu_region_fetch_cycles(addr, 0);
    return (uint32_t)bus_read8_raw(bus, addr) | ((uint32_t)bus_read8_raw(bus, addr + 1) << 8) |
           ((uint32_t)bus_read8_raw(bus, addr + 2) << 16) | ((uint32_t)bus_read8_raw(bus, addr + 3) << 24);
}
