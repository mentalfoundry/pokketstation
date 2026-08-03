# pk_timing_bench

pk_timing_bench is a PocketStation app. It measures real ARM7TDMI memory-timing behavior. It runs on real hardware, or in [this emulator](../core). It uses the device's own Timer0 as a stopwatch. It shows raw results on the 32×32 LCD.

This app justifies this emulator's memory-timing model with real-hardware evidence. Documentation guesses alone are not enough evidence. Use this app to verify your own PocketStation emulator's timing against the same real-hardware behavior.

It answers two questions. Before this app existed, `core/src/memory.c` held only best-guess values for both:

1. Does BIOS ROM opcode-fetch split 2 cycles ARM / 1 cycle Thumb, the same way FLASH is documented to?
2. Does the `FLASH_CTRL` register window (`0x06000000`-`0x063FF`) get WRAM's fast 1-cycle data-access rate? Or does it get the slow 2-cycle rate everything else gets?

**Real retail hardware now answers both questions.** See "Real-hardware findings" below. See [docs/hardware-notes.md](../docs/hardware-notes.md)'s "Memory access timing" section, and [docs/app-notes.md](../docs/app-notes.md), for the full writeup. [VERIFICATION.md](VERIFICATION.md) logs raw output from real-hardware runs. Use it to compare your own unit, or your own emulator, against a known-good result.

## Building

This build requires the [GNU Arm Embedded Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads). It provides `arm-none-eabi-as`, `arm-none-eabi-ld`, and `arm-none-eabi-objcopy`. The zip package needs no installer and no admin rights; extract it and use it directly. This build also requires any C compiler, for the small packer program:

```
make
```

`make` writes `pk_timing_bench.mcs` into this directory. The file is 8320 bytes: a real PS1 single-save directory frame, plus one 8KB PSX Title Sector app block. Load it with any of this project's frontends, via `psemu_load_content`. You can also load it with a real PocketStation's own memory-card-loading pipeline.

**Link the objects in the Makefile's exact `$(OBJS)` order** (`header icon mirrors font thumb_loop start helpers experiments ui`). If you drive the toolchain by hand rather than through `make`, do not pass `src/*.o`. The shell expands that alphabetically. It then links `experiments.o` first, and pushes `_start` hundreds of bytes further into `.text`. The fixed-offset sections in [`link.ld`](link.ld) still land correctly and the entry-point word at body `0x5C` still points at the real `_start`, so it builds and runs. The resulting image is laid out quite differently from what `make` produces, which makes it useless for byte-comparing against a known-good build. `_start` should come out immediately after `.thumb_loop`.

This build needs no Python, and no assembler-specific scripting. The real ARM/Thumb source lives in [`src/`](src/), as editable GNU-syntax assembly. [`link.ld`](link.ld) places every piece at the exact body offset the runtime logic needs. [`pack.c`](pack.c) is the only non-assembly code; it wraps the assembled and linked binary in the PS1 directory-frame header. If your toolchain binaries are not on `PATH`, override `AS`, `LD`, `OBJCOPY`, or `CC` on the `make` command line.

This directory already contains a prebuilt `pk_timing_bench.mcs`. You need the toolchain only if you want to modify the source.

## Two separate icons

This app has two independent icons. Two different things read them:

- **The PocketStation's own on-device browse-screen icon.** This is a direct 32×32 1bpp bitmap, at Title Sector body offset `0x100`. The device's own LCD shows this icon when you browse to this file, before launching it. See [`src/icon.s`](src/icon.s). See `docs/app-notes.md`'s "Browse-screen icon/graphic" section for how this project reverse-engineered this format.
- **The standard PS1 memory-card icon.** This icon has a 16-color BGR555 palette at body offset `0x60`, then a 16×16 4bpp bitmap at offset `0x80`. A real PS1 console's own memory-card manager renders this icon. PC-side memory-card management tools also render it. The PocketStation itself never reads this icon.

  This project found the format by diffing a real icon-embedding tool's output against this project's own build, byte-for-byte. Only 27 bytes differed, all within `0x60`-`0xFF`. This confirmed the format. This project's icon-frame-count byte, at body offset `0x02`, was already `0x11` ("1 frame, no animation"). That byte turned out to double as the standard field that this icon format gates.

[`icon_convert.c`](icon_convert.c) builds the standard PS1 icon automatically, from [`assets/card_icon.bmp`](assets/card_icon.bmp). `assets/card_icon.bmp` is a 16×16, 24-bit, uncompressed BMP. `make` runs `icon_convert.c` before it assembles `header.s`.

To change the icon: edit or replace `assets/card_icon.bmp`, then rebuild. The file must stay 16×16, 24-bit, and uncompressed. It must use 16 or fewer distinct colors. The converter reports a clear error if the file breaks these rules.

`icon_convert` samples the top-left pixel as the background color, at palette index 0. This matches the convention seen in a real reference icon.

## Controls

This app runs its startup measurements once, at power-on, then you page through the result screens by hand. Screen 13 is the exception: it is interactive and runs on demand.

- **RIGHT**: next screen
- **LEFT**: previous screen
- Screens wrap around (1 … 11 → screen 13 → screen 14 → 1)
- **DOWN**, on screen 13 only, runs the CLK stop test. That is the one measurement here that does not run at startup, because only a human pressing a button can end it. See "Screen 13" below.
- **Holding ACTION** opens a CONTINUE/EXIT prompt, for returning to the system without a hardware reset. UP selects CONTINUE, DOWN selects EXIT, and a fresh ACTION press confirms. See "Screen index used for the continue/exit prompt" in `src/constants.inc`, and `src/ui.s`'s `pb_prompt_confirm_exit`, for the full behavior.

## What each screen shows

Screens 1-4 share the same layout:

- Top-left: a single digit (1-5). This tells you which screen you are on.
- Middle row of 8 hex digits: the "test" raw tick count.
- Bottom row of 8 hex digits: the "control" raw tick count.

Each value is the number of Timer0 ticks elapsed across 30000 (`0x7530`) iterations of a tight loop. This is a raw elapsed-time count, not a pre-subtracted delta. A bigger number means a slower operation.

**Screen 1 - sanity check. Read this one first.** Test (top) is the ARM-mode loop delta. Control (bottom) is the Thumb-mode loop delta.

Both loops do the exact same thing: read a fixed WRAM address, in a tight branch-back loop, with N=30000. The only difference is whether the loop's own instructions are ARM-encoded or Thumb-encoded.

This screen re-derives an already-documented fact: FLASH opcode fetch costs 2 cycles per instruction in ARM mode, and 1 cycle per instruction in Thumb mode.

If the measurement methodology is sound, top is noticeably bigger than bottom. Loop and call overhead dilutes the ratio well below a clean 2:1; see "Real-hardware findings" for real numbers.

If screen 1 comes back close to 1:1, or some other unexpected ratio, do not trust screens 2-4. Something about the measurement technique itself is not behaving as expected on that unit. Suspect Timer configuration, `CLK_MODE`, or interrupt interference.

**Screen 2 - FLASH_CTRL vs WRAM data-access cost.** Test (top) reads `FLASH_CTRL+0x100` (`F_BANK_VAL[0]`), 30000 times. Control (bottom) reads a WRAM scratch address, 30000 times. If `FLASH_CTRL` gets WRAM's fast rate, the two numbers come back close or equal.

**Screen 3 - real BIOS ARM helper vs a WRAM copy of the same code.** Test (top) calls the real BIOS's "get selected app slot" helper, at `0x04001BC8` in BIOS ROM, 30000 times. Control (bottom) calls an identical instruction sequence copied into WRAM, 30000 times. If BIOS opcode-fetch matches FLASH's documented 2-cycle ARM rate, top comes back noticeably bigger than bottom.

**Screen 4 - real BIOS Thumb helper vs a WRAM copy.** This screen uses the same method as screen 3. It uses a Thumb-mode BIOS routine instead of ARM: the directory-frame-marker check, at `0x04001320`.

**Screen 5 - raw Timer0 diagnostic.** This project added screen 5 after a real-hardware anomaly; see "Real-hardware findings". Screen 5 shows four rows of raw Timer0 snapshots, not deltas: before and after a single isolated real BIOS call, then before and after the full 30000-iteration measurement that screen 3 already runs.

**Screen 6: does a timer armed with period P really take P ticks?** Both rows here are raw Timer0 stopwatch totals, not a test/control pair as on screens 1-4. Top is Timer2 armed with **period 1016**, measured across **256** reloads. Bottom is **period 2032** across **128** reloads. Both use Timer2's `/2` divisor and take **no interrupts at all**. Polling Timer2's count register detects the reloads. See `measure_timer_periods` in [`src/helpers.s`](src/helpers.s).

Why 1016: that is the exact period a real IR-using app arms Timer2 with while transmitting. Its nominal IR pulse unit is 1200, and it subtracts a hardcoded 184 before arming. It clearly expects the resulting pulse to land at the full 1200. This emulator produces only ~1041, ~13% short, which is why IR transfers fail in it. Screen 6 exists to find out where those ~184 ticks actually come from.

Because both rows measure the same total number of Timer2 ticks (1016×256 = 2032×128), a correct timer makes **both rows read the same value**. That is what makes the two rows a discriminator:

| Screen 6 result | What it means |
|---|---|
| Both rows `0x3F80` | The timer is exact: period P really does take P ticks. The missing ~184 must be spent *after* expiry, in the interrupt path (exception entry + BIOS/kernel dispatch + the app's handler prologue). |
| Top `0x4B00`, bottom `0x4540` | A fixed **additive** cost per period. The timer block itself takes P + ~184 ticks. That is an emulator timer-model bug, and a much easier fix. |
| Both rows `0x4B00` | A **proportional** rate error. The `/2` divisor, or the clock feeding it, is wrong. It is not a fixed cost. |

`0x3F80` (16256) is what this emulator currently produces for both rows, so any deviation on real hardware is a real, measured emulator inaccuracy. See [VERIFICATION.md](VERIFICATION.md) for logged runs.

**Screen 7: what does taking an interrupt actually cost?** This is the last unmeasured piece of the ~184 ticks a real IR-using app compensates for. Screen 6 proved only 1 of them is timer behavior, so the rest has to be interrupt entry, kernel dispatch and handler prologue.

Method: time the exact same measurement loop twice. The first run masks every interrupt, and gives the top row. The second run keeps a single timer interrupt live at a known fixed rate, and gives the bottom row. Each interrupt steals its full cost from the loop, so the two totals together give the per-interrupt cost. Timer1 is armed at period 99 with the `/32` divisor, firing every `(99+1)*32 = 3200` raw cycles.

**Reading it.** With `B` = top row and `W` = bottom row, both in Timer0 ticks:

```
per-interrupt cost (raw cycles) = 3200 * (1 - B / W)
```

(The interrupt count is not a fixed number: the loop runs longer when it is being interrupted, so more interrupts fit inside it. The formula above already accounts for that.)

| | B (masked) | W (one IRQ live) | per-interrupt cost |
|---|---|---|---|
| This emulator | `0x2BF2` | `0x2D4E` | **96 raw cycles** |
| Real hardware, IF the app's own 184-tick compensation is entry cost | `0x2BF2` | ~`0x31A8` | 368 raw cycles |

So if real hardware comes back near `0x31A8`, the emulator's interrupt path is roughly 4x too cheap and that fully explains the IR failure. A value near `0x2D4E` would instead mean the ~183 ticks are somewhere else again.

**Two bugs were hit building this, both caught in this project's emulator before any hardware run** - either would have wedged a real unit for no measurement:

1. Enabling a timer interrupt with no app IRQ callback registered visibly corrupts the app. The kernel expects one installed first. Experiment 7 therefore registers a handler before it un-masks anything. It registers through `SWI 1`, the call a real app demonstrably uses, found by disassembling that app's interrupt setup.
2. The registration helper first returned with `pop {..., pc}`. On ARMv4T a Thumb POP into PC does not interwork, so it returned into ARM caller code while still in Thumb state and faulted on `unrecognized thumb opcode 0xEBFF` - `0xEB` being the top byte of the ARM `BL` it landed on. It returns via `BX` now, the same way `measure_loop_ptr_thumb` always has.

`SWI 1`'s contract is inferred from that disassembly, not documented: `r0` is the callback slot, `r1` is the handler address, and 0 unregisters. That is worth knowing if screen 7 ever misbehaves on a unit. Experiment 7 re-masks every interrupt source and restores Timer1 before returning, so nothing after it runs with an interrupt still live.

An earlier version gated this behind holding UP at power-on, as a hedge against it hanging a unit. That was removed. The gate depended on reading a live button *level* out of `INTC_STATUS` at startup. Real hardware does that, but this emulator only approximates it, so the gate behaved differently in the two. Holding a direction button through launch also disturbs the BIOS's own menu navigation. With both failure modes above actually fixed rather than hidden behind a switch, the hedge bought nothing.

**Screen 8: how long does a timer expiry take to reach its handler's re-arm?** Top is the Timer0 stopwatch total across **64** Timer1 periods. Bottom is the period each one was armed with, `0x3F8` = 1016.

Screens 6 and 7 each removed a candidate for the ~184 ticks a real IR-using app compensates for, and neither explained it. Tracing that app showed what the earlier screens missed: its transmit handler **re-arms the timer on every interrupt** rather than letting it free-run.

That distinction is the whole point of this screen. A free-running timer keeps its period however late the handler runs, so interrupt latency cancels out and never shows up in the pulse. A re-armed timer does not begin its next period until the handler reaches the re-arm, so the latency is added to *every* period. That is why the app arms 1016 and expects 1200: it budgets 184 ticks for the trip from expiry to re-arm.

Timer1 is armed here with the same 1016 and the same `/2` divisor the real app uses, and its handler re-arms it with the same value.

**Reading it.** With `D` = the top row:

```
effective period (Timer1 ticks) = D * 32 / 64 / 2
expiry-to-re-arm latency        = effective period - 1017
```

(1017, not 1016, because a timer armed with P runs P+1 ticks. See screen 6.)

| | top row | effective period | latency |
|---|---|---|---|
| This emulator | `0x1060` | 1048 | 31 ticks |
| Real hardware (measured 2026-07-31, see [VERIFICATION.md](VERIFICATION.md)) | `0x0FE4` | 1017 | **0 ticks** |

**Real hardware rules this hypothesis out.** Expiry-to-re-arm latency measures 0 ticks on real hardware, not the ~184 the hypothesis needed. This emulator's own 31-tick figure for the same experiment is closer to the ruled-out hypothesis than real hardware is. Combined with screens 6 and 7, all three generic interrupt/timer-path candidates for the ~184-tick shortfall are now ruled out. See "Unresolved" in [docs/hardware-notes.md](../docs/hardware-notes.md) for where this leaves the investigation.

**Screen 9: does `IRDA_DATA`'s own MMIO write cost more than a plain WRAM store?** Top is a loop of **30000** stores to `IRDA_DATA` (`0x0C800004`). Bottom is the same loop storing to a WRAM scratch address instead. Same two-row test/control layout as screens 1-4, and the same method screen 2 already used for `FLASH_CTRL` vs WRAM, but timing stores instead of loads.

Screens 6, 7, and 8 each ruled out a generic interrupt/timer-path candidate for the ~184-tick IR pulse-width shortfall (see screen 8 above). None of them touch the real transmit handler's own work. The one MMIO write on that handler's hot path is `IRDA_DATA`, the LED bit, toggled on every pulse edge. This screen measures that write's cost directly.

This emulator currently charges `IRDA_DATA` the same generic 2-cycle "I/O" data-access rate as everything outside WRAM and `FLASH_CTRL` (see "Memory access timing" in [docs/hardware-notes.md](../docs/hardware-notes.md)), the same rate BIOS, FLASH, and VRAM get.

| | test (`IRDA_DATA`) | control (WRAM) |
|---|---|---|
| This emulator | `0x2BF2` | `0x2849` |
| Real hardware (measured 2026-07-31, see [VERIFICATION.md](VERIFICATION.md)) | `0x2BF2` | `0x2849` |

**Real hardware matches this emulator exactly, bit-for-bit.** `IRDA_DATA`'s own write cost is not the missing time either. Combined with screens 6-8, every generic memory-access and interrupt-path candidate this project can think of to measure has now been measured, and all of them match real hardware.

**Screen 10: does expiry-to-re-arm latency come out differently over FIQ than over IRQ?** Same layout and arithmetic as screen 8, but Timer2 (FIQ-routed) instead of Timer1 (IRQ-routed).

A disassembled trace of the *real* IR transmit handler, not a synthetic one, shows it runs on FIQ. Timer2 is hardwired to FIQ (`INT_FIQ_MASK`, see "Interrupt controller" in [docs/hardware-notes.md](../docs/hardware-notes.md)), and the real handler is reached through the FIQ vector (`0x1C`), confirmed by CPSR mode `0x11` at the exact point of its `IRDA_DATA` write. Screens 7 and 8 only ever measured IRQ. FIQ's own exception-entry cost was never measured on real hardware, only assumed identical to IRQ's.

**This screen exists because of a real bug this project found, not shipped, while building it.** The first version reused `register_irq_handler` (screen 7/8's SWI 1, slot 1) for Timer2. In this emulator, that hung: `pc` got stuck inside the BIOS's own FIQ vector handler forever. Disassembling a real BIOS ROM dump explained why: the IRQ vector handler and the FIQ vector handler read their app-registered callback from two *different* fixed RAM slots (`0xFC` for IRQ, `0x100` for FIQ). With nothing registered at `0x100`, nothing ever acknowledged Timer2's HOLD bit, so FIQ re-asserted the instant the handler returned, forever. The fix is `register_fiq_handler` (`src/thumb_loop.s`), SWI 1 with slot 2 instead of slot 1 - the same mechanism a real app's own comment already documented (`movs r0,#2; ... svc #1` to install a FIQ callback), just never previously wired up in this project's own code. **If you build a variant of this experiment yourself, use `register_fiq_handler`, not `register_irq_handler` - the wrong one hangs the device with no way back except the physical reset button.**

| | top row | effective period | latency |
|---|---|---|---|
| This emulator | `0x1080` | 1056 | 39 ticks |
| Real hardware (measured 2026-07-31, see [VERIFICATION.md](VERIFICATION.md)) | `0x0FE4` | 1017 | **0 ticks** |

**FIQ costs the same as IRQ on real hardware.** `0x0FE4`/`0x03F8` is the exact same raw pair screen 8 read for IRQ. This emulator's own 39-tick figure (higher than screen 8's 31-tick IRQ figure, for the same synthetic shape) is a small emulator inaccuracy, not evidence of FIQ-specific overhead - real hardware shows none. Combined with screens 6-9, every generic memory-access and interrupt-path cost this project can measure now matches real hardware. See "Unresolved" in [docs/hardware-notes.md](../docs/hardware-notes.md) for where this leaves the investigation.

**Screen 11: what does the real transmit handler's FULL dispatch chain cost, not just a bare re-arm?** Same layout and arithmetic as screens 8 and 10.

Screens 8 and 10 both measured a *bare* re-arm: install a minimal handler, take an interrupt, write two timer registers, return. Both came back at 0 ticks on real hardware. But a disassembled trace of the real transmit handler shows its actual dispatch is not bare. It acknowledges its own interrupt sources, calls through a jump table indexed by INTC bit, calls a nested subroutine that reads a state flag, then calls a second subroutine that crosses from ARM to Thumb through an interworking `BX`, before it re-arms Timer2. Measured directly in this emulator (`tools/ir_probe.c`, not `pk_timing_bench`) by timing that real dispatch chain across a real Chocobo World transmit burst, the steady-state cost came out at 128-160 Timer2 ticks - most of the app's 184-tick budget, not the ~0-39 ticks a bare re-arm measures.

This screen reproduces that same shape - acknowledge, nested ARM call, ARM-to-Thumb trampoline, then re-arm - as a real `pk_timing_bench` experiment, so it can be measured on real hardware the same direct way as screens 8 and 10, instead of only inside the emulator. It is not byte-for-byte identical to the real transmit handler (this is original homebrew code, not a copy), so an exact match to 128-160 is not expected. What matters is whether real hardware shows a bare-re-arm-like result (near 0, meaning this emulator overstates dispatch cost) or something closer to its own emulated figure or beyond (meaning the extra dispatch machinery genuinely costs real cycles this emulator's per-instruction model does not fully capture).

| | top row | effective period | latency |
|---|---|---|---|
| This emulator | `0x1103` | 1088.8 | 71.8 ticks |
| Real hardware (measured 2026-07-31, see [VERIFICATION.md](VERIFICATION.md)) | `0x0FE4` | 1017 | **0 ticks** |

**Real hardware matches screens 8 and 10 exactly, bit-for-bit, a third time.** Even with the full realistic dispatch chain in the handler, not a bare re-arm, real hardware still shows 0 added latency. This does more than rule out one more candidate: it falsifies the theory all three screens were built to test, that a re-armed timer's next period does not start until its handler reaches the re-arm. The simplest explanation left standing is that Timer2 auto-reloads in hardware the instant it expires, independent of when software services the interrupt, as long as the re-arm write lands before the *next* natural expiry - which it always does by a wide margin here. See "Unresolved" in [docs/hardware-notes.md](../docs/hardware-notes.md): the app's 184-tick figure is very unlikely to be latency compensation at all.

### Screen 14: the RTC's two interrupt-line rates

Runs at startup like screens 1-11, and is why startup now takes a couple of seconds longer than it used to: a running RTC toggle *is* a second, so measuring two of them costs two real seconds.

**Why it exists.** The RTC's interrupt line is documented as running at "approximately 1Hz" while the clock runs, and "approximately 4096Hz" while it is paused (mode bit 0, `PRGSEL` — the state the BIOS puts it in so `RTC_ADJUST` can step one field without the clock moving underneath it). Neither figure has ever been measured on hardware, and *approximately* is doing real work in that sentence. This emulator derives its paused rate as exactly 4096x its running rate, so if the true ratio is anything else, every `RTC_ADJUST`-driven wait in the BIOS is mistimed here.

The running rate matters for a different reason: it is what makes an emulated second last a real second. The constant behind it was 3.79x off for a long time, and the emulated device's clock lost about 45 minutes an hour as a result.

**Method.** Polls `INT_STATUS`'s RTC bit and times a fixed number of transitions against Timer0. No interrupt is un-masked and none is taken — the status register reports the raw signal level, so the line can be watched directly.

**Two rows of raw Timer0 ticks**, not rates:

| row | what it timed | Timer0 divisor |
|---|---|---|
| top | 256 transitions, RTC **paused** | /32 (as `start.s` leaves it) |
| bottom | 4 transitions (2 full pulses), RTC **running** | /512 |

The two rows use different divisors on purpose: a running pulse is a whole second, which overflows Timer0's real 16-bit count at /32. The run-mode row also discards one pulse before counting, because the first pulse after leaving program mode may be a partial one. Timer0 is restored afterwards, since every other screen's stopwatch is that timer.

**Doing the arithmetic.** The app sets `CLK_MODE 7`, so the CPU runs at 3,997,696Hz and Timer0 ticks at that over its divisor: 124,928/second at /32, 7,808/second at /512. A *waveform* Hz is half the transition rate, since a pulse is two transitions:

- **paused Hz** = 128 ÷ (top ÷ 124,928)
- **running Hz** = 2 ÷ (bottom ÷ 7,808)

**Real-hardware result** (measured, see [VERIFICATION.md](VERIFICATION.md)): top `0x0F40` = **4096.0Hz paused, exactly**. 256 transitions in 0.031250 seconds, to the tick. That settles two things at once: the documented 4096Hz is a waveform rate rather than a transition rate, and `CLK_MODE 7` really is 3,997,696Hz - a figure the timing table had only ever taken from documentation.

**Emulator control run:** top `0x0F44` (8184 transitions/second, 0.1% under hardware from integer rounding in `RTC_TICK_CYCLES_PAUSED`), bottom `0x3D00` (1.0000Hz).

**The open one is the bottom row.** A 1Hz waveform reads `0x3D00`. The first hardware run returned a value implying 1.123Hz - about 11% fast - but from a single pulse measured immediately after leaving program mode, which is exactly where a resynchronising divider would show up. This screen now discards a pulse and averages two. If roughly `0x3650` comes back again, the 11% is real and the running rate needs revisiting; `0x3D00` means the first reading was a settling artefact.

### Screen 13: does CLK control (0x0B000004) bit 0 stop the CPU?

Every other screen here reports a number measured at startup. This one is interactive: **press DOWN to run it.** The quantity being measured is how long the CPU stayed stopped, and on a device with no other input, only a human pressing a button ends that.

**Why it exists.** A real commercial app arms an idle timer and, when it expires with no button pressed, writes `1` to this register as the last step of a power-down sequence (sound off, RTC interrupt masked, display off). Real hardware demonstrably sleeps at that point — the screen blanks and a button press brings it back. But *that this register is what stops the CPU* has never been measured. It was inferred from what the write sits next to. This screen tests the write on its own, with none of the rest of that sequence, which no amount of tracing the real app can do. See "CLK control" in [docs/hardware-notes.md](../docs/hardware-notes.md).

**What it does.** Registers an interrupt handler, arms Timer1 slowly (~8Hz), un-masks the buttons and Timer1, draws a solid bar across the middle of the screen, and then issues one 32-bit store: `1` to `0x0B000004`. Everything after that store runs only once the CPU is running again. The LCD is deliberately left **on**, unlike the real app's sequence, so the result is readable.

**What you see.** If the clock really stops, the bar is the last thing drawn and it stays on screen until you press a button. If the store does nothing, the bar is replaced by the result screen too fast to see.

**Three rows of results**, top to bottom:

| row | meaning |
|---|---|
| top | whole seconds of RTC time that passed across the store |
| middle | Timer1 interrupts counted across the store (buttons are excluded) |
| bottom | `0x0B000004` read back afterwards |

**Reading them together.** Wait several seconds before pressing a button to wake it, so the top row is unambiguous:

| top (seconds) | middle (timer IRQs) | conclusion |
|---|---|---|
| several | 0 | **The CPU stopped and the timers stopped with it.** This is what the emulator models. |
| several | large | The CPU stopped, but the timers kept running and did not wake it. The emulator's wake logic is wrong. |
| 0 | 0 | **The CPU never stopped.** This register is not the stop; something else in the real app's sequence is. |
| 0 | small | The CPU stopped, and a timer interrupt woke it almost immediately. Timers keep running and do wake it. |

If the screen updates **on its own**, with no button pressed, then something other than a button wakes the CPU — the RTC is the likely candidate, since this test leaves its interrupt un-masked where the real app masks it.

The bottom row answers the last question: `0` means the bit self-cleared on wake, `1` means software has to clear it.

**Emulator control run** (this project's own model, for comparison), stopping for five seconds: top `0x00000005`, middle `0x00000000`, bottom `0x00000000`. All three rows are directly comparable to hardware, including the seconds: the emulated RTC keeps true 1Hz time, so the top row should match the wall-clock time you waited on either.

**Recovery.** If the CPU stops and nothing can wake it, the device needs its physical reset button. Nothing in this test writes flash, so that is the whole cost.

### Reading the hex digits

Each row is 8 hex digits (0-9, A-F), most-significant nibble first. A small 3×5-pixel font draws each digit. The full 32-pixel screen width is used exactly: 8 digits at a 4px pitch equals 32px.

A "confirmed slower" verdict looks like this: top is clearly and consistently larger than bottom, for example about 2x, or one extra hex digit of magnitude.

A "confirmed equal" verdict looks like this: the two numbers sit within a few percent of each other. Loop and call overhead is never perfectly identical between both sides. Do not expect exact matches, even when the underlying cost is equal.

## Real-hardware findings

**Departing with Action still held relaunched the app.** Holding Action opens this app's continue/exit prompt, and EXIT is confirmed on the Action *press* edge. The departure sequence then ran while the button was still physically down. A press lasts far longer than the departure takes, so control returned to the system with Action held, the system's own browse screen read that as a fresh press, and it launched this app again immediately.

Fixed by waiting for the release before departing, then acknowledging the button sources so no latched HOLD survives either. `INT_INPUT` reports a live button level on real hardware, which is what lets that wait terminate; this app's own hold-to-open gesture already depends on the same property, since it counts 75000 consecutive polls with Action held. The wait is bounded so a stuck contact cannot spin forever, since recovering from that would need the physical reset button.

`tools/pk_exit_test.c` confirms this end to end in the emulator: it holds Action, opens the prompt, selects EXIT, confirms while still holding Action, and checks that the CPU stays out of BIOS space until Action is released, then departs cleanly once it is. Removing the wait loop makes that same test fail exactly the way the real-hardware report described: departure into BIOS space with Action still held. The validated departure sequence itself is unchanged; the wait only precedes it.

That test needed one core fix first: `core/src/intc.h`'s `INT_LEVEL_MASK` did not include the buttons, so an acknowledge could clear a held button's STATUS bit even though `docs/hardware-notes.md` documents STATUS as tracking the live level. `pk_exit_test` does not actually exercise that fix, because this app never enables or acknowledges its own button interrupts, so nothing in this specific flow ever wipes STATUS regardless. The fix is still correct on its own terms, backed by the same documented behavior and by its own unit test (`test_button_status_survives_acknowledge` in `tests/cpu_test.c`), but it is not what makes this particular exit bug reproduce or resolve in the emulator. That is entirely the wait loop above.


**Two header bytes this app was leaving at zero, that every real app sets.** Both fell inside zero-fill regions in `header.s` and were assumed reserved:

- **`0x03` is the block count.** It holds how many 8KB blocks the save occupies. Every real app checked sets it correctly. The reference dumps on hand cover 1-, 2-, 4-, 7- and 13-block saves, and each one declares its own true count. This app declared 0.
- **`0x56` is `0x01` in all nine real apps inspected**, regardless of block count, icon style or frame count. It is therefore not a frame count. It is most likely a format or version marker. This app declared 0, the only file in the corpus doing so.

Between them these corrupted the real BIOS's own app-select screen. LEFT/RIGHT browse between saves normally (horizontal transition), but this app also had a vertical axis on UP/DOWN: pressing UP transitioned down into a glitched screen, every further UP press reproduced the identical glitch, and DOWN transitioned back up to the correctly-rendered icon.

The glitched screen renders whatever is at body `0x200-0x2FF` - deliberately editing that region changed which garbage appeared, which is how it was confirmed as the read target rather than the cause.

One reference dump is what isolated this. It has byte-for-byte the same container shape as this app: one block, a `0x2000` body, and chain link `0xFFFF`. It renders cleanly, which ruled out the container itself and left the declared header fields. With both bytes corrected, this app's header now matches a known-good multi-block dump in every field except block count, which legitimately differs.

Two suspects were ruled out along the way: the unidentified `0x200-0x2FF` icon trailer (filling it with this app's own bitmap changed the glitch but did not fix it, and was reverted), and the object link order used when building by hand (see "Building").

## Known caveats

- The CPSR-based IRQ/FIQ disable is a no-op on real hardware, from unprivileged User mode. The real BIOS always dispatches apps from User mode. The real mitigation is the INTC-mask write. It runs first, and works regardless of privilege level. If you see an occasional wildly-outlying number, on one run versus a repeat, suspect an interrupt slipping through in the brief window before that write lands.
- Screens 3 and 4's "test" (real BIOS) measurement does not force a specific internal code path inside the real BIOS routine. It calls the routine exactly as-is, using whatever live kernel RAM state happens to be there. The WRAM "control" copy reads the exact same live memory, so the comparison stays fair. Absolute numbers could still differ slightly between power cycles, if that live state differs.
- **The browse-screen icon's trailing 256 bytes are zero-filled in this build.** This range is Title Sector body offset `0x200`-`0x2FF`. This build does not source these bytes from a real reference app. Their real purpose is still unidentified; see `docs/app-notes.md`.

  During development, this build carried those bytes verbatim from a real reference app. That version is confirmed working on real hardware. That data is third-party game data, so it has no place in a committed, shareable tool.

  The zero-filled version is untested on real hardware. If the browse-screen icon ever fails to render, or renders wrong, on a real unit with this exact build, suspect this trailer first.
