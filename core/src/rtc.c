#include "rtc.h"

#include "intc.h"

void rtc_init(rtc_t *rtc) {
    rtc->mode = 0;
    rtc->control = 0;
    /* Real silicon power-on-reset values (RTCClock/RTCCalendar reset
       columns) - day-of-week BCD 4, date 1998-01-01. An arbitrary
       1999-01-01 would match the BIOS's well-known "resets itself to
       Jan 1999" software quirk, but that's a software action rather
       than the actual hardware POR value - not used here since these
       are the real POR values. */
    rtc->time = 0x04000000u;
    rtc->date = 0x00980101u;
    rtc->tick_accumulator = 0;
    rtc->int_line = 0;
}

static uint32_t *reg_ptr(rtc_t *rtc, uint32_t reg_index) {
    switch (reg_index % 4u) {
    case 0:
        return &rtc->mode;
    case 1:
        return &rtc->control;
    case 2:
        return &rtc->time;
    default:
        return &rtc->date;
    }
}

uint8_t rtc_read8(rtc_t *rtc, uint32_t offset) {
    uint32_t *reg = reg_ptr(rtc, offset / 4u);
    return (uint8_t)(*reg >> ((offset % 4u) * 8u));
}

/* Increments the BCD field selected by mode>>1, with the exact wraparound
   arithmetic confirmed against real hardware's register-write behavior
   (case 7, year-hi, is a documented no-op). */
static void rtc_increment_field(rtc_t *rtc) {
    switch (rtc->mode >> 1) {
    case 0: /* seconds */
        rtc->time += 0x00000001u;
        if ((rtc->time & 0x0000000Fu) == 0x0000000Au) {
            rtc->time &= 0xFFFFFFF0u;
            rtc->time += 0x00000010u;
            if ((rtc->time & 0x000000FFu) == 0x00000060u) {
                rtc->time &= 0xFFFFFF00u;
            }
        }
        break;
    case 1: /* minutes */
        rtc->time += 0x00000100u;
        if ((rtc->time & 0x00000F00u) == 0x00000A00u) {
            rtc->time &= 0xFFFFF0FFu;
            rtc->time += 0x00001000u;
            if ((rtc->time & 0x0000FF00u) == 0x00006000u) {
                rtc->time &= 0xFFFF00FFu;
            }
        }
        break;
    case 2: /* hours */
        rtc->time += 0x00010000u;
        if ((rtc->time & 0x00FF0000u) == 0x00240000u) {
            rtc->time &= 0xFF00FFFFu;
        } else if ((rtc->time & 0x000F0000u) == 0x000A0000u) {
            rtc->time &= 0xFFF0FFFFu;
            rtc->time += 0x00100000u;
        }
        break;
    case 3: /* day of week */
        rtc->time += 0x01000000u;
        if ((rtc->time & 0x0F000000u) == 0x08000000u) {
            rtc->time &= 0xF0FFFFFFu;
            rtc->time |= 0x01000000u;
        }
        break;
    case 4: /* day */
        rtc->date += 0x00000001u;
        if ((rtc->date & 0x000000FFu) == 0x00000032u) {
            rtc->date &= 0xFFFFFF00u;
        } else if ((rtc->date & 0x0000000Fu) == 0x0000000Au) {
            rtc->date &= 0xFFFFFFF0u;
            rtc->date += 0x00000010u;
        }
        break;
    case 5: /* month */
        rtc->date += 0x00000100u;
        if ((rtc->date & 0x0000FF00u) == 0x00001300u) {
            rtc->date &= 0xFFFFFF00u;
            rtc->date |= 0x00000001u;
        } else if ((rtc->date & 0x00000F00u) == 0x00000A00u) {
            rtc->date &= 0xFFFFF0FFu;
            rtc->date += 0x00001000u;
        }
        break;
    case 6: /* year, low BCD byte */
        rtc->date += 0x00010000u;
        if ((rtc->date & 0x000F0000u) == 0x000A0000u) {
            rtc->date &= 0xFFF0FFFFu;
            rtc->date += 0x00100000u;
            if ((rtc->date & 0x00F00000u) == 0x00A00000u) {
                rtc->date &= 0xFF00FFFFu;
            }
        }
        break;
    default: /* year, high BCD byte: documented no-op */
        break;
    }
}

void rtc_write8(rtc_t *rtc, uint32_t offset, uint8_t value) {
    uint32_t reg_index = (offset / 4u) % 4u;
    uint32_t shift = (offset % 4u) * 8u;

    if (reg_index != 1u) { /* mode, time, date: plain store */
        uint32_t *reg = reg_ptr(rtc, reg_index);
        *reg = (*reg & ~(0xFFu << shift)) | ((uint32_t)value << shift);
        return;
    }

    /* control: only byte 0 carries meaning in observed real usage. Writing
       1 while already 1 triggers the increment and resets to 0; writing
       while 0 just stores (real code deliberately writes 1 twice for a
       single increment - see docs/hardware-notes.md). */
    if (shift == 0u) {
        if (rtc->control == 1u && value == 1u) {
            rtc_increment_field(rtc);
            rtc->control = 0;
        } else if (rtc->control == 0u) {
            rtc->control = value;
        }
    }
}

/* Unconditional seconds -> minutes -> hours -> day-of-week cascade - this
   codebase's own RTC auto-advance logic (the auto-advance side, not the
   control-register-triggered side). Deliberately does NOT cascade into
   date on a day rollover - a gap in this codebase's own history that no
   independent source explains; no independent source documents the real
   date-rollover mechanism either, so this gap is inherited rather than
   invented. */
static void rtc_advance_second(rtc_t *rtc) {
    rtc->time += 0x00000001u;
    if ((rtc->time & 0x0000000Fu) != 0x0000000Au) {
        return;
    }
    rtc->time &= 0xFFFFFFF0u;
    rtc->time += 0x00000010u;
    if ((rtc->time & 0x000000FFu) != 0x00000060u) {
        return;
    }
    rtc->time &= 0xFFFFFF00u;
    rtc->time += 0x00000100u;
    if ((rtc->time & 0x00000F00u) != 0x00000A00u) {
        return;
    }
    rtc->time &= 0xFFFFF0FFu;
    rtc->time += 0x00001000u;
    if ((rtc->time & 0x0000FF00u) != 0x00006000u) {
        return;
    }
    rtc->time &= 0xFFFF00FFu;
    rtc->time += 0x00010000u;
    if ((rtc->time & 0x00FF0000u) == 0x00240000u) {
        rtc->time &= 0xFF00FFFFu;
        rtc->time += 0x01000000u;
        if ((rtc->time & 0x0F000000u) == 0x08000000u) {
            rtc->time &= 0xF0FFFFFFu;
            rtc->time |= 0x01000000u;
        }
    } else if ((rtc->time & 0x000F0000u) == 0x000A0000u) {
        rtc->time &= 0xFFF0FFFFu;
        rtc->time += 0x00100000u;
    }
}

void rtc_tick(rtc_t *rtc, struct intc *intc, uint32_t cycles) {
    rtc->tick_accumulator += cycles;
    for (;;) {
        int paused = (rtc->mode & 1u) != 0;
        uint32_t threshold = paused ? RTC_TICK_CYCLES_PAUSED : RTC_TICK_CYCLES_RUN;
        if (rtc->tick_accumulator < threshold) {
            break;
        }
        rtc->tick_accumulator -= threshold;
        rtc->int_line = !rtc->int_line;
        intc_set_line(intc, INT_RTC, rtc->int_line);
        if (!paused) {
            rtc_advance_second(rtc);
        }
    }
}
