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
| `0x0B000000`+ | 0x8 | `CLK_MODE` - CPU/timer clock speed control. |
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

**Inferred: Timer0's `count` register is effectively 16-bit, not the full 32-bit free-running counter this emulator models.** This emulator's `timer->timers[i].count` and `period` fields (`core/src/timer.c`) are plain `uint32_t`.

Found via `pk_timing_bench` (this project's homebrew timing-benchmark app):

- Every raw `count` snapshot captured on a real unit had its upper 16 bits at zero.
- A measurement loop wrote `period`/`control` once, then read `count` before and after a long loop with no reconfiguration in between. When the loop ran long enough to accumulate more than 65536 raw ticks, the *after* reading came back numerically larger than the *before* reading. The only explanation: the counter wrapped past zero and reloaded partway through the loop, at a 16-bit boundary rather than a 32-bit one.

This is inferred from real-hardware readings, not confirmed via BIOS disassembly like most facts in this section. Treat the 16-bit specifics as the current best explanation, not settled fact. The "before/after crossed a wrap boundary" observation itself is a direct, repeatable real-hardware result.

See `docs/app-notes.md`'s timing-benchmark writeup for the full before/after data.

## RTC

`0x0B800000`: `mode` bit 0 (`PRGSEL`) selects the RTC's operating mode:

- Run mode (`0`): ticks at 1Hz, auto-advances the clock.
- Program/pause mode (`1`): ticks at approximately 4096Hz, does not auto-advance. This lets a manual adjust-write step one field without the clock moving underneath it.

`mode` bits 1-3 (`CNTSEL`) select which BCD field a `control`/`RTC_ADJUST` write adjusts.

Auto-advance cascades in this order: seconds, minutes, hours, day-of-week. It does not cascade into `date` on a day rollover. No documentation confirms or denies this gap; this emulator inherits it rather than confirming it as correct.

Real hardware power-on-reset values: `RTCClock = 0x04000000` (day-of-week BCD 4, 00:00:00), `RTCCalendar = 0x00980101` (1998-01-01). `RTC_DATE` bits 24-31 are an unused, unidentified field, not a "year-high"/century byte. The real century value lives in battery-backed kernel RAM, and only the `GetBcdDate` SWI exposes it.

RTC ticks at a fixed 1Hz in Run mode, regardless of `CLK_MODE`. It runs from a separate oscillator, independent of the CPU clock.

The BIOS resets the clock to Jan 1 1999 in a documented condition Sony calls "The RTC Problem", a software workaround for inaccurate clock hardware. This reset is a software action, performed via the normal `RTC_ADJUST` mechanism. This emulator's own reset state does not build in this behavior.

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

- Writes to `IRDA_DATA`'s LED bit, while actually in an emitting state (`IFMODE`=transmit, `STDBY`=active, `BGEN`=carrier enabled), enqueue a timestamped edge (level + this `ir_t`'s own local monotonic clock) onto a TX queue. Leaving the emitting state (standby, receive, or carrier disabled) forces one final "LED off" edge, so the LED can never appear stuck on to whatever is downstream.
- `ir_tick` (called once per CPU step from `psemu_run`, alongside `timer_tick`/`rtc_tick`/`dac_tick`) advances that local clock and resolves any RX-queue edges now due, applying a `BFLT` glitch-filter debounce and calling `intc_set_line(intc, INT_IRDA, 1)` on a qualifying edge while actively receiving (`IFMODE`=receive, `STDBY`=active) - dropped otherwise, the same way a real half-duplex transceiver simply doesn't see a pulse while transmitting.
- `psemu_ir_pop_tx_edge`/`psemu_ir_push_rx_edge`/`psemu_ir_get_clock_us` (`psemu.h`) expose this as a pull/push edge queue, the same shape `psemu_get_audio_samples` already uses to let a frontend drive real I/O without core knowing about that I/O's transport. Core has no networking code and never will; timestamps only cross this API boundary in real microseconds (converted from the internal cycle-unit clock), everywhere else core stays in the same reference-rate cycle units every other peripheral already ticks in.

**Unconfirmed/inferred, flagged in `ir.h`'s own comments the same way as every other unconfirmed fact in this document:**

- The `BFLT` debounce window (~2 carrier periods, ~50us) is not documented externally. Over 200 million traced instructions in this project, nothing touched these registers. There is no real-hardware measurement to check this window against.
- `IRDA_DATA` reflecting the live demodulated level on read while in receive mode. No documentation this project found describes the receive-side meaning of this bit. Only the transmit-side ("LED") meaning is documented.
- Real IR pulse-length measurement (reading Timer2's live counter, reload `0xFFFFh`, from the `INT_IRDA` handler) is a real BIOS/software technique this emulator does not need to special-case: Timer2 already ticks independently every step regardless of IR state, so a real ISR reading it during an `INT_IRDA` handler already sees a plausible value with no extra coupling required between `ir.c` and `timer.c`.

**IR Link** (`frontends/desktop/ir_link.h`/`.c`, Windows only) is the two-*process* half of this: it relays edges between two independent `pokketstation.exe` instances over a local named pipe (`IR Link` menu: Host Session / Connect / Disconnect). The two processes' IR clocks are never synchronized with each other, so edges relay as absolute host wall-clock microseconds (`GetSystemTimePreciseAsFileTime`) rather than raw cycle counts. Both processes run on the same machine, and each can read that same wall clock with no coordination. Each one converts to and from its own local IR timeline only at the point an edge crosses the pipe. `psemu_reset`/`psemu_load_state` both wipe `ir_t`'s clock and any queued edges (like every other peripheral, on a full reset), so an active link is explicitly dropped any time either happens, to avoid a silent desync between the two instances.

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
- **IR (`0x0C800000`+)**: see "IR / IR Link" above for the full model, including what the external reference cited there does and does not confirm. The `BFLT` debounce window is inferred, not confirmed. The same is true of the receive-side meaning of `IRDA_DATA` bit0. Neither is documented externally. No trace in this project's test corpus (over 200 million instructions) touched these registers. There is no second real PocketStation available to validate a modeled protocol against. `IRDA_MISC` (`+0xC`) is an unknown or reserved register externally too. This emulator stubs it. It reads 0 and ignores writes. It does not guess at behavior that neither source documents.
- **Unresolved: a real IR-using app's transmitted pulses come out ~1% too short in this emulator, and it is not yet known why.** The app arms Timer2 with its nominal pulse unit minus a hardcoded 184 (1200 − 184 = 1016), and its receive handler accepts a sync pulse only within `4×unit ± unit/2`, i.e. `[4200, 5400]` Timer2 ticks. This emulator produces 4162. That is short by 38, just outside the window, so no transfer ever decodes.

  Five candidate explanations have been measured on real hardware via `pk_timing_bench`, and **all five are ruled out**:
  - *Timer period semantics* (screen 6): real hardware takes P+1 ticks per period, not P. Real, and now fixed in `timer.c`. It is worth only 1 tick of the shortfall.
  - *Interrupt entry/dispatch cost* (screen 7): measured at ~98 raw cycles on real hardware versus ~96 in this emulator, a 2.2% difference. The 184-tick compensation would have required ~368. This was the leading hypothesis and it is wrong.
  - *Expiry-to-re-arm latency, over IRQ* (screen 8): a real app's transmit handler re-arms Timer2 on every interrupt, instead of letting it free-run. A free-running timer's period holds however late its handler runs, so latency cancels out. A re-armed timer's next period does not start until its handler reaches the re-arm, so latency adds to every period. That is what the 184 budgets for. Real hardware measures this latency at **0 ticks**, not 184. This emulator currently measures 31 ticks for the same experiment, closer to the hypothesis than real hardware is.
  - *`IRDA_DATA`'s own MMIO write cost* (screen 9): the real transmit handler's one hot-path write is to `IRDA_DATA`, the LED bit. Real hardware measures this write's cost as bit-for-bit identical to this emulator's own figure (`0x2BF2` test, `0x2849` control, matching exactly). This emulator's generic 2-cycle "I/O" charge for `IRDA_DATA` is already correct.
  - *Expiry-to-re-arm latency, over FIQ* (screen 10): screen 8 measured re-arm latency over IRQ (Timer1), a stand-in, because Timer2 is actually FIQ-routed and screen 10's FIQ-registration mechanism did not exist yet. A disassembled trace of the real transmit handler confirms it runs on FIQ, not IRQ (CPSR mode `0x11` at the exact point of its `IRDA_DATA` write). Screen 10 repeats screen 8's exact method on the real FIQ-routed timer. Real hardware returns the identical raw reading screen 8 got for IRQ (`0x0FE4`/`0x03F8`, 0 ticks). FIQ costs the same as IRQ on real hardware; this emulator's own 39-tick figure for the FIQ case (versus screen 8's 31 for IRQ) is a small emulator inaccuracy, not evidence of FIQ-specific overhead.

  Building screen 10 also surfaced a real BIOS fact worth keeping: the IRQ and FIQ vector handlers each read their app-registered callback from a different fixed RAM slot (`0xFC` vs `0x100`, `SWI 1` with `r0=1` vs `r0=2`). See "Interrupt controller" above. Reusing the IRQ slot for a FIQ source hangs the device; this was caught in the emulator before ever reaching real hardware.

  So the remaining ~1% is not a generic cost. What is *not* in doubt: bulk instruction timing (screens 1-4 match hardware to ±1 tick), timer periods, interrupt entry cost, `IRDA_DATA`'s own write cost, and re-arm latency over both IRQ and FIQ. Every generic memory-access and interrupt-path cost this project can think of to measure now matches real hardware, or undershoots it, never overshoots it enough to explain 184 ticks. Whatever is missing must be specific to the real transmit handler's own instruction sequence between a Timer2 expiry and its `IRDA_DATA` write: work that sequence does that none of screens 1-10's synthetic loops or handlers reproduce.

  **This emulator's own execution of that real sequence, measured directly rather than hand-derived, gets much closer to 184 than any synthetic test did.** `tools/ir_probe.c` was temporarily instrumented to snapshot Timer0 at FIQ entry (filtered to genuine Timer2-sourced entries, via `INT_FIQ_MASK`) and again at the matching `IRDA_DATA` write, using Timer0 as the stopwatch, the same technique every `pk_timing_bench` screen already uses - this is the emulator's own authoritative cost model at work, not a hand re-derivation of ~90 instructions that would risk arithmetic error. Across 658 real writes from an actual Chocobo World transmit burst, the steady-state bulk of them (631 of 658, ~96%) cluster at 8-10 Timer0 ticks, i.e. **128-160 Timer2 ticks** (limited by Timer0's own 32-cycle sampling granularity; likely one true value inside that range). That is most of the 184-tick budget, not the ~31-39 ticks screens 8 and 10's synthetic minimal handlers measured. A smaller cluster (23 of 658, ~3.5%) measures ~1184 Timer2 ticks instead - more than a full Timer2 period, and not yet explained; it does not affect the steady-state figure above, which is what the bulk of a real transmission actually runs at.

  This does not close the question. It reframes it: real dispatch through BIOS FIQ entry, an app-side jump-table lookup (`0x02005780`, indexed directly by interrupt bit, not a linear scan), a nested subroutine call, and an `ARM`-to-`Thumb` interworking trampoline (the re-arm call) already accounts for most of the gap in this emulator's own model. The remaining ~24-56 ticks, and the unexplained ~1184-tick minority cluster, are where to look next.
- **`F_BANK_VAL` mapping multiple physical blocks to the same virtual slot** has explicitly undocumented real hardware behavior (the documentation itself says "maybe the data becomes ANDed together"). `flash_resolve_physical_bank` returns the lowest-indexed matching physical block in this case. This is a reasonable choice among the documented unknowns, but not a confirmed-correct model.
- **`FLASH_CTRL+0`'s "always read back bit 0 = 1" behavior** is necessary in practice: it unblocks a real BIOS busy-wait. It does not cleanly correspond to the official `REGRemap` register's documented `GENREM`/`FLASHVIR` bit semantics. Do not assume this behavior is spec-accurate.
- **The real BIOS-mirrored-at-address-0 plus `GENREM` remap sequence** (BIOS ROM aliased to `0x00000000` until the kernel remaps RAM in) is not modeled. `psemu_reset` starts the CPU directly at `PSEMU_BIOS_BASE`, skipping the pre-remap phase entirely. This is a deliberate simplification, not an oversight; it has shown no behavioral cost across 150 million traced instructions.
- **`BATT_CTRL`** (`0x0D800020`, low-voltage detection) is unmodeled: reads return 0, writes are no-ops. No emulator behavior depends on battery-level sensing, since this emulator has no actual battery.
- **Per-instruction cycle timing follows the documented memory-access-time table and standard ARM7TDMI instruction-class formulas** (see "Memory access timing" above), not a flat 1-cycle-per-instruction approximation. Real hardware now confirms which `F_xxx` ports get WRAM's faster data rate: `FLASH_CTRL` gets the fast rate (see "Memory access timing"). One gap remains, because the source documentation itself gives no answer: BIOS opcode-fetch cost. This emulator assumes it follows FLASH's full ARM/Thumb split; a real-hardware measurement now supports this in the same direction, though not to disassembly-trace standard (see "Memory access timing"). `F_WAIT1`/`F_WAIT2`'s waitstate-control bits are not modeled as dynamically changing timing, since no real trace has ever shown them written for that purpose.

## Licensing note

This project is licensed GPLv3 ([LICENSE](../LICENSE)). BSD-3 code may be referenced or adapted into a GPLv3 project with attribution; that direction is compatible.

Do not copy code from another open-source implementation that has no license. Do not use other closed-source documentation beyond documentation-level facts; none of these other implementations make their code available anyway.
