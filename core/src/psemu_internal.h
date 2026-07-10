#ifndef PSEMU_INTERNAL_H
#define PSEMU_INTERNAL_H

#include "cpu.h"
#include "flash.h"
#include "io.h"
#include "ir.h"
#include "lcd.h"
#include "memory.h"
#include "psemu/psemu.h"

struct psemu {
    arm7tdmi_t cpu;
    psemu_bus_t bus;
    lcd_t lcd;
    io_t io;
    flash_t flash;
    ir_t ir;
    int has_bios;
};

#endif
