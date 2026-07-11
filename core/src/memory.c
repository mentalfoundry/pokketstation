#include "memory.h"

#include <string.h>

#include "flash.h"
#include "intc.h"
#include "ir.h"
#include "lcd.h"
#include "rtc.h"
#include "timer.h"

void psemu_bus_init(
    psemu_bus_t *bus, struct lcd *lcd, struct intc *intc, struct flash *flash, struct ir *ir, struct timer *timer,
    struct rtc *rtc) {
    memset(bus->ram, 0, sizeof(bus->ram));
    memset(bus->bios, 0, sizeof(bus->bios));
    bus->lcd = lcd;
    bus->intc = intc;
    bus->flash = flash;
    bus->ir = ir;
    bus->timer = timer;
    bus->rtc = rtc;
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
    if (addr >= PSEMU_HW_READY_BASE && addr < PSEMU_HW_READY_BASE + 4u) {
        return (uint8_t)(PSEMU_HW_READY_VALUE >> ((addr - PSEMU_HW_READY_BASE) * 8u));
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
