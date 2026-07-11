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
/* Polled by the real BIOS at boot (empirically observed: LDR/TST #0x10/BEQ
   loop before flash-control init) - address and exact semantics are not in
   any sourced documentation, only reverse-engineered from that boot loop.
   Modeled here as "always ready" (bit 4 set) purely to get past the poll. */
#define PSEMU_HW_READY_BASE 0x0B000000u
#define PSEMU_HW_READY_VALUE 0x00000010u
/* Confirmed against real hardware: the real-time clock (see rtc.h). */
#define PSEMU_RTC_BASE 0x0B800000u
#define PSEMU_IR_BASE 0x0C800000u
#define PSEMU_LCD_VRAM_BASE 0x0D000100u

struct lcd;
struct intc;
struct flash;
struct ir;
struct timer;
struct rtc;

typedef struct psemu_bus {
    uint8_t ram[PSEMU_RAM_SIZE];
    uint8_t bios[PSEMU_BIOS_SIZE];
    struct lcd *lcd;
    struct intc *intc;
    struct flash *flash;
    struct ir *ir;
    struct timer *timer;
    struct rtc *rtc;
} psemu_bus_t;

void psemu_bus_init(
    psemu_bus_t *bus, struct lcd *lcd, struct intc *intc, struct flash *flash, struct ir *ir, struct timer *timer,
    struct rtc *rtc);

uint8_t psemu_bus_read8(psemu_bus_t *bus, uint32_t addr);
uint16_t psemu_bus_read16(psemu_bus_t *bus, uint32_t addr);
uint32_t psemu_bus_read32(psemu_bus_t *bus, uint32_t addr);
void psemu_bus_write8(psemu_bus_t *bus, uint32_t addr, uint8_t value);
void psemu_bus_write16(psemu_bus_t *bus, uint32_t addr, uint16_t value);
void psemu_bus_write32(psemu_bus_t *bus, uint32_t addr, uint32_t value);

#endif
