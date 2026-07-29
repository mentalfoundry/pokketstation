#ifndef PSEMU_DAC_H
#define PSEMU_DAC_H

#include <stdint.h>

/* Sony PocketStation DAC (audio out). Confirmed against real hardware; see docs/hardware-notes.md.

   The DAC has two 32-bit registers at 0x0D800010.
   ctrl is at +0x0. Bit 0 enables audio output.
   data is at +0x4. Bits 6-15 hold DACV, a signed 10-bit two's-complement output level.
   DACV's maximum positive value is 0x1FF. Its center value is 0. Its maximum negative value is 0x200.
   Bits 0-5 of data should stay zero.

   Audio also needs IOP_STOP/IOP_START bit 5 set (see iop.h).
   Both gates, DAC ctrl bit0 and IOP bit5, must be open before sound plays.

   Real hardware has no square-wave or noise generator, and no sound DMA channel.
   Software produces every tone by writing new DACV levels to DAC_DATA at audio rates.
   Timer1/IRQ-8 typically drives these writes.
   This makes the DAC a raw, CPU-bit-banged, one-sample-at-a-time device with no fixed sample rate of its own.

   dac_tick exposes this through a standard fixed-rate audio API.
   It resamples the currently-held output level into a ring buffer, at a fixed rate: PSEMU_DAC_SAMPLE_RATE_HZ.
   This resampling uses zero-order hold: the most recently written value stays current until the next write. */
#define DAC_REG_SPAN 0x10u

/* Real hardware runs at a variable clock, controlled via CLK_MODE (see clk.h).
   This clock reaches approximately 8MHz at mode 8 and above.

   PSEMU_ASSUMED_CPU_HZ is the fixed reference rate for psemu_run's cycle budget.
   This value matches frontends/desktop/main.c's real-time pacing: 33000 cycles per 32Hz frame (33000 * 32 = 1056000).

   dac_tick and rtc_tick always receive cycles already converted to this reference rate.
   This conversion happens regardless of the CPU's current CLK_MODE.
   See psemu_run's comment in psemu.c for the reason.

   Keep this value in sync with main.c's psemu_run() cycle count if that count ever changes.
   Otherwise audio pitch/tempo and on-screen timing will drift apart from each other.

   History: an earlier version of this file assumed approximately 4MHz instead, matching rtc.h's RTC_TICK_CYCLES.
   That was an unvalidated guess: it matched one uncalibrated constant to another, with no independent confirmation.
   Real-hardware testing showed that rate made on-screen animations visibly too fast.
   This value now follows the frontend's own empirically-confirmed pacing instead.

   A later session tried an ear-tuned fudge factor, 1077120u, after Timer started tracking CLK_MODE directly
   (see psemu_run's comment in psemu.c). That change aimed to fix an audio pitch reported "slightly too high".
   That fudge factor was explicitly a stopgap, with no hardware frequency reference behind it.
   Whether it masked a real, fixable bug elsewhere (for example an ARM cycle-timing inaccuracy in
   arm_exec.c/thumb_exec.c) is unconfirmed.
   This value was reverted to 33000u * 32u (1056000u) on request, instead of carrying an unconfirmed fudge forward.
   If the "slightly too high" pitch report resurfaces, look for the underlying cycle-timing bug before
   re-tuning this value by ear again. */
#define PSEMU_ASSUMED_CPU_HZ 1056000u
#define PSEMU_DAC_SAMPLE_RATE_HZ 8000u
#define DAC_CYCLES_PER_SAMPLE (PSEMU_ASSUMED_CPU_HZ / PSEMU_DAC_SAMPLE_RATE_HZ) /* = 132 */

#define DAC_SAMPLE_BUFFER_SIZE 8192u

typedef struct dac {
    uint32_t ctrl;
    uint32_t data;
    int16_t current_sample; /* current held output level, rescaled to full int16 range */
    uint32_t cycle_accumulator;
    int iop_muted; /* mirrors !iop_sound_enabled() - see iop.h */

    int16_t sample_buffer[DAC_SAMPLE_BUFFER_SIZE];
    uint32_t sample_write_pos;
    uint32_t sample_read_pos;
    uint32_t sample_count; /* samples currently buffered, <= DAC_SAMPLE_BUFFER_SIZE */
} dac_t;

void dac_init(dac_t *dac);
uint8_t dac_read8(dac_t *dac, uint32_t offset);
void dac_write8(dac_t *dac, uint32_t offset, uint8_t value);

/* Mirrors the IOP_STOP/IOP_START bit5 gate (see iop.h) into the DAC.
   dac_tick outputs non-silent samples only when both this gate and ctrl's enable bit are set. */
void dac_set_iop_muted(dac_t *dac, int muted);

/* Advances by `cycles`.
   Resamples the currently-held DAC output level into the sample buffer at PSEMU_DAC_SAMPLE_RATE_HZ.
   Outputs silence while ctrl's enable bit is clear, or while IOP-muted (see dac_set_iop_muted). */
void dac_tick(dac_t *dac, uint32_t cycles);

/* Drains up to max_samples into buf (mono, signed 16-bit).
   Returns the number of samples actually written.
   This is less than max_samples when the buffer holds fewer samples than requested. */
uint32_t dac_read_samples(dac_t *dac, int16_t *buf, uint32_t max_samples);

#endif
