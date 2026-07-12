#ifndef PSEMU_MEMORY_H
#define PSEMU_MEMORY_H

#include <stdint.h>

#include "psemu/psemu.h"

#define PSEMU_RAM_BASE 0x00000000u
#define PSEMU_RAM_SIZE 0x800u
#define PSEMU_FLASH1_BASE 0x02000000u
#define PSEMU_BIOS_BASE 0x04000000u
#define PSEMU_FLASH_CTRL_BASE 0x06000000u
#define PSEMU_FLASH2_BASE 0x08000000u
/* Confirmed against real hardware: the interrupt controller (see
   intc.h) - hold(+0x0), status(+0x4, buttons + RTC), enable(+0x8),
   mask(+0xC), acknowledge(+0x10). */
#define PSEMU_INTC_BASE 0x0A000000u
/* Confirmed against real hardware: 3 independent timers (see timer.h). */
#define PSEMU_TIMER_BASE 0x0A800000u
/* Confirmed against real hardware: CLK_MODE, CPU/timer clock speed
   control (see clk.h). A real BIOS boot loop polls this (LDR/TST #0x10/BEQ)
   before touching flash control, waiting for the clock to stabilize after a
   speed change. */
#define PSEMU_CLK_BASE 0x0B000000u
/* Confirmed against real hardware: the real-time clock (see rtc.h). */
#define PSEMU_RTC_BASE 0x0B800000u
#define PSEMU_IR_BASE 0x0C800000u
#define PSEMU_LCD_VRAM_BASE 0x0D000100u
/* Confirmed via the documentation: IOP
   power control (see iop.h) - IOP_CTRL(+0x0), IOP_STOP/IOP_STAT(+0x4),
   IOP_START(+0x8), IOP_DATA(+0xC). */
#define PSEMU_IOP_BASE 0x0D800000u
/* Confirmed via the documentation: the
   audio DAC (see dac.h). */
#define PSEMU_DAC_BASE 0x0D800010u

struct lcd;
struct intc;
struct flash;
struct ir;
struct timer;
struct rtc;
struct dac;
struct clk;
struct iop;

typedef struct psemu_bus {
    uint8_t ram[PSEMU_RAM_SIZE];
    uint8_t bios[PSEMU_BIOS_SIZE];
    struct lcd *lcd;
    struct intc *intc;
    struct flash *flash;
    struct ir *ir;
    struct timer *timer;
    struct rtc *rtc;
    struct dac *dac;
    struct clk *clk;
    struct iop *iop;
} psemu_bus_t;

void psemu_bus_init(
    psemu_bus_t *bus, struct lcd *lcd, struct intc *intc, struct flash *flash, struct ir *ir, struct timer *timer,
    struct rtc *rtc, struct dac *dac, struct clk *clk, struct iop *iop);

uint8_t psemu_bus_read8(psemu_bus_t *bus, uint32_t addr);
uint16_t psemu_bus_read16(psemu_bus_t *bus, uint32_t addr);
uint32_t psemu_bus_read32(psemu_bus_t *bus, uint32_t addr);
void psemu_bus_write8(psemu_bus_t *bus, uint32_t addr, uint8_t value);
void psemu_bus_write16(psemu_bus_t *bus, uint32_t addr, uint16_t value);
void psemu_bus_write32(psemu_bus_t *bus, uint32_t addr, uint32_t value);

/* TEMPORARY diagnostic flag - see memory.c. */
extern int psemu_clk_trace_enabled;

#endif
