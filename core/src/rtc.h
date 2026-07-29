#ifndef PSEMU_RTC_H
#define PSEMU_RTC_H

#include <stdint.h>

struct intc;

#define RTC_REG_SPAN 16u

/* Sony PocketStation RTC.
   Register behavior confirmed against the documented register table; see docs/hardware-notes.md for the full comparison.

   This emulator deliberately avoids two known pitfalls:
   - Hardcoding an arbitrary 1999-01-01 for time/date. The real BIOS resets itself to Jan 1999 as a well-known
     quirk, but that reset is a software action, not the real power-on-reset value.
   - Ticking the RTC timer at a flat 1Hz while "paused". The real interrupt runs at approximately 4096Hz while
     paused. This lets a "set the clock" UI step through an adjustment without waiting a full second per step.

   The RTC has four 32-bit registers at 0x0B800000:
   - mode (+0x0)
   - control (+0x4)
   - time (+0x8, read-only from software's view)
   - date (+0xC, read-only)

   time and date each pack BCD bytes, from LSB to MSB:
   - time: seconds, minutes, hours, day-of-week (1=Sun..7=Sat)
   - date: day, month, year (2-digit BCD)

   date's top byte is not a "year-hi"/century field.
   The documentation describes it as "Unknown? (this is NOT used as century)".
   The real century value lives in battery-backed kernel RAM.
   Only the GetBcdDate SWI reads the century value back; this register does not expose it.

   mode bit 0 (PRGSEL) selects the RTC's mode:
   - Run (0): ticks at 1Hz, auto-advances the clock every tick.
   - Program/pause (1): ticks at approximately 4096Hz, does not auto-advance.
   Program mode exists only so RTC_ADJUST writes can step one field at a time, without the clock moving underneath them.

   mode bits 1-3 (CNTSEL) select which BCD field a control-register write adjusts:
   0=sec, 1=min, 2=hour, 3=dow, 4=day, 5=month, 6=year, 7=none.

   Writing 1 to control while it already holds 1 increments the CNTSEL-selected field and resets control to 0.
   Writing to control while it holds 0 just stores the written value.
   This is the real "write 1 twice" idiom for a single increment.
   This part is confirmed against real hardware's behavior and independent community write-ups; this emulator
   implements it as-is.

   The automatic per-tick advance cascades seconds -> minutes -> hours -> day-of-week only.
   It uses the exact same BCD carry arithmetic as the CNTSEL=0 (seconds) case.
   This cascade is this emulator's own RTC auto-advance logic.
   It does not cascade into date on a day rollover.
   This is a gap in this codebase's own history; no independent source explains it.
   No independent source describes the real date-rollover mechanism either, so this gap is inherited, not invented. */
typedef struct rtc {
    uint32_t mode;
    uint32_t control;
    uint32_t time;
    uint32_t date;
    uint32_t tick_accumulator;
    int int_line;
} rtc_t;

/* Approximate cycle count between interrupt-line toggles while running (mode bit0 clear).
   This value is not calibrated against a real 1Hz reference.
   It is only fast enough that a wait-for-pulse loop resolves within a reasonable instruction budget.
   The paused rate is exactly 4096x faster than the running rate, matching the confirmed 1Hz-vs-4096Hz ratio. */
#define RTC_TICK_CYCLES_RUN 4000000u
#define RTC_TICK_CYCLES_PAUSED (RTC_TICK_CYCLES_RUN / 4096u)

void rtc_init(rtc_t *rtc);
uint8_t rtc_read8(rtc_t *rtc, uint32_t offset);
void rtc_write8(rtc_t *rtc, uint32_t offset, uint8_t value);

/* Advances by `cycles`.
   The real RTC's interrupt line toggles at approximately 1Hz while running, or approximately 4096Hz while
   paused (mode bit0). This emulator asserts that line as INT_RTC through `intc`.
   Real BIOS code waits for a full pulse (rising then falling), so a constant "always ready" value is not sufficient.
   While running, each toggle also auto-advances the clock (see rtc.h's top comment). */
void rtc_tick(rtc_t *rtc, struct intc *intc, uint32_t cycles);

#endif
