#ifndef PSEMU_DAC_H
#define PSEMU_DAC_H

#include <stdint.h>

/* Sony PocketStation DAC (audio out), confirmed via the documented documentation-sourced documentation - see docs/hardware-notes.md. Two 32-bit
   registers at 0x0D800010: ctrl(+0x0, bit0 = audio enable), data(+0x4,
   bits 6-15 = DACV, a signed 10-bit two's-complement output level,
   +max=0x1FF, center=0, -max=0x200; bits 0-5 should be zero per the
   documentation).

   Real hardware has no square-wave/noise generator or sound DMA channel
   - software produces tones entirely by writing new DACV levels to
   DAC_DATA at audio rates (typically driven by Timer1/IRQ-8), i.e. it's
   a raw, CPU-bit-banged 1-sample-at-a-time DAC with no fixed sample
   rate of its own. To expose this through a standard fixed-rate audio
   API, dac_tick resamples the currently-held output level (zero-order
   hold - the value most recently written stays "current" until the next
   write) into a ring buffer at a fixed internal rate, PSEMU_DAC_SAMPLE_RATE_HZ. */
#define DAC_REG_SPAN 0x10u

/* Real hardware runs at a variable clock (up to ~7.995MHz - see the
   CPU_FREQ table referenced in docs/hardware-notes.md), and this
   emulator doesn't model per-instruction cycle counts or CLK_MODE at
   all, so any single fixed "cycles per real second" is an
   approximation. PSEMU_ASSUMED_CPU_HZ matches frontends/desktop/main.c's
   own real-time pacing (33000 cycles per 32Hz frame) - an earlier
   version of this file assumed ~4MHz instead (matching rtc.h's
   RTC_TICK_CYCLES), which was an unvalidated guess matching one
   uncalibrated constant to another; real-hardware testing showed that
   rate made on-screen animations visibly too fast, so this now follows
   the frontend's own empirically-confirmed pacing instead. Keep this in
   sync with main.c's psemu_run() cycle count if that ever changes -
   otherwise audio pitch/tempo and on-screen timing will drift apart
   from each other again. */
#define PSEMU_ASSUMED_CPU_HZ (33000u * 32u) /* = 1056000 */
#define PSEMU_DAC_SAMPLE_RATE_HZ 8000u
#define DAC_CYCLES_PER_SAMPLE (PSEMU_ASSUMED_CPU_HZ / PSEMU_DAC_SAMPLE_RATE_HZ) /* = 132 */

#define DAC_SAMPLE_BUFFER_SIZE 8192u

typedef struct dac {
    uint32_t ctrl;
    uint32_t data;
    int16_t current_sample; /* current held output level, rescaled to full int16 range */
    uint32_t cycle_accumulator;

    int16_t sample_buffer[DAC_SAMPLE_BUFFER_SIZE];
    uint32_t sample_write_pos;
    uint32_t sample_read_pos;
    uint32_t sample_count; /* samples currently buffered, <= DAC_SAMPLE_BUFFER_SIZE */
} dac_t;

void dac_init(dac_t *dac);
uint8_t dac_read8(dac_t *dac, uint32_t offset);
void dac_write8(dac_t *dac, uint32_t offset, uint8_t value);

/* Advances by `cycles`, resampling the currently-held DAC output level
   into the sample buffer at PSEMU_DAC_SAMPLE_RATE_HZ. Outputs silence
   while ctrl's enable bit is clear. */
void dac_tick(dac_t *dac, uint32_t cycles);

/* Drains up to max_samples into buf (mono, signed 16-bit), returns the
   number actually written (less than max_samples if the buffer is
   currently more empty than that). */
uint32_t dac_read_samples(dac_t *dac, int16_t *buf, uint32_t max_samples);

#endif
