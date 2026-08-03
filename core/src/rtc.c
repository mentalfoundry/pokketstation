#include "rtc.h"

#include "intc.h"

void rtc_init(rtc_t *rtc) {
    rtc->mode = 0;
    rtc->control = 0;
    /* Real hardware power-on-reset values (RTCClock/RTCCalendar reset columns): day-of-week BCD 4, date 1998-01-01.
       An arbitrary 1999-01-01 would match the BIOS's well-known "resets itself to Jan 1999" software quirk.
       That quirk is a software action, not the real hardware POR value.
       This emulator uses the real POR values instead. */
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

/* Increments the BCD field selected by mode>>1.
   The wraparound arithmetic is confirmed against real hardware's register-write behavior.
   Case 7 (year-hi) is a documented no-op. */
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

    /* control: only byte 0 carries meaning in observed real usage.
       Writing 1 while control already holds 1 triggers the increment and resets control to 0.
       Writing while control holds 0 just stores the value.
       Real code deliberately writes 1 twice for a single increment; see docs/hardware-notes.md. */
    if (shift == 0u) {
        if (rtc->control == 1u && value == 1u) {
            rtc_increment_field(rtc);
            rtc->control = 0;
        } else if (rtc->control == 0u) {
            rtc->control = value;
        }
    }
}

/* Unconditional seconds -> minutes -> hours -> day-of-week cascade.
   This is this emulator's own RTC auto-advance logic (the auto-advance side, not the control-register-triggered side).
   It deliberately does not cascade into date on a day rollover.
   This is a gap in this codebase's own history; no independent source explains it.
   No independent source documents the real date-rollover mechanism either, so this gap is inherited, not invented. */
static uint32_t bcd_to_bin(uint32_t bcd) {
    return (bcd >> 4) * 10u + (bcd & 0x0Fu);
}

static uint32_t bin_to_bcd(uint32_t bin) {
    return ((bin / 10u) << 4) | (bin % 10u);
}

/* Length of `month` (1-12) in the RTC's own two-digit `year`.

   The leap rule can only be year % 4. RTC_DATE has no century field at all - the century lives in
   battery-backed kernel RAM and only GetBcdDate exposes it (see rtc.h) - so the hardware cannot apply the
   100/400-year exceptions even in principle. Year 00 therefore counts as a leap year, which is right for
   2000 and wrong for 1900. */
static uint32_t rtc_days_in_month(uint32_t month, uint32_t year) {
    static const uint8_t lengths[12] = {31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u};
    if (month < 1u || month > 12u) {
        return 31u; /* a corrupt month field must not index out of bounds */
    }
    if (month == 2u && (year % 4u) == 0u) {
        return 29u;
    }
    return lengths[month - 1u];
}

/* Advances RTC_DATE by one day, cascading into month and year.

   CONFIRMED on real hardware: the date does roll over at midnight. This emulator did not do it at all - the
   per-second cascade stopped at day-of-week - so an emulated device sat on the same date forever while its
   day-of-week advanced past it.

   NOT confirmed: the month lengths and the leap rule above. Real hardware is known to keep poor time (the
   BIOS ships a workaround Sony calls "The RTC Problem", see rtc.h), so a cheaper design that rolls every
   month at 31 is possible. Testing that needs a unit parked at 23:59:5x on a short month; see
   pk_timing_bench's README. Proper lengths are used here because a frontend can impose the host date
   through psemu_set_datetime, and a free-running clock that produced "31 February" would be visibly wrong
   in a way this is not.

   Byte 3 is left alone: it is documented as unused and unidentified, not a century field. */
static void rtc_advance_date(rtc_t *rtc) {
    uint32_t day = bcd_to_bin(rtc->date & 0xFFu);
    uint32_t month = bcd_to_bin((rtc->date >> 8) & 0xFFu);
    uint32_t year = bcd_to_bin((rtc->date >> 16) & 0xFFu);

    day++;
    if (day > rtc_days_in_month(month, year)) {
        day = 1u;
        month++;
        if (month > 12u) {
            month = 1u;
            year = (year + 1u) % 100u;
        }
    }
    rtc->date = (rtc->date & 0xFF000000u) | (bin_to_bcd(year) << 16) | (bin_to_bcd(month) << 8)
        | bin_to_bcd(day);
}

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
        /* Midnight: the calendar moves too. Confirmed on real hardware. */
        rtc_advance_date(rtc);
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
