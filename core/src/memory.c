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

/* TEMPORARY diagnostic flag - see intc.c's psemu_intc_trace_enabled for the
   same pattern. tools/inspect.c flips this on to log every real CLK_MODE
   and DAC_CTRL write with its real PC - used to confirm the real BIOS
   raises CLK_MODE during audio playback (see clk.h/docs/hardware-notes.md).
   Remove once the audio/animation-speed investigation is resolved. */
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
}

uint8_t psemu_bus_read8(psemu_bus_t *bus, uint32_t addr) {
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
    if (addr >= PSEMU_IR_BASE && addr < PSEMU_IR_BASE + 8u) {
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

void psemu_bus_write8(psemu_bus_t *bus, uint32_t addr, uint8_t value) {
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
    if (addr >= PSEMU_IR_BASE && addr < PSEMU_IR_BASE + 8u) {
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

uint16_t psemu_bus_read16(psemu_bus_t *bus, uint32_t addr) {
    return (uint16_t)(psemu_bus_read8(bus, addr) | ((uint16_t)psemu_bus_read8(bus, addr + 1) << 8));
}

uint32_t psemu_bus_read32(psemu_bus_t *bus, uint32_t addr) {
    return (uint32_t)psemu_bus_read8(bus, addr) | ((uint32_t)psemu_bus_read8(bus, addr + 1) << 8) |
           ((uint32_t)psemu_bus_read8(bus, addr + 2) << 16) | ((uint32_t)psemu_bus_read8(bus, addr + 3) << 24);
}

void psemu_bus_write16(psemu_bus_t *bus, uint32_t addr, uint16_t value) {
    psemu_bus_write8(bus, addr, (uint8_t)value);
    psemu_bus_write8(bus, addr + 1, (uint8_t)(value >> 8));
}

void psemu_bus_write32(psemu_bus_t *bus, uint32_t addr, uint32_t value) {
    psemu_bus_write8(bus, addr, (uint8_t)value);
    psemu_bus_write8(bus, addr + 1, (uint8_t)(value >> 8));
    psemu_bus_write8(bus, addr + 2, (uint8_t)(value >> 16));
    psemu_bus_write8(bus, addr + 3, (uint8_t)(value >> 24));
}
