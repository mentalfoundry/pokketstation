#ifndef PSEMU_INTERNAL_H
#define PSEMU_INTERNAL_H

#include "clk.h"
#include "cpu.h"
#include "dac.h"
#include "flash.h"
#include "intc.h"
#include "ir.h"
#include "lcd.h"
#include "memory.h"
#include "psemu/psemu.h"
#include "rtc.h"
#include "timer.h"

struct psemu {
    arm7tdmi_t cpu;
    psemu_bus_t bus;
    lcd_t lcd;
    intc_t intc;
    flash_t flash;
    ir_t ir;
    timer_t timer;
    rtc_t rtc;
    dac_t dac;
    clk_t clk;
    uint32_t buttons; /* last-set PSEMU_BUTTON_* bitmask, for edge detection into the INTC */
    int has_bios;
};

#endif
