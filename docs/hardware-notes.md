# PocketStation hardware notes

This file documents this emulator's hardware model.

Facts here come from three sources, ranked by trust:

1. Direct testing on real hardware.
2. Tracing real BIOS and app execution with `tools/inspect.c` against real dumps. These dumps are not committed to the repo. See `testdata/` (gitignored).
3. Official register documentation.

Some facts are inferred or unconfirmed. The section "Known open questions and unconfirmed behavior" at the end lists each one. Do not treat anything in that section as confirmed fact.

This emulator's CPU core has been validated by tracing real BIOS and real app execution: a real copy of Chocobo World, and a real ID-editing homebrew app. Combined, these traces cover hundreds of millions of instructions. After the fixes described below, these traces produced zero unrecognized-opcode faults.

## CPU

The CPU is an ARM7TDMI (ARM and Thumb instruction sets), fabricated by Atmel.

On reset, the CPU starts in Supervisor mode, ARM state. The first ~12 instructions visit FIQ mode, then IRQ mode, then return to Supervisor mode. Each mode visit initializes that mode's banked stack pointer (SP). Confirmed via a real BIOS trace.

**Real ARM7TDMI behaviors confirmed against real hardware and apps.** Each behavior below was a gap in this emulator's CPU core. Real execution traces found each gap, not synthetic tests. Each gap is now fixed:

- **Thumb `BL` sets bit 0 of the return address**: `(R15+2)|1`. This holds even on plain ARMv4T; it is not an ARMv5 `BLX`-only feature. Bit 0 set means a later `BX LR` stays in Thumb state. Fixed in `exec_long_branch_link`. See `test_thumb_bl_bx_lr_stays_thumb`.
- **`LDM` with the `^` suffix, register list includes `PC`** (example: `LDM SP!,{r1-r12,lr,pc}^`, the real SWI handler's return sequence): this restores the entire CPSR from the current mode's SPSR, not just `PC`. This is a distinct encoding from `MOVS/SUBS PC,LR`. Fixed in `exec_block_transfer`. See `test_arm_ldm_exception_return`.
- **A misaligned `LDRH` (odd address)** reads the aligned-down halfword, then rotates the result right by 8 bits. This swaps the halfword's two bytes before the value reaches the register. It does not simply round the address down and drop the low bit.

  **A misaligned `LDRSH`** does not get this rotate-then-sign-extend treatment. Real hardware instead performs a sign-extended byte load (`LDRSB`) from the odd address.

  Fixed in `exec_halfword_transfer`. See `test_arm_ldrh_misaligned_quirks`.

  Found via a real ID-editing homebrew's font-glyph routine. This routine reads a byte-packed table one byte at a time, using `LDRH Rd,[Rn],#1` (post-increment by 1, not 2), then masks the result to the low byte. This only works because of the rotation described above.
- **FIQ takes strict priority over IRQ.** FIQ entry sets both `CPSR.F` and `CPSR.I`. IRQ, SWI, and abort entry set only `CPSR.I`. See "Interrupt controller" below.

The CPU clock speed is variable, controlled via `CLK_MODE`. See "CLK_MODE" below.

## Memory map

| Range | Size | Purpose |
|---|---|---|
| `0x00000000` | 2KB | RAM. `0x000–0x1FF` kernel, `0x200–0x7FF` user. |
| `0x02000000` | virtual | FLASH1 — the currently selected/running app, mapped from FLASH2. |
| `0x04000000` | 16KB | BIOS ROM. Kernel around `0x1E00`, GUI around `0x2200`. |
| `0x06000000` | — | Flash control I/O (`F_CTRL`, `F_BANK_FLG`, `F_WAIT`); `+0x300` carries `F_EXTRA` (see "Hardware ID (F_SN)" below). |
| `0x08000000` | 128KB | FLASH2 — physical flash, 15 blocks. |
| `0x0A000000` | 0x14 | Interrupt controller: hold(+0x0, R), status(+0x4, R), enable(+0x8, W, ORs in), mask(+0xC, W, ANDs matching bits out of enable), acknowledge(+0x10, W, clears matching hold+status bits). |
| `0x0A800000`+ | 0x30 | 3 timers, 0x10 bytes apart: period(+0x0), count(+0x4), control(+0x8, bits0-1 = clock divisor, bit2 = enable). |
| `0x0B000000`+ | 0x8 | `CLK_MODE`(+0x0) - CPU/timer clock speed control. `CLK control`(+0x4) bit0 - stop/standby, halts the CPU until a button wakes it. Sleep/wake confirmed on real hardware; the attribution to this bit is inferred (see "CLK control" below). |
| `0x0B800000` | 0x10 | RTC: mode(+0x0), control/adjust(+0x4), time(+0x8, R), date(+0xC, R). |
| `0x0C800000`+ | 0x10 | IR: `IRDA_MODE`(+0x0, protocol/send-receive mode), `IRDA_DATA`(+0x4, beam on/off), `IRDA_MISC`(+0xC, unknown/reserved). See "IR / IR Link". |
| `0x0D000000` | 0x8 | `LCD_MODE`: bit6 `DISON` (display on/off), bit7 `ROT` (rotate 180°). |
| `0x0D000100` | 128B | LCD VRAM. |
| `0x0D800000` | 0x10 | IOP power control: IOP_CTRL(+0x0, unmodeled, no known effect), IOP_STOP/IOP_STAT(+0x4, W/R, sets bits), IOP_START(+0x8, W, clears bits), IOP_DATA(+0xC, unused by the real BIOS). Bit5 = Sound Enable. |
| `0x0D800010` | 0x10 | DAC: `ctrl`(+0x0, bit0 enable), `data`(+0x4, bits6-15 signed 10-bit `DACV`). |

BIOS ROM and FLASH2 support only 16-bit and 32-bit reads. RAM supports 8-bit, 16-bit, and 32-bit access.

## LCD

The LCD is 32×32 pixels, 1 bit per pixel. VRAM is 128 bytes: 32 rows, one 32-bit word per row. Bit 0 of each word is the leftmost pixel. A clear bit (0) is white; a set bit (1) is black. Hardware refreshes the display at approximately 32Hz after the CPU writes a row.

`LCD_MODE` (`0x0D000000`) is a separate register from VRAM.
- Bit 6 (`DISON`) controls whether the display shows anything. When clear, the display is blank.
- Bit 7 (`ROT`) rotates the displayed image 180°: it reverses scanline order, and reverses each scanline's bits left-to-right. Real hardware keeps this bit in sync with the docking flag, so the screen reads right-side-up whether the device is handheld or docked.

`psemu_get_framebuffer` returns this post-processed image, not raw VRAM.

The default value of `mode` has `DISON` set. This is this emulator's own safe default; the real power-on-reset (POR) value is not documented. This default ensures an app renders even if it never writes to `LCD_MODE`. See `test_lcd_mode_dison_and_rotate`.

**Confirmed on real hardware** (`pk_timing_bench`, this project's homebrew app): both VRAM and `LCD_MODE` reject operations that this emulator's own model allows without complaint.

**`LCD_MODE` bug.** A first real-hardware build turned the display on with a blind overwrite: `mov r1,#0x40; str` (writes `LCD_MODE` directly, not a read-modify-write). This emulator's own `LCD_MODE` default already equals `0x40`, so the blind overwrite was a no-op in every emulator test. On real hardware, the same write instantly blanked the screen and hung the device; recovery needed the physical reset button. The real pre-dispatch/POR value of `LCD_MODE` is undocumented. The working theory: the real BIOS leaves some other, unidentified bit set that real display output depends on, and the blind overwrite cleared it. Switching to a read-modify-write (read the current value, OR in only bit 6) fixed the bug.

**VRAM bug.** A byte-wide (`LDRB`/`STRB`) read-modify-write into VRAM produced a single lit horizontal row on real hardware, instead of the one intended pixel. This emulator's own VRAM model is byte-addressable and allows this without complaint. Switching to a word-wide (`LDR`/`STR`) read-modify-write fixed the bug; VRAM rows are already word-aligned, so this needs no per-byte splitting.

Neither bug reproduces in this emulator as it stands: `lcd_mode_write8` and `lcd_write8` (`core/src/lcd.c`) are unconditionally byte-granular, with no width restriction modeled. See `docs/app-notes.md` for the homebrew-facing writeup of both fixes.

## Buttons

There are 5 face buttons (Up, Right, Down, Left, Fire) and one physical reset button. The face buttons read as bits 0-4 of `INT_INPUT` at `0x0A000004`.

A button's `hold` bit is a momentary edge pulse for each physical press. It is not a sustained level for the whole time the button stays held down. This differs from level-triggered sources (IOP, battery, timers), whose `hold` bits stay set for as long as the condition holds.

This distinction matters for the BIOS's system-tick callback, a fixed-priority chain: IOP, then battery, then Timer1, then Action (buttons), then RTC. The chain only reaches the RTC-driven redraw step if none of the earlier bits are set in `hold`. If the Action bit stayed asserted for the whole time a button was held, it would permanently block RTC processing for that duration.

`status` still tracks the live button level, for code that polls it directly.

Implemented via `intc_clear_hold_only` (`core/src/intc.c`), called from `psemu_set_buttons` when a button stays held with no new press edge. See `test_button_hold_pulses_not_sustained`.

Button input is processed through the interrupt (`hold`) and callback path, not by directly polling `status`.

**App container format (PSX Title Sector) and the real BIOS's app-selection/dispatch routine are documented in [docs/app-notes.md](app-notes.md).** That file targets PocketStation app developers, as reference material for a future dev kit. This file covers only the emulator's own implementation.

## Flash memory

**FLASH2** (`0x08000000`, physical, 128KB) has 16 blocks of 8KB each. Block 0 holds a PS1-style memory-card directory: 16 frames of 128 bytes each. Frame 0 is the card header; frames 1-15 describe blocks 1-15.

**FLASH1** (`0x02000000`, virtual) is a 16-slot banked window onto FLASH2. Two `FLASH_CTRL` registers resolve this window live:
- `F_BANK_FLG` (`FLASH_CTRL+8`): a bitmask of which physical 8KB blocks are enabled for the current app.
- `F_BANK_VAL` (`FLASH_CTRL+0x100`-`0x13C`, 16 words, reset value 0): for each physical block, this says which virtual bank slot (0-15) it appears at (`table[physical]=virtual`). This is the reverse direction from a typical page table. Resolving virtual to physical requires a reverse search over the 16 entries (`flash_resolve_physical_bank`).

When `F_BANK_VAL` is untouched (its reset value, for every physical bank), resolution falls back to a contiguous linear mapping, starting at the lowest-numbered enabled physical block. Every real BIOS app-dispatch trace observed so far hits this fallback case: the real BIOS writes only `F_BANK_FLG`, never `F_BANK_VAL`.

See `test_flash_bank_val_remapping` for a non-contiguous, reordered mapping, and `test_flash_bank_select` for the fallback case.

`FLASH_CTRL` (`0x06000000`) also has:
- `+0`: a command/commit trigger. A write of `2` commits a bank-select change. A real BIOS routine then busy-waits on this same address, polling for bit 0 to read back `1`. This emulator's commits are synchronous, so reads of `+0` always OR in bit 0.
- `+0x10` (`F_WAIT2`, waitstates and flash-write status): a real app's flash-write routine polls this, expecting bit 2 to read back set once a write completes. This emulator's writes complete instantly, so this bit always reports "not busy".

See `test_flash_ctrl_busy_wait_bits`.

**`F_KEY1` (`0x08002A54`) and `F_KEY2` (`0x080055AA`) are flash-unlock command-latch addresses, not data storage.** Real flash hardware intercepts a write of the documented `FFAAh`/`FF55h`/`FFA0h` sequence at these addresses as an unlock command, arming the chip's write-unlock state machine. It does not store the value; the byte physically at that address is unaffected. `flash_write8` and `flash1_write8` (`core/src/flash.c`) no-op writes to either 16-bit-wide key address, in both the FLASH2 physical path and the FLASH1 virtual window. See `test_flash_key_addresses_are_not_data_storage`.

**App-selection and dispatch is documented in [docs/app-notes.md](app-notes.md).** See that file for the real BIOS's app-selection routine, and for how `flash_load_app` synthesizes a directory for a single loaded app.

## Register banking

Banked per mode, as on any ARM7TDMI: `r13`, `r14`, and `SPSR` for each of FIQ/IRQ/SVC/ABT/UND, with User and System sharing one set.

**`r8`-`r12` are banked as well, but for FIQ only.** That is what lets a FIQ handler use those five registers as scratch without saving them, and it is the reason "fast interrupt" is fast. Every non-FIQ mode shares a single copy, so an SVC-to-IRQ switch must leave them alone; only crossing the FIQ boundary swaps them.

This emulator banked only `r13`/`r14` for a long time, so a FIQ handler silently destroyed the interrupted code's `r8`-`r12`. **This was live, not theoretical.** Pop'n Music drives its audio from Timer2, which is FIQ-routed (`INT_FIQ_MASK`), and measurement shows its FIQ handler writing four of the five registers on essentially every entry: 462 of 463 FIQs in a boot-and-play run left `r8`, `r9`, `r11` and `r12` changed. Under the old model each of those writes landed on whatever the interrupted code was holding. See `test_fiq_banks_r8_to_r12`.

IRQ does *not* bank these on real hardware either, so an IRQ handler has to save them itself, and the real BIOS's does - measured at 0 of 25120 IRQ entries leaving them changed.

**`LDM`/`STM` with the `^` suffix and PC absent from the register list** transfers the User bank rather than the current mode's, which is how a privileged handler reaches an interrupted app's `r13`/`r14` without switching modes. `exec_block_transfer` ignored the S bit in that case and moved the current mode's registers instead. The real BIOS uses the form - `STMIA r0!,{r13,r14}^` at `0x04001944` with the matching `LDM` at `0x04001B90` - though no app this project can drive has been observed executing those, so this was a latent gap rather than an active bug. See `test_ldm_stm_user_bank_transfer`. The S-bit-with-PC case is the separate CPSR-restore idiom described under "CPU" above.

## SWI (syscall) mechanism

The vector table at RAM `0x00000000`-`0x0000001C` has 7 identical `LDR PC,[PC,#0x18]` entries, plus one filler entry. The real handler addresses follow immediately, in a literal pool, in this order: reset, undefined instruction, SWI, prefetch abort, data abort, reserved, IRQ, FIQ. The SWI vector is at `0x08`.

The real SWI handler runs these steps:

1. Save `r1-r12,lr`.
2. Read `SPSR`'s `T` bit to compute the original SWI instruction's address in `lr` (subtract 2 if Thumb, 4 if ARM).
3. Read that instruction's low byte as a syscall number.
4. Look up a function pointer from a dispatch table (`table[syscall_number]`). The table's base address is stored at RAM `0x000000E0`.
5. Call the function pointer via an interworking `BX`.
6. Return via `LDM SP!,{r1-r12,lr,pc}^` (see "CPU" above).

## Interrupt controller

`0x0A000000` has four registers: `hold`, `status`, `enable`, `mask`. `intc_irq_asserted` and `intc_fiq_asserted` (`core/src/intc.c`) compute `hold & enable & INT_IRQ_MASK` and `hold & enable & INT_FIQ_MASK` on demand.

The bit-to-source mapping (`INT_BTN_*`, `INT_TIMER*`, `INT_RTC`, `INT_IOP`, `INT_IRDA`) matches the official 14-source table exactly. FIQ sources are bit 6 (`COM`) and bit 13 (`Timer2`). Every other source is IRQ.

Both IRQ and FIQ are level-triggered: the emulator polls both lines every CPU step, not as a one-shot latched request. The CPU keeps re-entering the handler for as long as the line stays asserted.

**FIQ has strictly higher priority than IRQ.** FIQ entry sets `CPSR.F` in addition to `CPSR.I`; IRQ entry sets only `CPSR.I`. See `arm7tdmi_step` in `core/src/cpu.c`.

`intc_fiq_asserted` was implemented correctly early in this project, but no code called it until this was fixed. Before the fix, this emulator never delivered FIQ, for any app. See "Hardware ID (F_SN)" below for how this bug was found. See `test_fiq_delivery_and_priority` and `test_fiq_takes_priority_over_irq`.

Button and RTC sources (`INT_STATUS_MASK`) latch into both `hold` and `status` on assertion, and clear from both together on deassertion.

**The BIOS's IRQ and FIQ vector handlers each call a separate app-registered callback, from two different fixed low-RAM slots.** Confirmed by disassembling a real J-110 BIOS dump: the IRQ vector handler (`0x04001414`) reads its callback pointer from RAM offset `0xFC`. The FIQ vector handler (`0x040014D4`) reads its own from offset `0x100`, and only for a non-`COM` FIQ source; a `COM` FIQ (bit 6) branches to internal BIOS handling first and never reaches that slot. Neither handler acknowledges the interrupt itself. Both leave that entirely to the registered callback, matching `IRDA_DATA`'s own no-BIOS-API pattern (see "IR / IR Link" below). An app installs each callback via `SWI 1`: `r0` selects the slot (`1` for IRQ, `2` for FIQ), `r1` is the handler address, and `0` unregisters. Registering into the wrong slot for a source's actual exception type does not fail loudly. It leaves that source's HOLD bit permanently unacknowledged, so the CPU re-enters the vector on every step and never leaves it. `pk_timing_bench`'s screen 10 hit this directly (see `pk_timing_bench/README.md`): it first tried the IRQ slot for Timer2, a FIQ source, and hung in this emulator before ever reaching real hardware.

## Timers

There are 3 timer channels at `0x0A800000`+, spaced `0x10` bytes apart. Each channel has: `period` (+0x0), `count` (+0x4), `control` (+0x8). `control` bits 0-1 select the clock divisor: `0` or `3` = /2, `1` = /32, `2` = /512. `control` bit 2 enables the timer. `count` decrements once per `divisor` raw cycles, not once per raw cycle.

Timer1 drives the BIOS's audio-generation loop, and also drives general GUI ticks: the date-setting screen's blink, the HELLO boot animation. Timer1 is not an audio-exclusive IRQ source. Timer2 is the FIQ-driven timer (see "Interrupt controller" above).

Timer `count` follows raw, `CLK_MODE`-scaled cycles. Real timers are clocked by the System Clock, tied directly to the CPU's variable clock (see "CLK_MODE" below). RTC and DAC are not tied to this clock.

**Confirmed: a timer armed with period P expires every P+1 ticks, not P.** The counter runs P, P-1, … 1, 0 and reloads on the tick *after* it reaches zero, so zero is a state the counter actually occupies.

Measured directly on real hardware via `pk_timing_bench` screen 6 (raw values logged in `pk_timing_bench/VERIFICATION.md`), by polling Timer2's reloads with Timer0 as a stopwatch and taking no interrupts at all. Timer2 at period 1016 across 256 reloads, and at period 2032 across 128 reloads, each came back **exactly 1 tick per reload** slower than a plain P-tick period predicts. The same absolute excess at both periods rules out a rate/divisor error and pins it to a fixed per-period off-by-one.

`core/src/timer.c` previously consumed exactly `count` ticks per reload, so every timer fired one tick early. This is now fixed; three timer expectations in `tests/cpu_test.c` had encoded the old behavior and were corrected with it.

**Timer `period` and `count` are 16-bit registers, not the 32-bit counters this emulator originally modeled.** Now implemented: `timer_write8` masks both with `TIMER_REG_MASK` (`core/src/timer.h`), so the upper half of a wider store is discarded, and every reload masks too. See `test_timer_registers_are_16_bit`.

Found via `pk_timing_bench` (this project's homebrew timing-benchmark app):

- Every raw `count` snapshot captured on a real unit had its upper 16 bits at zero.
- A measurement loop wrote `period`/`control` once, then read `count` before and after a long loop with no reconfiguration in between. When the loop ran long enough to accumulate more than 65536 raw ticks, the *after* reading came back numerically larger than the *before* reading. The only explanation: the counter wrapped past zero and reloaded partway through the loop, at a 16-bit boundary rather than a 32-bit one.

The width was originally recorded here as an inference from those readings alone. **A real commercial app has since confirmed it independently.** Pop'n Music (`testdata/popnmusic.mcr`) drives its audio from Timer2, the FIQ-driven channel, and programs it with a value whose upper half is nonzero. Under the old 32-bit model the surviving upper bits stretched the period from 851 ticks to `0x03240353`, about 52.6 million — roughly 62,000x too long — so the audio interrupt never fired. The game opened its DAC gate (`DAC_CTRL`=1, IOP bit5 started), played an entire song with notes scrolling and a results screen, and wrote `DAC_DATA` exactly zero times: total silence with every gate open. Masking to the real 16-bit width leaves period `0x0353` (851), matching the `0x34F` (847) programmed into Timer1 alongside it, and the music plays.

This also makes the failure mode worth remembering: a too-wide timer period does not announce itself. Nothing faults, the app runs normally, and the only symptom is a peripheral that silently never fires.

`test_timer_scales_with_clk_mode` had encoded the old behavior, loading a period of 100000000 that no real timer can hold; it now uses the largest real 16-bit period, with a halved budget so the fast-clock case still never wraps.

See `docs/app-notes.md`'s timing-benchmark writeup for the full before/after data.

## RTC

`0x0B800000`: `mode` bit 0 (`PRGSEL`) selects the RTC's operating mode:

- Run mode (`0`): ticks at 1Hz, auto-advances the clock.
- Program/pause mode (`1`): ticks at approximately 4096Hz, does not auto-advance. This lets a manual adjust-write step one field without the clock moving underneath it.

`mode` bits 1-3 (`CNTSEL`) select which BCD field a `control`/`RTC_ADJUST` write adjusts.

Auto-advance cascades in this order: seconds, minutes, hours, day-of-week. It does not cascade into `date` on a day rollover. No documentation confirms or denies this gap; this emulator inherits it rather than confirming it as correct.

Real hardware power-on-reset values: `RTCClock = 0x04000000` (day-of-week BCD 4, 00:00:00), `RTCCalendar = 0x00980101` (1998-01-01). `RTC_DATE` bits 24-31 are an unused, unidentified field, not a "year-high"/century byte. The real century value lives in battery-backed kernel RAM, and only the `GetBcdDate` SWI exposes it.

RTC ticks at a fixed 1Hz in Run mode, regardless of `CLK_MODE`. It runs from a separate oscillator, independent of the CPU clock.

**Both documented rates are waveform rates, not transition rates: 1Hz running and 4096Hz paused, with two line transitions per pulse.** Measured on real hardware by `pk_timing_bench`'s screen 14 (see its `VERIFICATION.md`): paused, 256 transitions in exactly 0.031250 seconds — 8192 transitions per second, so 4096 full pulses; running, four transitions in exactly 2.000 seconds. This emulator treated both figures as transition rates and ran its line at half the real frequency in both modes. `rtc_tick` now advances the clock one second per full pulse rather than per transition, so the wall clock stays 1:1 while the line runs at the real rate.

That paused measurement incidentally **confirms `CLK_MODE 7` = 3,997,696Hz**, a figure the frequency table had only ever taken from documentation: landing exactly on 0.031250 seconds is not possible if the real CPU rate differs meaningfully.

**`RTC_TICK_CYCLES_RUN` is that 1Hz, expressed in `PSEMU_ASSUMED_CPU_HZ` reference cycles, so the two constants are equal by definition.** This needs no measurement: the RTC drives a wall clock, and one emulated second has to last one real second. It was `4000000` for a long time - a value chosen only to make a wait-for-pulse loop resolve quickly, and explicitly never checked against a real 1Hz reference. That is 3.79 reference-seconds per tick, so the device's own clock ran nearly 4x slow: 60 seconds of real time advanced it by 15, losing about 45 minutes an hour. Anything that read the PocketStation's clock saw that drift.

The BIOS resets the clock to Jan 1 1999 in a documented condition Sony calls "The RTC Problem", a software workaround for inaccurate clock hardware. This reset is a software action, performed via the normal `RTC_ADJUST` mechanism. This emulator's own reset state does not build in this behavior.

### Where the date/time settings actually live

Traced with `tools/datetime_probe.c`. The settings are split across the RTC registers and several RAM shadows, and none of it reaches flash:

| Location | Holds | Written by | Role |
|---|---|---|---|
| `RTC_TIME`/`RTC_DATE` | seconds, minutes, hours, day-of-week; day, month, year (BCD) | `RTC_ADJUST` increments from `0x0400055A`, `0x04000580` and neighbours | drives the clock display |
| RAM `0x426` | century, BCD `0x19` | `0x0400330C`, once at boot | drives the clock display |
| RAM `0x0CF` | century, BCD `0x19` | `0x04000350` | derived from the RTC year, not an input |
| RAM `0x0CD` | year, BCD `0x99` | `0x04000668` | derived, as above |
| RAM `0x120`-`0x123`, `0x128`-`0x12B` | a date, `01 01 99 19` | `0x040003D4`, `0x04000498` | boot-time working copies; nothing observable reads them back |

**The century exists only in RAM.** `RTC_DATE` has no century field, which is why `GetBcdDate` is the only way to read it back.

**Which byte is the century was established by experiment, and it is `0x426`, not `0x0CF`.** `tools/datetime_probe.c`'s `screen` mode boots to the clock screen, pokes a chosen combination of these locations after the boot reset has finished, and dumps the LCD so the rendered digits can be read directly. The screen renders the date as `YYYY/MM/DD` (the day is cut off at 32px). Results, all with the RTC set to 2026-08-01:

- RTC registers alone: renders `1926/08/`. The RTC supplies month and the two low year digits; the century does not follow.
- Plus `0x0CF`/`0x0CD`: still `1926/08/`. Poking them changes nothing on screen.
- Plus the `0x120`/`0x128` working copies: still `1926/08/`.
- Plus `0x426`: renders `2026/08/`.

`0x426` is written exactly once, at boot, from `0x0400330C`, and thereafter only read - 251 times across 3M instructions, all from `0x04002542`, the clock-display routine.

**`0x0CF` and `0x0CD` are outputs, not inputs.** In the RTC-only run above, the BIOS itself moved `0x0CF` from `0x19` to `0x20` and `0x0CD` to `0x26` in response to the RTC year becoming 26, without either being poked. So the BIOS derives a century from the two-digit year through some windowing rule, and caches it there.

**There are two independent century bytes, and they can disagree.** `GetBcdDate` reads `0x0CF`; the clock screen reads `0x426`. Confirmed by calling the SWI directly (see below) with `RTC = 2026-08-01`, `0x0CF = 0x20` and `0x426` left at `0x19`: the SWI returned `0x20260801` while the screen rendered `1926/08/`. Anything that overrides the date has to write both, or apps and the BIOS's own clock will report different years.

**`0x426` is app RAM once an app is dispatched, and a per-frame override of it corrupts the app.** The dispatch routine clears user RAM `0x200`-`0x7FF` before branching to the entry point (see `docs/app-notes.md`, "App-selection and dispatch"), and `0x426` is inside that range. Traced with `tools/datetime_probe.c`'s `browse` mode against `testdata/YGO_jap.mcr`: once the app is running it both reads `0x426` (from FLASH1 PCs `0x02002772` and `0x02001F3E`) and rewrites it continuously, cycling through values like `0xE7`/`0x24`/`0x66` roughly every 11000 instructions. A frontend re-applying a century byte there 32 times a second is writing into a running app's working memory. This is not theoretical: it is what made the desktop app's `datetime_override=os` setting drive Yu-Gi-Oh's PocketStation app into its "ODD DATA" screen, and it is why `psemu_app_running` exists (see `core/include/psemu/psemu.h`).

**The BIOS's browse screen does not read either century byte, so pinning them is safe while the BIOS shell is up.** This was worth checking separately, because the browse screen renders each card app's icon and animates it, and `0x426` sits in user RAM next door to the volume byte's own open question. `tools/datetime_probe.c`'s `browse` mode navigates to the browse screen, seeds `0x426 = 0x5A` and `0x0CF = 0xA5`, and watches both bytes for the rest of the run. Against three real cards (`YGO_jap.mcr`, `popnmusic.mcr`, `samplememcard.mcr`), every rendered frame across 5.5M instructions is byte-identical to a control run that seeds nothing, and the read counts and reading PCs match exactly. Only four PCs ever read either byte, all of them date/clock code: `0x04000386` reads `0x0CF` once at boot, and `0x04002542` (the clock-display routine), `0x040029E2` and `0x04002A08` read `0x426`. The two latter PCs read it once each, while the clock screen is still on display. Nothing in icon rendering touches either byte.

### SWI dispatch table (J110)

`tools/datetime_probe.c`'s `swi` mode calls every entry of the kernel dispatch table in isolation, from a post-boot state snapshot, with a sentinel return address and a poisoned scratch buffer, restoring state between entries. The table base is at RAM `0x0E0` and reads `0x04001688`.

| SWI | Entry | Returns |
|---|---|---|
| 10 (`0Ah`) | `0x040017A5` | `FlashReadSerial` - returned `0x410000D3`, this emulator's default `F_SN`, confirming the documented mapping |
| 13 (`0Dh`) | `0x04000369` | `GetBcdDate` - `r0` = BCD `CCYYMMDD`. Returned `0x19990101` at boot, `0x20260801` with the RTC and `0x0CF` moved |
| 14 (`0Eh`) | `0x04000391` | `GetBcdTime` - `r0` = BCD, day-of-week/hours/minutes/seconds, matching `RTC_TIME` exactly |

Both date SWIs return their value in `r0` rather than writing through a caller-supplied buffer; the scratch buffer passed in `r0` came back untouched. `GetBcdDate` composes its result from two sources: the century byte at RAM `0x0CF`, and the day/month/year from `RTC_DATE`.

Entries 2, 9, 12 and 15 never reached the sentinel within 20000 instructions, so nothing is claimed about them; entry 12 (`0x04000519`) sits inside the boot date-setting routine and reads `0x0CF`, which makes `SetBcdDate` a reasonable guess worth confirming separately.

**The boot path writes the date unconditionally.** At around instruction 14686 the BIOS fills the RAM shadow with 1999-01-01, then switches the RTC to program mode (`PRGSEL=1`) and walks each field with `RTC_ADJUST` increments until the hardware clock matches. It reads `RTC_DATE` first (at `0x0400036E`/`0x04000372`, returning the power-on-reset `0x00980101`), but no RAM date byte is ever read before it is written, so the decision is not gated on RAM contents.

**Three separate attempts to reach a warm-boot path all failed.** In each case the RAM shadow still ended at 1999-01-01 and the RTC was still walked to match:

- Preloading `RTC_DATE`/`RTC_TIME` with a valid later date (2007-06-15).
- The same, plus preloading every RAM shadow and the century byte.
- The same, plus loading a real 128KB memory card image (the BIOS reads `FLASH2` offset 0 shortly before the decision, so card contents were the last untested input).

**This is a milder version of the same shape as the volume setting.** Volume (`0x290`) is also cleared on every boot this emulator can produce, and is also expected to survive on real hardware because the battery holds SRAM up — but it is cleared once, early, and never rewritten afterwards, so pinning the byte is enough (see "System sound volume setting"). Date/time is walked to a target by the BIOS over many instructions, so there is nothing equivalent to pin. `psemu_reset` is equivalent to inserting a fresh battery, and no other boot path exists here, so the Jan-1999 reset may well be faithful to that specific situation rather than a bug. What is not established is whether real hardware has a warm path at all, and what gates it.

Consequence for persistence: preloading registers before boot cannot work for date/time, because the BIOS overwrites them afterwards. The options are to restore a full save state (which bypasses the boot path entirely, and already works), or to re-apply the wanted date after boot through the same `RTC_ADJUST` walk the BIOS itself uses.

## IR / IR Link

`0x0C800000`: `IRDA_MODE` (`+0x0`) bit0 `IFMODE` (0=Receive, 1=Transmit), bit1 `STDBY` (0=Active, 1=Stand-by), bit2 `BGEN` (0=Enable 40kHz carrier generator, 1=Disable), bit3 `BFLT` (0=Enable filter, 1=Disable). `IRDA_DATA` (`+0x4`) bit0 `LED`: in transmit, the value software bit-bangs (0=off, 1=on); in receive, this emulator's own read-back of the current demodulated level. That receive-side meaning is inferred, not confirmed. See below. `IRDA_MISC` (`+0xC`) is unknown or reserved. This emulator reads it back as 0 and ignores writes. This is the same stub treatment as `BATT_CTRL` below.

**Source: an independently published, community-maintained hardware reference for this register range.** This project built its own IR model by disassembly, before it checked that reference. The two agree independently on the register layout above. This agreement raises confidence in both. That reference is community reverse engineering, not a leaked Sony devkit spec. It marks several IR details "reportedly", or with a question mark.

Two published versions of that reference disagree with each other on `IRDA_MODE` bits 1-3. One gives `STDBY`/`BGEN`/`BFLT` as above. The other instead guesses a plain "disable" bit, plus two differently-named bits. That version calls its own guess uncertain. This project's disassembly settles the disagreement in favor of `STDBY`/`BGEN`/`BFLT`. A real app's behavior outranks a secondary source.

**What that reference does not have: any numeric timing.** It gives none of these:
- microsecond pulse widths
- a carrier-to-pulse ratio, beyond "long is usually twice as long as short"
- anything close to the ~184-tick transmit-timing gap this project's hardware testing is chasing (see "Known open questions" below)

A register reference cannot answer that gap. Only measurement on real hardware can answer it. This is why `pk_timing_bench` screens 6-8 exist.

**What that reference does confirm, independent of this project's own disassembly:**
- The real BIOS has no IR functions at all, aside from basic initialization and power-down handling. This matches what this project's own trace already found. A real app drives `IRDA_MODE` and `IRDA_DATA` directly from its own code. It uses its own hand-rolled interrupt handler, not a documented BIOS SWI. This project does not need to look for an undiscovered BIOS-level IR API.
- Real pulses alternate ON and OFF. They do not hold one long ON period. Real IR receiver hardware adapts to ambient light. A sustained signal risks looking like the new ambient level, not like data. This project's own transmit-side trace already shows this alternating short/long shape (see `tools/ir_probe.c`).
- `INT_IRDA` triggers on rising or falling edges of incoming data. A real handler typically reads Timer 2's live counter (reload `0xFFFFh`) to measure the interval. This project inferred that same technique from disassembly. The external reference now confirms it independently.

This emulator models IR as an **asynchronous edge relay between two independently-clocked instances**, not a lockstep timing simulation. This matches the real hardware directly: two separate PocketStations, two separate oscillators, linked only by an optical signal, with no shared clock. `core/src/ir.c`/`ir.h` implement the state machine:

- Writes to `IRDA_DATA`'s LED bit, while actually in an emitting state (`IFMODE`=transmit, `STDBY`=active), enqueue a timestamped edge (level + this `ir_t`'s own local monotonic clock) onto a TX queue. Leaving the emitting state (standby or receive) forces one final "LED off" edge, so the LED can never appear stuck on to whatever is downstream. **`BGEN` is deliberately not part of that test** - see "Two transmit styles" below.
- `ir_tick` (called once per CPU step from `psemu_run`, alongside `timer_tick`/`rtc_tick`/`dac_tick`) advances that local clock and resolves any RX-queue edges now due, applying a `BFLT` glitch-filter debounce and calling `intc_set_line(intc, INT_IRDA, 1)` on a qualifying edge while actively receiving (`IFMODE`=receive, `STDBY`=active) - dropped otherwise, the same way a real half-duplex transceiver simply doesn't see a pulse while transmitting.
- `psemu_ir_pop_tx_edge`/`psemu_ir_push_rx_edge`/`psemu_ir_get_clock_us` (`psemu.h`) expose this as a pull/push edge queue, the same shape `psemu_get_audio_samples` already uses to let a frontend drive real I/O without core knowing about that I/O's transport. Core has no networking code and never will; timestamps only cross this API boundary in real microseconds (converted from the internal cycle-unit clock), everywhere else core stays in the same reference-rate cycle units every other peripheral already ticks in.
- **A falling edge (LED digitally commanded off) is delayed by a fixed amount before it is enqueued, capped at the pulse's own ON duration.** This is a deliberate, documented concession, not a modeled physical effect: see `IR_TX_FALL_STRETCH_CYCLES` in `ir.c` for the full reasoning, and the "Unresolved" bullet below for the real-hardware testing behind it. A guard (`ir_t::tx_last_edge_cycles`) clamps this so a stretched falling edge can never land after the rising edge that follows it, keeping the TX queue's timestamps monotonic even for a pulse shorter than the stretch itself. **The cap matters as much as the guard**: the constant was tuned against one app whose pulses are wide envelopes, where 200 cycles is a small additive correction. Applied flat to Yu-Gi-Oh Forbidden Memories' ~7-cycle pulses it inverted the waveform outright - a measured 7-on/205-off arrived as 207-on/5-off, with 272 gaps collapsing to exactly 0 cycles, merging pulse pairs and the bits their gaps encoded. The guard alone only kept that from going *backwards*; a zero-length gap is already unrecoverable.

### Two transmit styles: `BGEN` does not gate emission

**Real apps do not agree on `BGEN`, and both work on real hardware.** This was found by disassembling Yu-Gi-Oh Forbidden Memories' PocketStation app after it transmitted nothing at all in this emulator, with the IR diagnostics showing no traffic whatsoever.

| | Chocobo World | Yu-Gi-Oh Forbidden Memories |
|---|---|---|
| `IRDA_MODE` while transmitting | `0x01` | `0x0D` |
| `BGEN` (bit 2) | 0 - hardware 40kHz carrier **on** | 1 - hardware carrier **off** |
| `BFLT` (bit 3) | 0 - glitch filter **on** | 1 - glitch filter **off** |
| Pulse shape | wide envelopes, hardware fills them with carrier | ~7 reference cycles (6.6us), LED driven directly |
| Encoding | pulse *width* (long is about twice short) | pulse *distance*: gaps of 205 or 406 cycles at a ~194us slot |

The two rows at the bottom explain the two at the top. Yu-Gi-Oh's pulses are far narrower than `IR_BFLT_DEBOUNCE_CYCLES`, which is exactly why it turns the glitch filter off in the same register write that turns the carrier generator off. The app is not misconfiguring anything; it is using a different, self-clocked signalling style and switching off the two hardware helpers that would interfere with it.

**`BGEN` therefore selects whether the hardware chops the LED's ON envelope into a 40kHz burst. It does not decide whether the LED lights.** This emulator relays only that ON/OFF envelope and explicitly does not model the sub-carrier inside it, so there is nothing left for `BGEN` to gate. `tx_emit_active` used to require `BGEN`=0, which made every one of Yu-Gi-Oh's `IRDA_DATA` writes a no-op: `tools/ir_probe.c` reported `edges relayed: A->B 0` against a save state parked on the app's own transfer screen, while the register trace showed the app bit-banging `IRDA_DATA` thousands of times.

Note this is a case where the external register reference's wording ("0=Enable 40kHz carrier generator, 1=Disable") is correct as far as it goes, and still led to a wrong model. The reference describes what the bit does to the carrier. It says nothing about emission depending on it, and the emulator inferred a dependency that real apps disprove.

**Two encodings, one link.** These two apps also settle a question about how to judge an IR transfer at all. `tools/ir_probe.c` now recovers the encoding from the relayed edges alone, with no knowledge of either app: it clusters both the gap between pulses and the width of the pulses, and reports whichever axis splits into two populations. Chocobo World comes back as pulse-width with symbols at 803/1408 reference cycles (760/1333us) on a constant 404-cycle gap; Yu-Gi-Oh as pulse-distance with 228/413-cycle gaps between fixed ~7-cycle pulses.

Note that Chocobo World's two symbols are in a ratio of **1.75, not 2**. The external reference's "long is usually twice as long as short" is approximate, and a detector built on integer multiples of a unit reads Chocobo World's short symbol as half a unit, rounds it to one, and reports a single-symbol stream with nothing to decode. Clustering two populations and measuring their separation carries no such assumption and reads both apps correctly. Independent corroboration: Chocobo World's own state block records its nominal unit at `+0x20` as `1200`, against the 1207 measured externally from the edge stream.

**Verified after the fix**, with `ir_probe` driving two instances from the app's own separately-armed sender and receiver save states:
- 13060 edges relayed sender-to-receiver, and a 398-edge reply back.
- Every one of the 6529 rise-to-rise gaps quantized exactly to 1 or 2 slot widths, with none ambiguous - the modulation survives the relay intact.
- Decoding those gaps as pulse-distance (1 slot = 0, 2 slots = 1, matching the transmit ISR at `0x020018D8`) yields an 816-byte message: a `0x0000000A` header, a 199-entry table of `u32`s, and a trailer. The reply decodes to `FFFFFFFF` followed by five `0x0000007F` words.
- The receiving instance's screen differs materially from a control run with a silent sender, rendering a received card sprite the control never shows.
- Chocobo World still completes a full bidirectional exchange (`A->B VERIFIED, B->A VERIFIED`), so the change is not a trade of one app for the other.

**Unconfirmed/inferred, flagged in `ir.h`'s own comments the same way as every other unconfirmed fact in this document:**

- The `BFLT` debounce window (~2 carrier periods, ~50us) is not documented externally. Over 200 million traced instructions in this project, nothing touched these registers. There is no real-hardware measurement to check this window against.
- `IRDA_DATA` reflecting the live demodulated level on read while in receive mode. No documentation this project found describes the receive-side meaning of this bit. Only the transmit-side ("LED") meaning is documented.
- Real IR pulse-length measurement (reading Timer2's live counter, reload `0xFFFFh`, from the `INT_IRDA` handler) is a real BIOS/software technique this emulator does not need to special-case: Timer2 already ticks independently every step regardless of IR state, so a real ISR reading it during an `INT_IRDA` handler already sees a plausible value with no extra coupling required between `ir.c` and `timer.c`.

**IR Link** (`frontends/desktop/ir_link.h`/`.c`, Windows only) is the two-*process* half of this: it relays edges between two independent `pokketstation.exe` instances over a local named pipe (`IR Link` menu: Host Session / Connect / Disconnect). The two processes' IR clocks are never synchronized with each other, so edges relay as absolute host wall-clock microseconds (`GetSystemTimePreciseAsFileTime`) rather than raw cycle counts. Both processes run on the same machine, and each can read that same wall clock with no coordination. Each one converts to and from its own local IR timeline only at the point an edge crosses the pipe, using an offset that is held constant for the duration of a message and refreshed between messages (see "Known open questions" for why both halves of that matter). `psemu_reset`/`psemu_load_state` both wipe `ir_t`'s clock and any queued edges (like every other peripheral, on a full reset), so an active link is explicitly dropped any time either happens, to avoid a silent desync between the two instances.

## CLK_MODE

`0x0B000000`: bits 0-3 index a 16-entry CPU-frequency table. These are the exact, documented `PMFrequency`/`SetCpuSpeed` values, not a simple doubling ladder. Examples: mode 1 = 63488 Hz, not 65536 Hz; mode 7 = 3997696 Hz, not 4194304 Hz; mode 8 = 7995392 Hz, not 8388608 Hz; mode 5 to mode 6 steps by approximately 1.97x, not 2x.

Mode 0 = 32.768kHz, and this emulator treats it as the idle default. Modes 9-15 alias mode 8's rate. See `core/src/clk.c` for the full table.

A readback of `CLK_MODE` ORs in a "steady"/PLL-locked bit (`0x10`). This always reports stable, because mode changes are instantaneous in this emulator.

Confirmed via a real 20-million-instruction boot trace: real firmware never issues `CLK_MODE=0`. The first act of the real BIOS is `CLK_MODE=7`. Every subsequent write cycles only between modes 7, 4, and 3.

Official documentation describes mode `00h` as an invalid/reserved setting that hangs hardware, rather than giving it a frequency. This emulator's idle-default use of mode 0 is harmless in practice, because real firmware never triggers that code path.

**Tracks `CLK_MODE`:** overall CPU instruction throughput (`psemu_run`'s cycle budget), and the timers' count-down rate.

**Does not track `CLK_MODE`** (pinned to real elapsed time instead):

- RTC: runs from a separate oscillator.
- DAC: this emulator's audio resampling needs a fixed real-time output rate, regardless of the app's chosen CPU speed.

See `test_clk_mode_scales_run_speed`, `test_timer_scales_with_clk_mode`, `test_clk_mode_keeps_rtc_dac_on_real_time`.

## CLK control (0x0B000004): stop/standby

**Bit 0 of the second `CLK` register halts the CPU, and everything clocked from the same oscillator, until a button wakes it.** This is how a PocketStation sleeps.

**The behaviour is confirmed on real hardware. Which register produces it is inferred.** Keep those apart when reading what follows.

**Confirmed, by direct testing on a real retail unit:** a real commercial app, left completely idle, blanks the screen about 37 seconds after the last button press and puts the device to sleep. The next button press wakes it and the screen returns where it was. Any emulator has to stop the CPU somewhere for that to happen.

**Inferred:** that bit 0 of this register is what does it. The register is undocumented and this project has no way to probe it directly. The inference rests on two things.

First, the write sits at the end of an unmistakable power-down sequence, with nothing else it could plausibly mean:

| write | meaning |
|---|---|
| `IOP_STOP = 0x62` | sound and other IOP subsystems off |
| `INTC mask = 0x200` | RTC interrupt disabled |
| `LCD_MODE &= ~0x48` | `DISON` cleared - display off |
| `CLK control = 1` | this |

Second, modelling it as a stop reproduces the confirmed hardware behaviour exactly, end to end: the framebuffer goes to zero lit pixels at the stop, the CPU executes nothing until a button arrives, and the screen returns on the wake.

**Leaving the write inert is not a harmless simplification; it corrupts the app.** The app's idle countdown is only written back *after* its sleep call returns. A CPU that keeps running executes the short delay loop that follows the stop, which pumps the app's own tick, which re-reads the same expired countdown and calls sleep again. That recursion is unbounded, at 28 bytes of stack per level. The app has 388 bytes before its stack reaches its globals (its user stack starts at `0x800`; its globals are at a hardcoded `0x67C`). It overruns them, a record-array `STRB` overwrites a byte of a saved return address on the stack, and the following `POP {pc}` jumps into data. The CPU then drifts through non-code memory until it hits an unrecognized opcode - which is what the fault looks like from a crash report, several million instructions downstream of the actual cause.

**What stops and what does not.** Timer is clocked by the System Clock (see "Timers"), so it freezes here too. That part is essential rather than incidental: waking on any asserted interrupt is not a stop at all, because a running timer re-asserts within microseconds and the CPU never actually pauses. RTC keeps running on its own oscillator, which is what lets a sleeping device still know the time, and DAC keeps this emulator's own fixed resampling rate.

**Open questions, all needing a `pk_timing_bench` screen to settle.** Whether bit 0 of this register specifically is the stop, rather than some other write in the sequence. Whether the timers really do freeze (snapshot `TIMER0_COUNT` and `GetBcdTime` across a sleep: if the RTC advances seconds while Timer0 advances nothing, they freeze). Whether real hardware auto-clears the bit on wake - this emulator does, so software need not. And whether anything other than a button can wake it: repeat with `INTC_MASK = 0x1F`, buttons masked and a timer live, and see whether it wakes on its own. No app this project can drive reaches any of these cases.

See `test_clk_stop_halts_until_a_button_wakes_it`.

## Memory access timing

`arm7tdmi_step` returns the real wait-state cost of the instruction it just ran, not a flat value of 1. `psemu_run`'s cycle budget, and so the timers' count-down rate (see "CLK_MODE" above), is spent at this real per-instruction granularity, not a fixed unit per instruction.

Two cost tables drive this. Both come from the documented "Memory Access Time" reference material, confirmed against the raw source document rather than a rendered page (see below). Both are implemented in `memory.c`, in `psemu_region_fetch_cycles` and `psemu_region_data_cycles`:

- **Opcode fetch** (`psemu_bus_fetch16`/`psemu_bus_fetch32`, used only by `arm7tdmi_step`'s own fetch): WRAM costs 1 cycle, in ARM or Thumb state. FLASH (`FLASH1`/`FLASH2`, app code) costs 2 cycles in ARM state, 1 cycle in Thumb state. Thumb code fetches faster than ARM code from flash; this is a real cost difference, not just an effect of Thumb's smaller encoding.
- **Data read/write** (every other `psemu_bus_read*`/`write*` call): WRAM costs 1 cycle. `FLASH_CTRL` (the F_xxx bank-select, F_WAIT, F_SN registers) also costs 1 cycle; confirmed on real hardware, see below. Everything else (FLASH, BIOS, VRAM, I/O) costs 2 cycles. Confirmed: this cost does not depend on access width (8/16/32-bit) or on sequential vs. non-sequential access. This emulator charges the cost once per logical call, never once per byte.

On top of the per-access cost above, `arm7tdmi_add_cycles` charges two more kinds of cost that a bus access alone does not capture. These follow the standard ARM7TDMI instruction-class timing formulas (well-documented, not PocketStation-specific):

- Internal-only "I" cycles, with no bus transaction of their own: a register-specified shift, `LDR`/`LDM`'s fixed `+1I`, and `MUL`/`MLA`/`UMULL` (and similar)'s data-dependent extra cycles via the early-termination rule on `Rs`.
- Pipeline-refill fetches, whenever an instruction changes PC: branches, `BX`, a data-processing/`LDR`/`LDM` instruction targeting R15, exception entry. This emulator models this as 2 more opcode fetches at the new PC, in whichever ARM/Thumb state applies after the change.

See the call sites in `arm_exec.c` and `thumb_exec.c` for the per-opcode breakdown.

**Confirmed on real retail hardware: which `F_xxx` ports get WRAM's faster 1-cycle data-access rate.**

The documented table says only "WRAM (and some F_xxx ports)", without naming them. This project changed its guess twice before confirming the answer on real hardware:

1. First guess: all of `FLASH_CTRL` is fast. Based only on the documentation's phrasing.
2. Second guess: all of `FLASH_CTRL` is slow. Based on disassembling an independent third-party emulator's source (see below), which put every `FLASH_CTRL` address on the slow path.
3. Confirmed via real hardware: `FLASH_CTRL` gets the fast (WRAM) rate. `pk_timing_bench` (this project's homebrew app) ran a tight loop reading `FLASH_CTRL+0x100` (`F_BANK_VAL[0]`) 30000 times, back-to-back with an identical loop reading plain WRAM, on a real retail unit. Both loops returned the exact same elapsed-tick count.

Implemented in `psemu_region_data_cycles` (`core/src/memory.c`). Per this project's trust order (see the top of this file), real hardware overrides the earlier guess: the other emulator's source was real independent evidence, but real hardware is stronger evidence. See `docs/app-notes.md` for the full real-hardware result.

**A gap remains in the source documentation itself, not just in this project's knowledge. Two separately-confirmed hazards follow it:**

- **BIOS opcode-fetch cost is undocumented, even in the source material.** The documented "Memory Access Time for Opcode Fetch" table lists WRAM and FLASH numerically, but leaves BIOS as a bare `?`. The kernel executes constantly out of BIOS ROM, so this emulator cannot leave this cost unassigned.

  This emulator assumes BIOS matches FLASH's full rate and split: 2 cycles ARM, 1 cycle Thumb, not a flat 2 cycles for both. Two other parts of the same source document (not marked `?`) pair BIOS with FLASH rather than WRAM: the data-access table (a different table from the opcode one) lists `BIOS` at 2 cycles, in the same row as `VIRT/PHYS/XTRA_FLASH`; and the bus-width section states that FLASH and BIOS ROM allow only 16-bit and 32-bit reads, while RAM allows 8-bit, 16-bit, and 32-bit access.

  Real hardware now supports this assumption too, not just documentation inference. `pk_timing_bench` measured a real BIOS ARM-mode helper call against an identical WRAM-copied version: the BIOS call was noticeably slower, consistent with BIOS paying FLASH's 2-cycle ARM rate against WRAM's 1-cycle rate. The same test in Thumb mode came back nearly equal to its WRAM copy, consistent with BIOS's Thumb rate matching WRAM's 1-cycle rate (the same as FLASH's Thumb rate). Both results point the same direction as the documentation-based assumption.

  This measurement is not proof to the same standard as a direct BIOS-disassembly trace: loop control and timer-read overhead dilute the pure fetch-cost signal. It is real, independent, hardware-level support for an assumption that previously had none. See `docs/app-notes.md`.
- **FLASH's documented "readable only in 16/32-bit units" restriction is confirmed as a real hazard, not just a documentation note.** This emulator's `bus_read8_raw` and `flash1_read8` (`core/src/memory.c`/`flash.c`) service an 8-bit `LDRB` from FLASH1 with no restriction modeled. `pk_timing_bench` read a font table out of FLASH1 one byte at a time via `LDRB`, and got scrambled glyph data on real hardware. This confirms the documented restriction matters in practice. Switching to a word-aligned `LDR` plus an in-register shift, avoiding any 8-bit bus access to FLASH1, fixed it. See `docs/app-notes.md` and "LCD" above; VRAM has a similar, previously undocumented word-only restriction, found the same way.
- **`F_WAIT1`/`F_WAIT2`'s waitstate-control bits are not modeled as dynamically affecting timing.** The documentation describes `F_WAIT1` as automatically following the `CLK_MODE` speed band: `00000000h` for modes 0-7, `00000010h` for modes 8-15. It describes `F_WAIT2` bit 5 as toggling WRAM/F_xxx between 1 and 2 cycles. Most of `F_WAIT2`'s other bits are themselves marked speculative in the source document ("no effect? but that bit is used in some cases!"). No real BIOS or app trace this project has captured writes this register for waitstate-control purposes.

  Modeling a register toggle that has never been observed in use risks encoding a guess as fact. This emulator instead uses a fixed table (bit 5 = 0), consistent with this project's real-trace-over-documentation trust order (see the top of this file).

**Note for anyone re-verifying this section.** This project cross-checked the reference material against its raw source document, not an AI-summarized fetch of a rendered page; the AI-summarized version had silently dropped the BIOS "`?`" caveat above.

This project also checked the two guesses above against two other public register-reference and emulator-source repositories. Neither helped: one repository mirrors the same source text; the other stores `F_WAIT1`/`F_WAIT2` but never reads them back for timing, and runs every access at a flat per-clock rate with no per-region wait-state model.

A more useful check: disassembling an independent third-party PocketStation emulator, a self-described "experimental" Windows tool by the same author as a well-regarded cycle-accurate PS1 emulator. Its x86 memory-dispatch routines (separate 8/16/32-bit read/write paths, plus separate ARM/Thumb opcode-fetch decoders) compute cost from two accumulator cells, written identically at every call site. This implements only a flat two-tier model: `addr <= 0x1FFFFFF` (WRAM) = 1x, everything else = 2x, uniform across reads, writes, and both instruction sets.

This other emulator does not implement FLASH's documented ARM/Thumb opcode-fetch split at all; BIOS and FLASH1 share the exact same fetch continuation in both its ARM and Thumb decoders. This is why it cannot settle the BIOS-opcode-fetch question: it makes no ARM/Thumb distinction for any region, not just BIOS.

It does settle where `FLASH_CTRL` falls on its coarser fast/slow axis: every `0x06000000`-`0x063FF` address dispatches through the same `> 0x1FFFFFF` slow path as FLASH/BIOS, not through WRAM's fast path. This project treated that finding as the best available evidence for a time, until real hardware (see above) contradicted it directly. Treat this other emulator's finding as one independent implementor's guess at the same ambiguous documented wording. It was useful when nothing stronger was available; real hardware now overrides it on this specific question.

## DAC / audio

`0x0D800010`: `ctrl` (+0x0, bit 0 = enable), `data` (+0x4, bits 6-15 = a signed 10-bit `DACV` value, rescaled ×64 to a full `int16` sample range).

Real hardware has no square-wave/noise generator and no sound DMA channel. Software produces every tone by writing new `DACV` levels directly to `DAC_DATA` at audio rates, typically driven by the Timer1 IRQ.

`dac_tick` resamples the currently-held level (zero-order hold) into a ring buffer, at a fixed internal rate: `PSEMU_AUDIO_SAMPLE_RATE_HZ` (8000Hz), independent of `CLK_MODE`.

`PSEMU_ASSUMED_CPU_HZ` (1,056,000 = 33000×32) is the reference cycle rate this conversion is calibrated against. `psemu_run`'s own per-frame budget (33000 cycles at a 32Hz refresh) is calibrated against the same reference rate. Keep both values in sync if either changes.

Audio output also requires `IOP_STOP`/`IOP_START` bit 5 ("Sound Enable", `0x0D800004`/`0x0D800008`) to be open, in addition to `DAC_CTRL` bit 0. Both gates must be open for non-silent output.

`IOP_STOP` ORs bits into a shared mask; `IOP_START` ANDs them out. Real code writes these registers via single-byte stores, not always a full 32-bit store. Both registers apply each byte's effect immediately, rather than deferring to a full-word write. See `test_iop_sound_gate_mutes_dac`, `test_iop_stop_start_take_effect_via_single_byte_writes`.

### System sound volume setting (RAM `0x290`)

**The BIOS system menu's three-level sound setting is a single byte of RAM at `0x290`.** There is no hardware volume control to hold it: the DAC exposes only an enable bit and a `DACV` level, and the only other audio gate (`IOP` bit 5) is binary. The setting is therefore applied entirely in software, by scaling the `DACV` amplitude the BIOS writes.

| `0x290` | Menu setting | Beep amplitude |
|---|---|---|
| `0x00` | Loud | `DACV` -500..496 |
| `0x02` | Quiet | `DACV` -125..124 |
| `0x04` | Mute | no `DACV` writes at all |

Pressing Up on the sound-setting screen cycles `0x04` -> `0x02` -> `0x00` -> `0x04`. Every such change is written by BIOS `0x04002994`, the only writer ever observed.

**The BIOS clears this byte early in its boot, at `0x04000060`, ~instr #696.** It is read from four sites: `0x040032F4` and `0x04003020` during sound init (immediately around the `DAC_CTRL` enable at `0x0400304E`), `0x04003910` inside the tone loop, and `0x04002BC6` in the menu. Outside that boot-time clear it is only ever read, never written except by the menu's own writer at `0x04002994` — so the BIOS does treat it as already-present state, which on real hardware is what battery-backed SRAM provides. What is not established is whether real hardware skips the clear on a warm boot; this emulator only ever cold-boots, which is the same limitation recorded above for date/time.

This was originally written up here as "the BIOS never initializes this byte", which was wrong, and the mistake is worth keeping visible because it is easy to repeat. `volume_probe`'s `watch` mode detects writes by comparing the byte before and after each instruction, and it starts from a `psemu_reset`, where RAM is already all zeroes. A clear that writes `0x00` over `0x00` produces no difference to observe, so 12 million instructions of watching reported no writer. Pre-loading a non-zero value first (`volume_probe <bios> boot 04 0`) makes it visible immediately. **Any prev-vs-now memory watch is blind to a write that stores the value already there** — seed the watched location with something the code under test would not write.

**The setting is never committed to flash.** Byte-comparing the full 128KB card image across all three settings shows no change. It lives only in RAM.

Between the menu's three values, the byte behaves as a power-of-two attenuation on `DACV`: `0`, `1`, `2`, `3` give roughly full, half, quarter, and eighth amplitude, and `5` through `8` continue halving down to silence at `9` and above. `4` breaks that ramp and is special-cased to full silence, which is how the menu encodes Mute. Whether the underlying implementation is a shift or a small gain table is not disassembled; a poke of `0xFF` produces an amplitude that neither model predicts.

**Consequence for this emulator:** `psemu_reset` zeroes all of RAM, and the BIOS then clears the byte again during its own boot, so `0x290` reads `0x00` and the emulated PocketStation is at full volume unless something holds the byte against both.

A frontend cannot hold it by re-applying between frames, which is how every other RAM-backed setting here is held. The whole sequence — RAM clear at instr #696, sound init reading `0x290` at #14548, boot chime finished by #15405 — fits inside a single 33000-cycle frame, so the frame boundary never falls between the clear and the read. Re-applying per frame does silence everything *after* the boot chime, which is exactly what the original bug looked like: the setting appeared to work until the device was rebooted.

`psemu_set_volume_override` is what actually holds it. It writes the byte, makes it read-only to emulated code (one compare on the RAM write path in `bus_write8_raw`, nothing on the read or opcode-fetch path), and re-seeds it on every `psemu_reset` — standing in for the battery that holds it on real hardware. `psemu_clear_volume_override` hands the byte back to the BIOS sound menu. `psemu_set_volume` remains the plain unheld write. Verified against a real boot with `volume_probe <bios> boot <level> 2`: Mute produces zero `DACV` writes and the BIOS never even enables `DAC_CTRL`; Quiet produces the documented `-125..124`.

Save states preserve the override along with everything else, since `psemu_save_state` copies the whole `psemu_t`. The lock lives in `psemu_bus_t`, so adding it changed the state blob's size; the desktop frontend's quicksave version went to 2 to reject version-1 files.

Found empirically with `tools/volume_probe.c`: a full-RAM poke sweep over the boot beep (replayed from a save state per candidate byte) isolated `0x290` as the only address that scales amplitude while leaving the beep's timing and write count untouched; a desktop quicksave parked on the sound-setting screen then confirmed the three real values. The read sites came from `psemu_bus_read_trace_cb` (`core/src/memory.c`), a diagnostic hook that reports every bus read with its real PC. Reads are the half of RAM access that a snapshot-diff probe cannot see, and the sweep alone was not enough to find this. That hook is compiled in only for the `psemu_trace` library target, which the diagnostic tools link instead of `psemu`: it sits on the hottest path in the emulator, and measured about 20% slower on a fixed 6.5M instruction workload even with the callback left NULL. Frontends link plain `psemu` and are unaffected.

Note that `0x290` sits in the region the memory map above calls user RAM (`0x200`-`0x7FF`). The BIOS keeps a good deal of its own state there (`0x230`, `0x254`, `0x264`, `0x280`-`0x2A7`, `0x300`-`0x31F`, `0x3F0`, `0x410`), so that kernel/user split does not mark where BIOS-owned state ends. See "Known open questions" for what this implies about apps clobbering the setting.

## Diagnostics

`arm7tdmi_t` keeps a 256-entry ring buffer of the most recently executed `(pc, cpsr)` pairs, plus a monotonic instruction counter. Every step updates both, regardless of caller.

`psemu_write_crash_report` and `psemu_cpu_faulted` (public API) dump the full register state, the fault opcode and its real fetch address (if a fault occurred), and this trace.

The desktop frontend writes a timestamped `pokketstation_report_*.log` automatically on a CPU fault, and on demand via the **F12** hotkey. See `test_crash_report_contents`, `test_cpu_faulted_flag`, `test_faulted_cpu_stops_advancing`.

## Hardware ID (F_SN)

Each real unit carries a 32-bit serial number in `F_EXTRA` (`FLASH_CTRL+0x300`, 256 bytes): `F_SN_LO`/`F_SN_HI` (`+0x300`/`+0x302`, two 16-bit halves) and `F_CAL` (`+0x308`, LCD calibration). Real code reads `F_SN_LO`/`F_SN_HI` via two separate 16-bit `LDRH` instructions, never a single 32-bit `LDR`. Implemented in `core/src/flash.c`/`flash.h` (`flash_get_serial`/`flash_set_serial`). See `test_flash_serial_number_register_access`.

**Read**: real apps use `SWI 0Ah` (`FlashReadSerial`), or read `F_EXTRA` directly.

**Write**: `SWI 0Fh` (`FlashWriteSerial`) works only on the `061` BIOS revision (see "BIOS/kernel revisions" below). It hangs the CPU on the retail `110` revision.

The only working write path on retail hardware is a 3-step NOR-flash unlock sequence, at fixed physical addresses: `F_KEY2`=`0xFFAA`, `F_KEY1`=`0xFF55`, `F_KEY2`=`0xFFA0`. This sequence is followed by writes to physical `FLASH2` offset `0`/`2`/`8`, not to `F_EXTRA` (where the value is read from).

This is documented in official register documentation: *"At physical address 08000000h: `[8000000h]=new F_SN_LO value [8000002h]=new F_SN_HI value`"*. This project confirmed it by disassembling a real ID-editing homebrew's flash-write routine, and confirmed it working end-to-end on real retail hardware.

Implemented as a gated redirect in `flash_write8` (`core/src/flash.c`), not an unconditional address alias. This avoids misrouting any other legitimate write that happens to land on the same three offsets; address alone is not a safe-enough signal.

An `unlock_step` field on `flash_t` tracks progress through the 3-step unlock sequence, based only on which key address is hit next. This emulator does not validate the actual values written, matching how these addresses have always been treated as commands, not data.

Once armed, a write to physical offset `0`/`2`/`8` redirects to `F_SN_LO`/`F_SN_HI`/`F_CAL` instead of `flash->data[]`. The armed state persists across the 3 halfword writes a real header update performs, and disarms on the first write to any other offset.

One implementation pitfall found along the way: the state initially advanced on every byte of each key halfword, instead of once per halfword. This happened because `psemu_bus_write16` (like a real `STRH`) issues two separate 8-bit bus writes. Fixed by advancing the state only on the low byte.

See `test_flash_header_write_via_unlock_sequence`, `test_flash_header_write_requires_unlock_first`, `test_flash_header_write_disarms_after_unrelated_write`, `test_flash_header_write_requires_correct_key_order`.

**Verified end-to-end** against the real homebrew, a real BIOS, and a real button sequence (`tools/inspect.c`, `button_sim=9`): navigating the on-screen digit cursor to its last position, editing the digit, and committing the edit correctly updates `F_SN` through this gated redirect (`0x410000D3` becomes `0x410000D4` after a single edit).

This test also surfaced the FIQ delivery bug described in "Interrupt controller" above. The homebrew's post-write confirmation beep configures Timer2, a FIQ source. Before the FIQ fix, this beep produced silence. After the fix, it plays correctly: `DAC_CTRL`/`DAC_DATA` show continuously-varying activity immediately after the write commits, at Timer2's configured rate.

### Human-readable ID format

A real PocketStation prints its hardware ID on a sticker under the front cover: one ASCII letter followed by 8 decimal digits (user-reported from a real unit: `"A02374684"`). The letter is `F_SN`'s high byte. The 8 digits are its low 24 bits (max `16777215`), in decimal.

Chocobo World's rank calculation applies this same mask to `F_SN`: it reads the register via `SWI 0Ah`, masks off the high byte, and uses the last 3 decimal digits of the remainder as its rank-determining "ID" stat. Confirmed by disassembling a real copy of the game. The community-documented best rank is `211` (also FF8's Japanese release date, 2/11).

`psemu_parse_hardware_id` and `psemu_format_hardware_id` (`core/src/psemu.c`) accept and produce exactly 8 plain hex digits (`0-9`, `A-F`/`a-f`). This matches exactly what a real ID-editing homebrew displays and edits, and can represent every value the hardware allows. Confirmed via real-hardware testing: writing `"EEEEEEEE"` persists correctly, a value the letter-prefixed sticker form cannot represent, since `0xEE` is not an ASCII letter.

This parser does not accept the sticker form, on input or output. A persisted hardware-ID string (the `hardware_id=` line in `settings.cfg`) holds exactly the raw value, with nothing hidden or translated. A sticker-to-raw-value converter, if ever needed, belongs as a separate desktop-app feature. See `test_hardware_id_string_conversion`.

**Default value: `0x410000D3` (`"410000D3"` in hex form).** Its low 24 bits equal `211` in decimal, giving every fresh Chocobo World save the best rank out of the box, for the reasons above. This default is this emulator's own choice; real units ship with an arbitrary factory-assigned serial.

This value lives outside the ordinary 128KB card image, so it needs its own persistent store. The desktop frontend persists it (as an 8-hex-digit string) in `settings.cfg`, alongside its other preferences: BIOS path, color scheme, key bindings, and more. The libretro frontend has no such mechanism, and always uses the core default.

## BIOS/kernel revisions

Two documented BIOS/kernel ROM revisions exist, identified by ASCII tag strings baked into the ROM: the Core Kernel Version at BIOS offset `0x1DFC` (`"C061"` or `"C110"`), and the Japanese GUI Version at `0x3FFC` (`"J061"` or `"J110"`). Only revision `110` ever shipped in a retail unit.

The `061` dump in circulation is documented as a prototype-hardware dump that does not work correctly with some games, not a real retail BIOS. This project's own testing uses the real `110`-revision BIOS dump.

Both revisions are factory mask-ROM revisions, not a user- or field-applied patch. `BIOS_ROM` is genuine ROM, not the writable `FLASH` region, and no update mechanism (disc-based flasher, service program) is documented anywhere.

Revision `110` contains patches relative to `061`, while preserving the same SWI dispatch-table addresses. This makes it a binary-compatible bugfix revision, consistent with `061` predating retail release.

The confirmed difference between revisions: `SWI 0Fh` (`FlashWriteSerial`) works only on `061`. On `110`, this vector is padded with jump opcodes that hang the CPU in an endless loop; calling it on a `110` BIOS freezes the device, not merely fails.

A real ID-editing homebrew never calls `SWI 0Fh` anywhere in its code. Confirmed by exhaustively scanning its binary for the `SWI #0xF` opcode: zero hits. The binary has no runtime BIOS-version detection; it avoids the call unconditionally, because it was built that way from the start.

## Known open questions and unconfirmed behavior

- **Chocobo World event-screen crash (counter overflow): not fully confirmed.** A real, reproducible CPU fault occurred: a stale-`LR` wild branch into an orphaned Thumb `BL` half-instruction. Tracing found the cause: a command-dispatcher call with an out-of-range value (`0x200`). This value comes from a small RAM-resident counter (`0x332`), in the app's own private user-RAM state, not a documented kernel construct. This counter is consumed one at a time and is normally expected to stay within `0`-`0x13`.

  This crash appeared only after a long, deterministically-seeded pseudo-random button-mashing run: 1.3 billion instructions. This is well-supported, but not confirmed: no trace has found the counter's increment site, so whether the counter needs faster-than-human input to overflow (a latent bug in Chocobo World's own code, not this emulator) remains a hypothesis.

  If revisited: watch RAM `0x332` for writes across a fresh `button_sim=6` run in `tools/inspect.c`. Check whether increments are tied to button input (supports the hypothesis) or to a timer/frame tick (would instead point to a timing-accuracy bug in this emulator).
- **Unconfirmed: whether Chocobo World has in-game sound beyond a single ~17ms launch chime.** Over 200 million instructions of generic automated exploration, no DAC activity appeared after that one chime. This emulator's generic button-mashing exploration may not reach the gameplay states that gate further sound. Confirming this either way needs real interactive play, focused on deeper gameplay.
- **Unconfirmed: whether real hardware has a warm-boot path that skips the Jan-1999 clock reset, and what gates it.** See "Where the date/time settings actually live" above for the three inputs already ruled out (RTC contents, RAM shadows, card contents). The leading remaining suspect is `BATT_CTRL` (`0x0D800020`), which is stubbed to read 0 here: a real unit that can distinguish "battery still good" from "battery just replaced" would have somewhere to read that from, and this emulator would always look like the latter. If revisited, model `BATT_CTRL` with a non-zero read and re-run `tools/datetime_probe.c`'s `warm` mode. Note also that the warm runs left `RTC_DATE` at `0x009A0101`, whose year byte is not valid BCD, where the cold run lands cleanly on `0x00990101` - the `RTC_ADJUST` walk appears to overshoot when it starts from a year other than the power-on value, which is worth checking against `rtc.c`'s BCD carry independently of the warm-boot question.
- **Unconfirmed: whether a dispatched app can clobber the system sound volume setting.** The setting is a single byte at RAM `0x290` (see "System sound volume setting" above), which sits inside the `0x200`-`0x7FF` range the memory map calls user RAM. Outside its boot-time clear and its own menu writer, the BIOS never rewrites it, so an app that uses that byte as its own scratch would leave the user's choice silently changed until they set it again in the menu. Whether real apps actually reach it is untested; the check is to run the real apps in `testdata/` and watch `0x290` for writes from a FLASH1 PC (`tools/volume_probe.c`'s `watch` mode already reports the writing PC — seed the byte non-zero first, for the reason recorded under "System sound volume setting"). This also decides how much RAM a battery-backed-SRAM persistence feature would have to save and restore, and when. Note `psemu_set_volume_override` would make this moot by making the byte read-only to emulated code, but no frontend calls it any more - the desktop app's Volume Override menu was removed in favour of an application-level output volume, so in normal use nothing stops an app reaching the byte. The core API and its tests remain, and `tools/volume_probe.c` still exercises it.
- **IR (`0x0C800000`+)**: see "IR / IR Link" above for the full model, including what the external reference cited there does and does not confirm. The `BFLT` debounce window is inferred, not confirmed. The same is true of the receive-side meaning of `IRDA_DATA` bit0. Neither is documented externally. No trace in this project's test corpus (over 200 million instructions) touched these registers. There is no second real PocketStation available to validate a modeled protocol against. `IRDA_MISC` (`+0xC`) is an unknown or reserved register externally too. This emulator stubs it. It reads 0 and ignores writes. It does not guess at behavior that neither source documents.
- **Mostly resolved: a real IR-using app now completes a full bidirectional transfer in this emulator.** A verified two-instance exchange works (see the hardware-ID check further down this bullet). Two real emulator bugs were the blockers, both found by disassembling the app rather than by black-box probing: an inverted receive-line polarity, and a timer that did not load its counter when armed. The long investigation recorded below chased a different theory - a ~1% transmit-pulse shortfall - and is kept because its real-hardware measurements stand on their own and because the reasoning went wrong in instructive ways.

  The original framing was: the app arms Timer2 with its nominal pulse unit minus a hardcoded 184 (1200 − 184 = 1016), its receive handler accepts a sync pulse only within `4×unit ± unit/2` = `[4200, 5400]` Timer2 ticks, and this emulator produced 4162 - short by 38, just outside the window. That measurement was real, but it was not why transfers failed: with the polarity bug in place the handler was measuring the wrong interval entirely (the short inter-pulse gap, not the sync burst), so no value of the transmit timing could ever have satisfied it. `IR_TX_FALL_STRETCH_CYCLES` remains in `ir.c` as a documented concession, and sweeping it across 0-380 changes nothing about whether a transfer succeeds.

  Six candidate explanations have been measured on real hardware via `pk_timing_bench`, and **all six are ruled out**:
  - *Timer period semantics* (screen 6): real hardware takes P+1 ticks per period, not P. Real, and now fixed in `timer.c`. It is worth only 1 tick of the shortfall.
  - *Interrupt entry/dispatch cost* (screen 7): measured at ~98 raw cycles on real hardware versus ~96 in this emulator, a 2.2% difference. The 184-tick compensation would have required ~368. This was the leading hypothesis and it is wrong.
  - *Expiry-to-re-arm latency, bare handler, over IRQ* (screen 8): a real app's transmit handler re-arms Timer2 on every interrupt, instead of letting it free-run. The original theory: a re-armed timer's next period does not start until its handler reaches the re-arm, so dispatch latency adds to every period, and that is what the 184 budgets for. Real hardware measures this latency at **0 ticks**, not 184.
  - *`IRDA_DATA`'s own MMIO write cost* (screen 9): the real transmit handler's one hot-path write is to `IRDA_DATA`, the LED bit. Real hardware measures this write's cost as bit-for-bit identical to this emulator's own figure (`0x2BF2` test, `0x2849` control, matching exactly). This emulator's generic 2-cycle "I/O" charge for `IRDA_DATA` is already correct.
  - *Expiry-to-re-arm latency, bare handler, over FIQ* (screen 10): a disassembled trace of the real transmit handler confirms it runs on FIQ, not IRQ (CPSR mode `0x11` at the exact point of its `IRDA_DATA` write), so screen 8's IRQ result was a stand-in. Screen 10 repeats screen 8's exact method on the real FIQ-routed timer. Real hardware returns the identical raw reading screen 8 got for IRQ (`0x0FE4`/`0x03F8`, 0 ticks).
  - *Expiry-to-write latency, full realistic dispatch, over FIQ* (screen 11): a disassembled trace of the real transmit handler shows its actual dispatch is not bare. It acknowledges its own interrupt sources, calls through a jump table indexed by INTC bit, calls a nested subroutine that reads a state flag, then calls a second subroutine that crosses from ARM to Thumb through an interworking `BX`, before it re-arms Timer2. Screen 11 reproduces that shape as original homebrew code. Real hardware returns, for a third time, the identical raw reading screens 8 and 10 got (`0x0FE4`/`0x03F8`, 0 ticks) - even with the full realistic dispatch chain in the handler.

  Building screen 10 also surfaced a real BIOS fact worth keeping: the IRQ and FIQ vector handlers each read their app-registered callback from a different fixed RAM slot (`0xFC` vs `0x100`, `SWI 1` with `r0=1` vs `r0=2`). See "Interrupt controller" above. Reusing the IRQ slot for a FIQ source hangs the device; this was caught in the emulator before ever reaching real hardware.

  **Screen 11's result does more than rule out a sixth candidate. It falsifies the theory screens 8, 10, and 11 were all built to test.** Three separate real-hardware measurements, with dispatch complexity ranging from a bare two-instruction re-arm to a full multi-call chain crossing ARM/Thumb, all show exactly 0 ticks of added latency. That result does not depend on how much work the handler does before its re-arm write, which the "re-arm accumulates dispatch latency" theory predicted it would. The simplest explanation left standing: Timer2 auto-reloads in hardware the instant it expires, independent of when software services the interrupt. The re-arm write only needs to land before the *next* natural expiry to take effect for the following period, which at a ~1017-tick period against a dispatch chain measured at well under 200 ticks, it always does with enormous margin. **The 184-tick figure the real app compensates for is very unlikely to be dispatch-latency compensation at all.** The original disassembly that produced the "nominal unit minus a hardcoded 184 (1200 − 184 = 1016)" reading is worth re-examining for an alternative meaning: a protocol constant unrelated to timing, a different arithmetic relationship than assumed, or something else in the encoding this project has not yet considered.

  Separately, and still true regardless of the above: this emulator's own execution of the real transmit handler's actual dispatch chain, measured directly via `tools/ir_probe.c` rather than hand-derived, costs 128-160 Timer2 ticks in this emulator (across 658 real writes from an actual Chocobo World transmit burst; limited by Timer0's own 32-cycle sampling granularity). That figure was originally read as "closing most of the 184-tick gap." Screen 11's real-hardware result means that framing no longer holds: real hardware shows this same shape of dispatch costs 0 added ticks, so a 128-160-tick emulator figure for comparable work is now better read as a *separate, real emulator-side cost inaccuracy* (this emulator overcharges something in a nested-call/interworking dispatch chain that real hardware does not), not as evidence toward explaining the app's own 184-tick budget.

  **A full disassembly of the real transmit handler's state machine (not just its dispatch chain) shows the "184" framing was wrong about the mechanism, not just the cause.** Timer2's period does not directly encode one pulse. It paces a repeating quantum clock: each FIQ reads one bit from a data buffer (`field+0x14`, MSB-first) and holds the LED for 1 quantum (bit 0) or 2 quanta (bit 1), separated by a 1-quantum gap - the documented "long is twice as long as short" shape, built up over many interrupts, not set by a single period value. `unit` (`field+0x20` = 1200) is not the quantum length; it only appears in the receive side's sync-pulse formula (`4×unit ± unit/2`), and sync itself is 4 back-to-back ON quanta. The per-quantum shortfall (armed 1016 against an intended 1200) compounds across those 4 quanta, which is why the sync pulse specifically was the first thing to fail.

  Given three separate real-hardware measurements already rule out any CPU or interrupt-dispatch cause for that shortfall, the leading explanation is real transceiver physics this emulator does not model as an analog signal: LED turn-off decay, and a receiving photodiode's own response/AGC settling, both add real time before a real receiver would perceive a pulse as "ended". This project has no way to reproduce that physics, and does not try to. Instead, `core/src/ir.c` now delays only the falling edge of a transmitted pulse by a fixed, documented amount (`IR_TX_FALL_STRETCH_CYCLES`), tuned against this emulator's own measured shortfall rather than against the real app's own "184" constant (an earlier attempt at the literal value reordered the TX edge queue outright - the real gap between pulses is far smaller than 184 Timer2 ticks). `tools/ir_probe.c`'s own sync-window check, which mechanically reproduces what a real app's receive handler checks, now passes: the previously-rejected sync pulse (measured at ~4162-4174 against a `[4200, 5400]` floor before this fix) now lands inside the window.

  **A byte-for-byte comparison of the transmitter's and receiver's data buffers (`tools/ir_probe.c`) was initially read as showing a 37-of-41-byte-correct decode. That reading was wrong, and has been retracted.** The two instances load the same save file, so their buffers start byte-identical before any transfer is attempted at all (confirmed directly: running the same harness with no button script, so zero edges are ever relayed, still reports 41 of 41 bytes "matched"). Watching the receiver's buffer for writes across a full run with real transmission (658 edges relayed) shows it is never written to, even once. The transmitter's own buffer does change, early, before transmission begins - it is being constructed, not received into. The apparent "37 of 41 match" was this constructed buffer compared against the receiver's untouched, stale, pre-loaded copy; the 4 "mismatches" are simply the positions where that stale copy happened to already differ.

  The receiver's own state field (`field+0x28`) shows why: across the full run it oscillates between two sync-detection states (seen 65+ times) and only leaves that loop in the final slice, jumping straight to a sixth state without ever passing through the intermediate bit-accumulation states a successful decode would need. The receiver never locks sync in this harness. Two separate, confirmed causes explain that, and only one is fixed:

  **Cause 1 (fixed): the receive edge queue was far too small.** `core/src/ir.h`'s `IR_EDGE_QUEUE_CAPACITY` was 64. A real 41-byte Chocobo World burst produces 658 edges. Counting every edge's path through the receive side directly (not inferred) showed only 64 of 658 ever reached the debounce/decode logic; the other 594 were silently dropped by the queue's own full-queue guard, before decode ever saw them. Raising the capacity to 4096 (a documented, generous multiple of one message's worth) brought that to 658 of 658 reaching decode with zero drops, confirmed the same way. This was a real, fixable emulator bug, unrelated to timing accuracy.

  **A large apparent clock skew between the two instances is a harness artifact, not an emulator bug - ruled out.** `tools/ir_probe.c` steps both instances with the same per-slice budget, and `psemu_run`'s budget is in reference-rate cycles converted to real seconds, so equal budgets should mean equal real time regardless of each instance's own app-selected `CLK_MODE`. The catch is that `psemu_run`'s loop always executes at least one instruction. When one instruction's real duration exceeds the whole slice budget, that call overshoots badly. At `slice_cycles=4` (a 3.79us budget) an instruction at the slower instance's `~254KHz` takes ~11.8us, a 3x overshoot every call, while the other instance at `~4MHz` fits several instructions per budget and barely overshoots. That asymmetry, not any clock modeling, produced a ~957000-cycle divergence. Sweeping the slice size confirms it directly: the skew falls to ~78600 at slice 64, ~20700 at 256, and ~8200 at 1024, and at slice 1024 the total elapsed time comes out at exactly 400x33000 cycles, the correct value, versus a 67% overshoot at slice 4. Fine slice granularity therefore buys relay precision at the cost of timing accuracy, and this is a property of the diagnostic harness only - the desktop frontend relays once per frame and never slices this finely.

  **Resolved by disassembly: two real emulator bugs, and a diagnostic that had been measuring the wrong memory entirely.** Working from the addresses actually observed executing, rather than from guessed field offsets, produced three findings:

  - *`field+0x14` is a table of buffer pointers, not a buffer.* The receive bit-store resolves its target as `table[(field+0x27) - 1]`. Every byte comparison run against `field+0x14` directly, including the retracted "37 of 41" result, was comparing pointers and neighbouring state rather than message content. That is also why one such dump contained `0x410000D3`, the hardware ID, where payload was assumed.
  - *The demodulated receive line is active low, and this emulator had the polarity backwards.* The handler reads the live level out of INTC STATUS bit 12, compares it against an expected level it keeps in its own state, and arms expecting `0` before any carrier has arrived. With carrier-present reported as 1, the handler rejected the edge that begins a sync burst, locked onto the ~657-tick inter-pulse gap instead of the ~4500-tick burst, and could never satisfy its own `|delta - 4*unit| <= unit/2` test. Real IR demodulator receivers are active low too, so this is also the physically expected behavior. Fixed in `ir.c`, with `rx_level` still stored in physical terms and the inversion applied where software observes it.
  - *Arming a timer must load its counter from PERIOD.* The app's timer-set helper disables the timer, writes PERIOD, then re-enables it, and never writes COUNT. Its receive handler re-arms Timer2 between pulses and reads elapsed time as `armed period - current count`. Without a load on the enable edge the counter kept descending from a stale free-running value, so every interval measured after sync was wrong. Fixed in `timer.c`. This does not disturb the P+1 semantics confirmed on real hardware by screen 6, and `pk_timing_bench` screens 8/10/11 return byte-identical readings before and after the change (31.0/39.0/71.8 ticks), so the separately documented emulator-vs-hardware gap on those screens is untouched.

  **With both fixes in, a full bidirectional IR exchange completes and is verified.** Proving it needed a check that could not pass by coincidence, because byte-for-byte buffer comparison had already misled three times: both instances run the same app from the same save file, so their buffers start identical and "match" before a single edge is relayed; the buffers are cleared and reused straight after a transfer, so an end-of-run read finds only zeroes; and `field+0x14` is a pointer table, so comparing it compared pointers. The check that works is to give the two instances **distinct hardware IDs**. A real IR message carries the sender's ID, and neither instance can learn the other's by any route except the link. Running A with `0xAA1111AA` and B with `0xBB2222BB`, each side ends up holding the other's ID at `0x0000034C`, and the reverse direction now carries traffic too (`A->B 980`, `B->A 658`, where B previously transmitted nothing at all). The same run with no button input reports neither ID present and zero edges, so the check has a working negative control.

  **Relay granularity alone does not break a transfer, but relaying with no playout delay does.** `tools/ir_probe.c` used to relay edge timestamps as-is and described that as "the most favorable case possible". It is the opposite: the receiver gets a batch of edges already timestamped in its past, releases them all at once, and every interval it measures collapses. At the desktop frontend's own once-per-frame relay this fails outright, while one frame of playout delay completes the exchange in both directions. The desktop frontend already had `IR_LINK_PLAYOUT_DELAY_US` for exactly this reason. The probe was the unfaithful one, and now defaults to the frontend's own delay. With that default the transfer verifies at every relay granularity tested, from 4 cycles up to a full 33000-cycle frame.

  **The two-process transport had three further faults of its own, none of which any single-process test could reach.** The core model being correct turned out to say very little about whether two real `pokketstation.exe` windows could exchange anything, and each fault was found only by driving a whole message through the real pipe rather than a single edge:
  - *Throughput.* `pump_connected` completed one read and one write per call, and it is called once per rendered frame, so the transport carried about one edge per frame in each direction against the ~65 per frame a real burst produces. The write queue then overflowed and dropped the rest, which is unrecoverable when pulse width is the data. It was sized 64, the same number and the same failure as `IR_EDGE_QUEUE_CAPACITY` in core. Both directions now drain until the pipe is empty or full, and the queue and pipe buffers hold whole messages.
  - *Clock-offset sampling.* The wall-to-core offset was recomputed on every conversion, but an edge's timestamp records when it was produced. Sampling the offset later mixes two different moments, and since emulated and wall time never advance at the same rate, edges from different frames were shifted by different amounts - distorting the very spacing that encodes each bit.
  - *Clock drift between processes, which is the one that only two processes show.* Each instance advances its emulated clock by exactly one frame of cycles per rendered frame, but a real frame takes longer in wall-clock terms, by an amount that differs per process. Latching the offset once per connection fixes the spacing problem above but lets that drift accumulate without bound: measured between two real processes, one side saw *every* arriving edge land about 200ms in its own past, released all 658 at once, and decoded nothing, while the other direction had ~330ms of margin and worked. The offset is now re-latched only after the link has been quiet long enough that no message can be in flight, so it is constant within a message and current between messages. Note that two endpoints inside one process drift *identically* and therefore cancel, which is exactly why every single-process check passed while two real windows failed.

  The playout delay was also raised from 100ms to 250ms: drift across one message consumed about 85ms of margin, leaving only ~16ms, which is thin on a slower or busier machine. Measured margin is now ~174ms.

  `ir_link_t` carries always-on counters for edges sent, received, dropped, and arriving too late to place, and the connected status line reports them in the window title. Telling "the peer sent nothing" from "we dropped it" from "it arrived too late to use" required new instrumentation every single time, and the last of those three is invisible by any other means - the link looks perfectly healthy while decoding nothing. **Confirmed working between two real `pokketstation.exe` instances**, which is the only test that ever mattered here.

  **The transmit-stretch constant is not involved in any of this.** Sweeping `IR_TX_FALL_STRETCH_CYCLES` across 0, 100, 200, 300 and 380, before the two fixes above, changed only whether the sync measurement landed in the app's acceptance window; every value, including 0, left the receiver equally stuck and its buffer equally unwritten. The constant is not the blocker.

  One inference drawn from that sweep was wrong and is corrected here. The sweep prompted a note that `field+0x28` could not be a receive state machine, on the grounds that it changed about once per delivered edge. The disassembly shows it is exactly a receive state machine: 1 waits for the first qualifying edge, 2 measures and tests the sync burst, 3 and 4 alternate to measure each gap and each pulse (3 accepting `|m + 224 - unit| <= unit/2`, 4 decoding `|m - 224 - unit| <= unit/2` as a 0 bit and `|m - 224 - 2*unit| <= unit/2` as a 1 bit), and 6 is reached when the transfer ends. Its roughly-one-change-per-edge behavior was the 1-to-2-and-back failure loop described originally, not evidence against a state machine. The earlier description was right; the correction was wrong.

  Also still true, and unexplained: the receiving instance never transmits anything back (0 edges in the reverse direction across every run), so if the real protocol expects an acknowledgement or handshake from the receiving side, nothing in these runs would have satisfied it.
- **`F_BANK_VAL` mapping multiple physical blocks to the same virtual slot** has explicitly undocumented real hardware behavior (the documentation itself says "maybe the data becomes ANDed together"). `flash_resolve_physical_bank` returns the lowest-indexed matching physical block in this case. This is a reasonable choice among the documented unknowns, but not a confirmed-correct model.
- **`FLASH_CTRL+0`'s "always read back bit 0 = 1" behavior** is necessary in practice: it unblocks a real BIOS busy-wait. It does not cleanly correspond to the official `REGRemap` register's documented `GENREM`/`FLASHVIR` bit semantics. Do not assume this behavior is spec-accurate.
- **The real BIOS-mirrored-at-address-0 plus `GENREM` remap sequence** (BIOS ROM aliased to `0x00000000` until the kernel remaps RAM in) is not modeled. `psemu_reset` starts the CPU directly at `PSEMU_BIOS_BASE`, skipping the pre-remap phase entirely. This is a deliberate simplification, not an oversight; it has shown no behavioral cost across 150 million traced instructions.
- **`BATT_CTRL`** (`0x0D800020`, low-voltage detection) is unmodeled: reads return 0, writes are no-ops. No emulator behavior depends on battery-level sensing, since this emulator has no actual battery.
- **Per-instruction cycle timing follows the documented memory-access-time table and standard ARM7TDMI instruction-class formulas** (see "Memory access timing" above), not a flat 1-cycle-per-instruction approximation. Real hardware now confirms which `F_xxx` ports get WRAM's faster data rate: `FLASH_CTRL` gets the fast rate (see "Memory access timing"). One gap remains, because the source documentation itself gives no answer: BIOS opcode-fetch cost. This emulator assumes it follows FLASH's full ARM/Thumb split; a real-hardware measurement now supports this in the same direction, though not to disassembly-trace standard (see "Memory access timing"). `F_WAIT1`/`F_WAIT2`'s waitstate-control bits are not modeled as dynamically changing timing, since no real trace has ever shown them written for that purpose.

## Licensing note

This project is licensed GPLv3 ([LICENSE](../LICENSE)). BSD-3 code may be referenced or adapted into a GPLv3 project with attribution; that direction is compatible.

Do not copy code from another open-source implementation that has no license. Do not use other closed-source documentation beyond documentation-level facts; none of these other implementations make their code available anyway.
