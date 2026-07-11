#ifndef PSEMU_RTC_H
#define PSEMU_RTC_H

#include <stdint.h>

struct intc;

#define RTC_REG_SPAN 16u

/* Sony PocketStation RTC. Register behavior confirmed against the
   documented register table - see
   docs/hardware-notes.md for the full comparison, including two known
   pitfalls this port deliberately avoids: hardcoding an arbitrary
   1999-01-01 for time/date (matching the BIOS's well-known "resets
   itself to Jan 1999" quirk, but that's a software action, not the real
   power-on-reset value), and ticking the RTC timer at a flat 1Hz even
   while "paused", when the real interrupt runs at ~4096Hz while paused,
   presumably so a "set the clock" UI doesn't have to wait a full second
   per adjust step.

   Four 32-bit registers at 0x0B800000: mode(+0x0), control(+0x4),
   time(+0x8, read-only from software's view), date(+0xC, read-only).
   time/date are packed BCD bytes, LSB to MSB:
     time: seconds, minutes, hours, day-of-week (1=Sun..7=Sat)
     date: day, month, year (2-digit BCD) - the top byte is NOT a
       "year-hi"/century field (documented as
       "Unknown? (this is NOT used as century)") - the real century lives
       in battery-backed kernel RAM, read back only via the GetBcdDate
       SWI, not this register).

   mode bit0 (PRGSEL) selects Run (0, ticks at 1Hz, auto-advances the
   clock every tick) vs Program/pause (1, ticks at ~4096Hz, does NOT
   auto-advance - used only to let RTC_ADJUST writes step one field at a
   time without the clock moving underneath them). mode bits 1-3
   (CNTSEL) select which BCD field a control-register write adjusts:
   0=sec,1=min,2=hour,3=dow,4=day,5=month,6=year,7=none. Writing 1 to
   control while it already holds 1 increments the CNTSEL-selected field
   and resets control to 0; writing while control is 0 just stores the
   value (the real "write 1 twice" idiom for a single increment - this
   part is confirmed against real hardware's behavior and independent
   community write-ups, so it's implemented as-is). The automatic
   per-tick advance (seconds ->
   minutes -> hours -> day-of-week only) uses the exact same BCD carry
   arithmetic as the CNTSEL=0 (seconds) case - this codebase's own RTC
   auto-advance logic - note it does NOT cascade into date on a day
   rollover, a gap in this codebase's own history that no independent
   source explains; no independent source describes the real
   date-rollover mechanism either, so this is a known gap inherited
   rather than invented. */
typedef struct rtc {
    uint32_t mode;
    uint32_t control;
    uint32_t time;
    uint32_t date;
    uint32_t tick_accumulator;
    int int_line;
} rtc_t;

/* Approximate cycle count between interrupt-line toggles while running
   (mode bit0 clear) - not calibrated against a real 1Hz reference, just
   fast enough that a wait-for-pulse loop resolves within a reasonable
   instruction budget. The paused rate is exactly 4096x faster, matching
   the confirmed 1Hz-vs-4096Hz ratio. */
#define RTC_TICK_CYCLES_RUN 4000000u
#define RTC_TICK_CYCLES_PAUSED (RTC_TICK_CYCLES_RUN / 4096u)

void rtc_init(rtc_t *rtc);
uint8_t rtc_read8(rtc_t *rtc, uint32_t offset);
void rtc_write8(rtc_t *rtc, uint32_t offset, uint8_t value);

/* Advances by `cycles`; the real RTC's interrupt line toggles at ~1Hz
   while running or ~4096Hz while paused (mode bit0), asserted as INT_RTC
   through `intc` - real BIOS code waits for a full pulse (rising then
   falling), so a constant "always ready" value is not sufficient. While
   running, each toggle also auto-advances the clock (see rtc.h's top
   comment). */
void rtc_tick(rtc_t *rtc, struct intc *intc, uint32_t cycles);

#endif
