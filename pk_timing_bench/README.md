# pk_timing_bench

pk_timing_bench is a PocketStation app. It measures real hardware behavior that no available documentation records: memory-access cost, timer and interrupt timing, the CPU stop bit, and the two interrupt-line rates of the RTC. It operates on real hardware, and in [this emulator](../core). It uses Timer0 of the device as a stopwatch. It shows raw results on the 32 by 32 LCD.

This app supports the hardware model of this emulator with real-hardware evidence. A guess from documentation is not sufficient evidence. Use this app to compare the timing of a different PocketStation emulator against the same real-hardware behavior.

This app started with two questions about the memory-timing table in `core/src/memory.c`. It now covers nine. **Real retail hardware answers each one:**

| Question | The answer that real hardware gives | Screen |
|---|---|---|
| Does a BIOS ROM opcode fetch divide into 2 cycles for ARM and 1 cycle for Thumb, the same way as the documented behavior of FLASH? | Yes. | 1, 3, 4 |
| Does the `FLASH_CTRL` register window (`0x06000000` to `0x063FF`) get the fast 1-cycle data-access rate of WRAM, or the slow 2-cycle rate that each other region gets? | The fast rate. | 2 |
| Does a timer that is armed with period P take P ticks? | No. It takes P+1 ticks. | 6 |
| What does one interrupt cost? | Approximately 98 raw cycles. | 7 |
| How long does a timer expiry take to reach the arm operation in its handler? | 0 ticks, over IRQ, over FIQ, and with a full dispatch chain. | 8, 10, 11 |
| Does an MMIO write to `IRDA_DATA` cost more than a plain WRAM store? | No. The cost is identical. | 9 |
| Does bit 0 of `CLK control` (`0x0B000004`) stop the CPU? | Yes, and the timers stop with it. | 13 |
| Are the documented RTC interrupt-line rates waveform rates or transition rates? | Waveform rates, at 1Hz and 4096Hz. | 14 |
| Is `CLK_MODE 7` really 3,997,696Hz? | Yes. | 14 |

Those answers corrected `core/src/memory.c`, `timer.c`, `rtc.c`, and `clk.c` in this emulator. Each correction is a real behavior difference, and not a tuning value.

[VERIFICATION.md](VERIFICATION.md) records the confirmed real-hardware value of each screen, next to the value that this emulator gives. Use it to compare a different unit, or a different emulator, against a known-good result. See the "Memory access timing" section in [docs/hardware-notes.md](../docs/hardware-notes.md), and [docs/app-notes.md](../docs/app-notes.md), for the full writeup of the memory-timing result. "Other real-hardware findings" below gives the results that are not screen values.

## Building

This build needs the [GNU Arm Embedded Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads). That toolchain supplies `arm-none-eabi-as`, `arm-none-eabi-ld`, and `arm-none-eabi-objcopy`. The zip package needs no installer, and it needs no administrator rights. Extract it and use it directly. This build also needs any C compiler, for the small packer program:

```
make
```

`make` writes `pk_timing_bench.mcs` into this directory. The file is 8320 bytes. It holds a real PS1 single-save directory frame, and one 8KB PSX Title Sector app block. Load it with any frontend of this project, through `psemu_load_content`. The memory-card-loading pipeline of a real PocketStation also loads it.

**Link the objects in the exact `$(OBJS)` order of the Makefile** (`header icon mirrors font thumb_loop start helpers experiments ui`). Do not pass `src/*.o` to the toolchain by hand. The shell expands that pattern alphabetically. It then links `experiments.o` first, and it pushes `_start` several hundred bytes further into `.text`. The fixed-offset sections in [`link.ld`](link.ld) still land correctly, and the entry-point word at body `0x5C` still points at the real `_start`. Thus the build is successful, and the app operates. But the layout of that image is very different from the layout that `make` gives. Thus a byte comparison against a known-good build is not possible. `_start` must come immediately after `.thumb_loop`.

This build needs no Python, and it needs no assembler-specific scripting. The real ARM source and Thumb source are in [`src/`](src/), as editable GNU-syntax assembly. [`link.ld`](link.ld) puts each piece at the exact body offset that the runtime logic needs. [`pack.c`](pack.c) is the only code that is not assembly. It puts the assembled and linked binary into the PS1 directory-frame header. If the toolchain binaries are not on `PATH`, give `AS`, `LD`, `OBJCOPY`, or `CC` on the `make` command line.

This directory already holds a prebuilt `pk_timing_bench.mcs`. The toolchain is necessary only for a change to the source.

## Two separate icons

This app has two independent icons. Two different things read them:

- **The browse-screen icon of the PocketStation.** This is a direct 32 by 32 1bpp bitmap, at Title Sector body offset `0x100`. The LCD of the device shows this icon at a browse operation to this file, before the launch. See [`src/icon.s`](src/icon.s). See the "Browse-screen icon/graphic" section in `docs/app-notes.md` for the method that gave this format.
- **The standard PS1 memory-card icon.** This icon has a 16-color BGR555 palette at body offset `0x60`, and then a 16 by 16 4bpp bitmap at offset `0x80`. The memory-card manager of a real PS1 draws this icon. A PC-side memory-card management tool also draws it. The PocketStation never reads this icon.

  A byte-for-byte comparison gave this format. That comparison put the output of a real icon-embedding tool against the build of this project. Only 27 bytes were different, and each one is in the range `0x60` to `0xFF`. That result confirmed the format. The icon-frame-count byte of this project, at body offset `0x02`, was already `0x11`, which means "1 frame, no animation". That byte is also the standard field that this icon format uses as a gate.

[`icon_convert.c`](icon_convert.c) builds the standard PS1 icon automatically, from [`assets/card_icon.bmp`](assets/card_icon.bmp). That file is a 16 by 16, 24-bit, uncompressed BMP. `make` runs `icon_convert.c` before it assembles `header.s`.

To change the icon, edit or replace `assets/card_icon.bmp`. Then build again. The file must stay 16 by 16, 24-bit, and uncompressed. It must use 16 distinct colors or fewer. The converter gives a clear error message if the file does not obey these rules.

`icon_convert` takes the top-left pixel as the background color, at palette index 0. This agrees with the convention in a real reference icon.

## Controls

This app runs its startup measurements one time, at power-on. A person then pages through the result screens by hand. Screen 13 is the exception. It is interactive, and it operates on demand.

- **RIGHT**: the next screen
- **LEFT**: the previous screen
- The screens wrap around: 1 to 11, then screen 13, then screen 14, then 1.
- **DOWN**, on screen 13 only, runs the CLK stop test. That is the one measurement here that does not run at startup, because only a button press from a person can end it. See "Screen 13" below.
- **A hold on ACTION** opens a CONTINUE/EXIT prompt. That prompt returns to the system without a hardware reset. UP selects CONTINUE, DOWN selects EXIT, and a new ACTION press confirms the selection. See "Screen index used for the continue/exit prompt" in `src/constants.inc`, and `pb_prompt_confirm_exit` in `src/ui.s`, for the full behavior.

## What each screen shows

Screens 1 to 4 use the same layout:

- Top-left: one digit, from 1 to 5. This digit gives the screen number.
- The middle row of 8 hex digits: the "test" raw tick count.
- The bottom row of 8 hex digits: the "control" raw tick count.

Each value is the number of Timer0 ticks across 30000 (`0x7530`) iterations of a tight loop. This is a raw elapsed-time count. It is not a delta with a subtraction already applied. A larger number shows a slower operation.

**Screen 1, the sanity check. Read this screen first.** The test value (top) is the ARM-mode loop delta. The control value (bottom) is the Thumb-mode loop delta.

Both loops do exactly the same operation. They read a fixed WRAM address, in a tight branch-back loop, with N=30000. The only difference is the encoding of the instructions of the loop, which is ARM or Thumb.

This screen derives a documented fact again. A FLASH opcode fetch costs 2 cycles for each instruction in the ARM mode, and 1 cycle for each instruction in the Thumb mode.

The top value is clearly larger than the bottom value if the measurement method is correct. The loop overhead and the call overhead dilute the ratio well below a clean 2:1. Real hardware gives approximately 1.7:1. See [VERIFICATION.md](VERIFICATION.md) for the raw values.

Do not trust screens 2 to 4 if screen 1 gives a ratio near 1:1, or a different unexpected ratio. Something in the measurement technique does not behave as expected on that unit. Examine the Timer configuration, `CLK_MODE`, and interrupt interference.

**Screen 2, the data-access cost of `FLASH_CTRL` against WRAM.** The test (top) reads `FLASH_CTRL+0x100` (`F_BANK_VAL[0]`) 30000 times. The control (bottom) reads a WRAM scratch address 30000 times. The two numbers come back close or equal if `FLASH_CTRL` gets the fast rate of WRAM.

**Screen 3, a real BIOS ARM helper against a WRAM copy of the same code.** The test (top) calls the "get selected app slot" helper of the real BIOS, at `0x04001BC8` in BIOS ROM, 30000 times. The control (bottom) calls an identical instruction sequence in WRAM 30000 times. The top value comes back clearly larger than the bottom value if a BIOS opcode fetch agrees with the documented 2-cycle ARM rate of FLASH.

**Screen 4, a real BIOS Thumb helper against a WRAM copy.** This screen uses the same method as screen 3. It uses a BIOS routine in the Thumb mode in place of the ARM mode: the directory-frame-marker check, at `0x04001320`.

**Screen 5, the raw Timer0 diagnostic.** This project added screen 5 after screens 3 and 4 returned `0xFFFFxxxx` values on real hardware. Those values were a Timer0 wraparound artifact, and a 16-bit mask corrected them. Screen 5 shows four rows of raw Timer0 snapshots, and not deltas. Those rows are the values before and after one isolated real BIOS call, and then the values before and after the full 30000-iteration measurement that screen 3 runs.

The sanity check for this screen is `(full_before − full_after) mod 65536`. That value must equal the test value of screen 3 exactly. Real hardware and this emulator both give `0x802D`. Thus the raw counter data is internally consistent on both sides.

**Screen 6: does a timer that is armed with period P take P ticks?** Both rows here are raw Timer0 stopwatch totals. They are not a test and control pair, which is the layout of screens 1 to 4. The top row is Timer2 armed with **period 1016**, measured across **256** reloads. The bottom row is **period 2032** across **128** reloads. Both rows use the `/2` divisor of Timer2, and both take **no interrupts at all**. A poll of the count register of Timer2 finds each reload. See `measure_timer_periods` in [`src/helpers.s`](src/helpers.s).

The reason for 1016: that is the exact period that the serial-carrying app arms Timer2 with while it transmits. Its nominal IR pulse unit is 1200, and it subtracts a fixed 184 before the arm operation. This screen was built to find the source of those approximately 184 ticks, at a time when the transmitted pulse of this emulator measured only approximately 1041, which is approximately 13% short.

Both rows measure the same total number of Timer2 ticks, because 1016 x 256 = 2032 x 128. Thus a correct timer makes **both rows read the same value**. That property makes the two rows a discriminator:

| Screen 6 result | Meaning |
|---|---|
| Both rows `0x3F80` | The timer is exact. A period of P takes P ticks. Thus the missing approximately 184 ticks occur *after* the expiry, in the interrupt path. That path is the exception entry, the BIOS and kernel dispatch, and the prologue of the handler of the app. |
| Top `0x4B00`, bottom `0x4540` | A fixed **additive** cost for each period. The timer block takes P plus approximately 184 ticks. That result is a timer-model fault in this emulator, and it is a much easier correction. |
| Both rows `0x4B00` | A **proportional** rate error. The `/2` divisor, or the clock that feeds it, is incorrect. It is not a fixed cost. |

Real hardware gave `0x3F90` and `0x3F88`, which is the second condition at a much smaller scale: exactly 1 extra tick for each period. `core/src/timer.c` now has that correction, and this emulator returns `0x3F91` and `0x3F88`. See [VERIFICATION.md](VERIFICATION.md) for the confirmed values.

**Screen 7: what does an interrupt cost?** Screen 6 gives only 1 tick of the approximately 184 ticks that the serial-carrying app compensates for. This screen tests the next candidate for the remainder, which is the interrupt entry, the kernel dispatch, and the prologue of the handler.

**The method.** Time exactly the same measurement loop two times. The first run masks each interrupt, and it gives the top row. The second run keeps one timer interrupt live at a known fixed rate, and it gives the bottom row. Each interrupt takes its full cost from the loop. Thus the two totals together give the cost of one interrupt. Timer1 is armed at period 99 with the `/32` divisor. Thus it fires every `(99+1)*32 = 3200` raw cycles.

**How to read it.** `B` is the top row, and `W` is the bottom row. Both are in Timer0 ticks:

```
per-interrupt cost (raw cycles) = 3200 * (1 - B / W)
```

The interrupt count is not a fixed number. The loop runs longer while interrupts interrupt it, thus more interrupts fit inside it. The formula above already includes that effect.

| | B (masked) | W (one IRQ live) | cost of one interrupt |
|---|---|---|---|
| This emulator | `0x2BF2` | `0x2D4E` | 96 raw cycles |
| Real hardware (measured 2026-07-31, see [VERIFICATION.md](VERIFICATION.md)) | `0x2BF2` | `0x2D56` | **98 raw cycles** |
| The prediction, if the 184-tick compensation of the app were entry cost | `0x2BF2` | approximately `0x31A8` | 368 raw cycles |

**Real hardware removes this hypothesis.** The interrupt entry, dispatch, and return cost of this emulator is 2.2% under hardware, and not approximately 4 times too cheap. Thus this path is essentially correct, and it is **not** the place where the missing IR time goes.

**This project met two faults during the build of this screen. This emulator caught both faults before any run on hardware.** Either fault stops a real unit, and it gives no measurement:

1. A timer interrupt that is enabled with no app IRQ callback registered corrupts the app visibly. The kernel needs a callback first. Thus experiment 7 registers a handler before it unmasks anything. It registers through `SWI 1`, which is the call that a real app uses. A disassembly of the interrupt setup of that app found the call.
2. The registration helper returned with `pop {..., pc}` at first. On an ARMv4T core, a Thumb POP into the PC does not change the instruction set. Thus the code returned into ARM caller code while it was still in the Thumb state. It then caused an `unrecognized thumb opcode 0xEBFF` fault. `0xEB` is the highest byte of the ARM `BL` instruction at that address. The helper now returns with `BX`, the same way as `measure_loop_ptr_thumb`.

The contract of `SWI 1` comes from that disassembly. It has no documentation. `r0` is the callback slot, `r1` is the handler address, and a value of 0 removes the callback. That contract is useful if screen 7 behaves incorrectly on a unit. Experiment 7 masks each interrupt source again, and it restores Timer1, before it returns. Thus nothing after it operates with a live interrupt.

An earlier version put this experiment behind a hold on UP at power-on, as protection against a stop on a unit. This project removed that gate. The gate read a live button *level* from `INTC_STATUS` at startup. Real hardware gives that level, but this emulator only approximates it. Thus the gate behaved differently in the two. A hold on a direction button through the launch also disturbs the menu navigation of the BIOS. Both failure modes above now have a real correction, thus the protection gave nothing.

**Screen 8: how long does a timer expiry take to reach the arm operation in its handler?** The top row is the Timer0 stopwatch total across **64** Timer1 periods. The bottom row is the period that each one was armed with, which is `0x3F8` = 1016.

Screens 6 and 7 each removed a candidate for the approximately 184 ticks that the serial-carrying app compensates for, and neither one explained it. A trace of that app shows the part that the earlier screens missed. Its transmit handler **arms the timer again at each interrupt**. It does not let the timer run free.

That difference is the reason for this screen. A free-running timer keeps its period however late the handler runs. Thus the interrupt latency cancels, and it never appears in the pulse. Under the theory that this screen tests, a re-armed timer does not start its next period until the handler reaches the arm operation. Thus the latency adds to *each* period. That theory explains why the app arms 1016 and expects 1200: it budgets 184 ticks for the path from the expiry to the arm operation.

Timer1 is armed here with the same 1016 and the same `/2` divisor that the real app uses. Its handler arms it again with the same value.

**How to read it.** `D` is the top row:

```
effective period (Timer1 ticks) = D * 32 / 64 / 2
expiry-to-arm latency           = effective period - 1017
```

The value is 1017, and not 1016, because a timer that is armed with P runs P+1 ticks. See screen 6.

| | top row | effective period | latency |
|---|---|---|---|
| This emulator | `0x1060` | 1048 | 31 ticks |
| Real hardware (measured 2026-07-31, see [VERIFICATION.md](VERIFICATION.md)) | `0x0FE4` | 1017 | **0 ticks** |

**Real hardware removes this hypothesis.** The latency from the expiry to the arm operation measures 0 ticks on real hardware. It is not the approximately 184 ticks that the hypothesis needs. The 31-tick figure of this emulator, for the same experiment, is closer to the removed hypothesis than real hardware is. With screens 6 and 7, each of the three general interrupt-path and timer-path candidates for the approximately 184-tick shortfall is now removed. See "Timing measurements on real hardware" in [docs/hardware-notes.md](../docs/hardware-notes.md) for the full table of screen results.

**Screen 9: does an MMIO write to `IRDA_DATA` cost more than a plain WRAM store?** The top row is a loop of **30000** stores to `IRDA_DATA` (`0x0C800004`). The bottom row is the same loop with the store to a WRAM scratch address. This screen uses the same two-row test and control layout as screens 1 to 4. It uses the same method that screen 2 uses for `FLASH_CTRL` against WRAM, but it times a store and not a load.

Screens 6, 7, and 8 each removed a general interrupt-path or timer-path candidate for the approximately 184-tick IR pulse-width shortfall. See screen 8 above. None of them cover the work of the real transmit handler. The one MMIO write on the frequent path of that handler is `IRDA_DATA`, at the LED bit, which the handler changes at each pulse edge. This screen measures the cost of that write directly.

This emulator gives `IRDA_DATA` the same general 2-cycle "I/O" data-access rate as each region outside WRAM and `FLASH_CTRL`. See "Memory access timing" in [docs/hardware-notes.md](../docs/hardware-notes.md). BIOS, FLASH, and VRAM get that same rate.

| | test (`IRDA_DATA`) | control (WRAM) |
|---|---|---|
| This emulator | `0x2BF2` | `0x2849` |
| Real hardware (measured 2026-07-31, see [VERIFICATION.md](VERIFICATION.md)) | `0x2BF2` | `0x2849` |

**Real hardware agrees with this emulator exactly, bit for bit.** Thus the write cost of `IRDA_DATA` is not the missing time either. With screens 6 to 8, each general memory-access candidate and interrupt-path candidate that this project can measure is now measured, and each one agrees with real hardware.

**Screen 10: is the latency from the expiry to the arm operation different over FIQ than over IRQ?** This screen uses the same layout and the same arithmetic as screen 8. It uses Timer2, which routes to FIQ, in place of Timer1, which routes to IRQ.

A disassembly trace of the *real* IR transmit handler, and not a synthetic one, shows that the handler runs on FIQ. Timer2 connects to FIQ directly. See `INT_FIQ_MASK`, and "Interrupt controller" in [docs/hardware-notes.md](../docs/hardware-notes.md). The FIQ vector (`0x1C`) reaches the real handler. The CPSR mode of `0x11`, at the exact point of its `IRDA_DATA` write, confirms this path. Screens 7 and 8 measured only IRQ. The exception-entry cost of FIQ had no measurement on real hardware. This project only assumed that it is identical to the cost of IRQ.

**This screen exists because of a real fault that this project found during its build. That fault never shipped.** The first version used `register_irq_handler` for Timer2. That helper is the `SWI 1` slot 1 call of screens 7 and 8. In this emulator, that use stopped the app: `pc` stayed inside the FIQ vector handler of the BIOS with no exit. A disassembly of a real BIOS ROM dump gives the reason. The IRQ vector handler and the FIQ vector handler read their app-registered callback from two *different* fixed RAM slots. Those slots are `0xFC` for IRQ, and `0x100` for FIQ. With no callback at `0x100`, nothing acknowledged the HOLD bit of Timer2. Thus FIQ asserted again immediately after each return of the handler, with no exit. The correction is `register_fiq_handler` in `src/thumb_loop.s`, which is `SWI 1` with slot 2 in place of slot 1. A comment in a real app already documents that same mechanism (`movs r0,#2; ... svc #1` installs a FIQ callback), and this project had never used it before. **Use `register_fiq_handler`, and not `register_irq_handler`, in a variant of this experiment. The incorrect one stops the device, and only the physical reset button recovers it.**

| | top row | effective period | latency |
|---|---|---|---|
| This emulator | `0x1080` | 1056 | 39 ticks |
| Real hardware (measured 2026-07-31, see [VERIFICATION.md](VERIFICATION.md)) | `0x0FE4` | 1017 | **0 ticks** |

**On real hardware, FIQ costs the same as IRQ.** The pair `0x0FE4` and `0x03F8` is the exact raw pair that screen 8 read for IRQ. The 39-tick figure of this emulator is higher than the 31-tick IRQ figure of screen 8, for the same synthetic shape. That difference is a small inaccuracy of this emulator. It is not evidence of an overhead that is specific to FIQ, because real hardware shows none. With screens 6 to 9, each general memory-access cost and interrupt-path cost that this project can measure now agrees with real hardware. See "Timing measurements on real hardware" in [docs/hardware-notes.md](../docs/hardware-notes.md) for the full table of screen results.

**Screen 11: what does the FULL dispatch chain of the real transmit handler cost?** This screen measures more than a minimal arm operation. It uses the same layout and the same arithmetic as screens 8 and 10.

Screens 8 and 10 both measured a *minimal* arm operation. That operation is a minimal handler, one interrupt, a write to two timer registers, and a return. Both gave 0 ticks on real hardware. But a disassembly trace of the real transmit handler shows that its actual dispatch is not minimal. That handler acknowledges its own interrupt sources. It then calls through a jump table, with the INTC bit as its index. It then calls a nested subroutine, which reads a state flag. It then calls a second subroutine, which changes from ARM to Thumb with an interworking `BX`. It arms Timer2 again after those steps. A direct measurement in this emulator gives a steady-state cost of 128 to 160 Timer2 ticks. `tools/ir_probe.c` made that measurement, and not `pk_timing_bench`. It timed that real dispatch sequence across a real transmit burst of the serial-carrying app. That cost is most of the 184-tick value of the app. It is much more than the approximately 0 to 39 ticks that a minimal arm operation gives.

This screen reproduces that same shape as a real `pk_timing_bench` experiment. That shape is an acknowledge, a nested ARM call, an ARM-to-Thumb transition, and then the arm operation. Thus a person can measure it on real hardware, the same direct way as screens 8 and 10. The measurement is not only possible in the emulator. This screen is original homebrew code, and not a copy. Thus it is not byte-for-byte identical to the real transmit handler, and an exact match to 128 to 160 ticks is not expected.

| | top row | effective period | latency |
|---|---|---|---|
| This emulator | `0x1103` | 1088.8 | 71.8 ticks |
| Real hardware (measured 2026-07-31, see [VERIFICATION.md](VERIFICATION.md)) | `0x0FE4` | 1017 | **0 ticks** |

**Real hardware agrees with screens 8 and 10 exactly, bit for bit, a third time.** Real hardware gives 0 added latency, even with the full realistic dispatch chain in the handler and not a minimal arm operation. This result does more than remove one more candidate. It disproves the theory that all three screens test, which gives the next period of a re-armed timer as a period that starts only when its handler reaches the arm operation. The simplest remaining explanation: Timer2 reloads in hardware at the moment that it expires, independently of the time when software services the interrupt. That behavior needs one condition. The arm write must occur before the *next* natural expiry. Here, that write always occurs in time, with a large margin. Thus the 184-tick figure of the app is very probably not a latency compensation at all. See "Timing measurements on real hardware" in [docs/hardware-notes.md](../docs/hardware-notes.md), and the open question about the meaning of that figure in "Known open questions and unconfirmed behavior".

### Screen 14: the two interrupt-line rates of the RTC

This screen executes at startup, the same as screens 1 to 11. It is the reason that the startup now needs a few more seconds. One transition of a running RTC *is* one second. Thus a measurement of two transitions costs two real seconds.

**The reason for this screen.** The available documentation gives the interrupt line of the RTC as "approximately 1Hz" while the clock runs, and as "approximately 4096Hz" while the clock is paused. The paused state is mode bit 0, `PRGSEL`. The BIOS uses that state, thus `RTC_ADJUST` can change one field while the clock stays constant. Neither figure had a measurement on hardware, and the word "approximately" carries real weight in that sentence. This emulator derives its paused rate as exactly 4096 times its running rate, in `RTC_TICK_CYCLES_PAUSED` (`core/src/rtc.h`). A different true ratio mistimes each `RTC_ADJUST` wait in the BIOS. This screen confirms the ratio of 4096.

The running rate is important for a different reason. It makes an emulated second last a real second. The constant behind it was 3.79 times incorrect for a long time, and the clock of the emulated device lost approximately 45 minutes each hour.

**The method.** This screen polls the RTC bit of `INT_STATUS`, and it times a fixed number of transitions against Timer0. It unmasks no interrupt, and it takes no interrupt. The status register reports the raw signal level, thus a poll can watch the line directly.

**Two rows of raw Timer0 ticks**, and not rates:

| row | what it timed | Timer0 divisor |
|---|---|---|
| top | 256 transitions, RTC **paused** | /32, which is the value that `start.s` leaves |
| bottom | 4 transitions, which is 2 full pulses, RTC **running** | /512 |

The two rows use different divisors deliberately. A running pulse is a full second, which overflows the real 16-bit count of Timer0 at /32. The run-mode row also discards one pulse before it counts, because the first pulse after the exit from program mode can be a partial pulse. This screen restores Timer0 afterwards, because that timer is the stopwatch of each other screen.

**The arithmetic.** The app sets `CLK_MODE 7`. Thus the CPU runs at 3,997,696Hz, and Timer0 ticks at that rate over its divisor: 124,928 each second at /32, and 7,808 each second at /512. A *waveform* Hz is one half of the transition rate, because a pulse is two transitions:

- **paused Hz** = 128 / (top / 124,928)
- **running Hz** = 2 / (bottom / 7,808)

**The real-hardware result is 4096Hz paused.** Two measurements gave `0x0F40` and then `0x0F3F`. Those two values are one tick apart, which is the resolution of this measurement. See [VERIFICATION.md](VERIFICATION.md). That is 256 transitions in 0.031250 seconds, to the tick. That result settles two things at one time. The documented 4096Hz is a waveform rate, and not a transition rate. And `CLK_MODE 7` really is 3,997,696Hz, which is a figure that the timing table had only ever taken from documentation.

**The control run in this emulator.** The top value is `0x0F44`, which is 8184 transitions each second. That value is 0.1% under hardware, from integer rounding in `RTC_TICK_CYCLES_PAUSED`. The bottom value is `0x3D00`, which is 1.0000Hz.

**The bottom row is settled too.** Hardware returns `0x3D00`, which is four transitions in exactly 2.000 seconds. That is a **1Hz waveform**, which is again exactly the documented figure. An earlier build measured one pulse with no discard, and it read 11% fast. The cause was a divider that resynchronized immediately after the exit from program mode. A discard of one pulse removes that effect completely.

**Thus both documented RTC figures are confirmed, and both are waveform rates.** See [VERIFICATION.md](VERIFICATION.md) for the confirmed values.

### Screen 13: does bit 0 of CLK control (0x0B000004) stop the CPU?

Each other screen here reports a number that it measured at startup. This screen is interactive. **Press DOWN to run it.** The measured quantity is the time that the CPU stayed stopped. On a device with no other input, only a button press from a person ends that time.

**The reason for this screen.** A real commercial app arms an idle timer. When that timer expires and no button is down, the app writes `1` to this register. That write is the last step of a power-down sequence. The earlier steps turn the sound off, mask the RTC interrupt, and turn the display off. Real hardware sleeps at that point, which a test confirms: the screen blanks, and a button press brings it back. But *that this register is the register that stops the CPU* was an inference from the writes next to it. This screen tests the write alone, with no other part of that sequence. No quantity of tracing of the real app can give that separation. See "CLK control" in [docs/hardware-notes.md](../docs/hardware-notes.md).

**The real-hardware result confirms the inference.** With a wait of approximately 10 seconds, the top row read `0x0000000A`, and the middle row read `0x00000000`. Thus bit 0 of this register stops the CPU, and the timers stop with it. Timer1 was armed and unmasked for the full ten seconds, and it counted zero interrupts. That second part matters as much as the first. A wake on each asserted interrupt is not a stop at all, because a running timer asserts again in microseconds. See [VERIFICATION.md](VERIFICATION.md).

**What it does.** It registers an interrupt handler. It arms Timer1 slowly, at approximately 8Hz. It unmasks the buttons and Timer1. It draws a solid bar across the middle of the screen. It then makes one 32-bit store of `1` to `0x0B000004`. Each step after that store runs only when the CPU runs again. This screen leaves the LCD **on** deliberately, which is different from the sequence of the real app. Thus the result is readable.

**What appears.** If the clock really stops, the bar is the last thing that this screen draws, and it stays on the screen until a button press. If the store does nothing, the result screen replaces the bar too fast to see.

**Three rows of results**, from the top to the bottom:

| row | meaning |
|---|---|
| top | the whole seconds of RTC time that passed across the store |
| middle | the Timer1 interrupts counted across the store. The buttons are excluded. |
| bottom | the value that `0x0B000004` reads back afterwards |

**How to read them together.** Wait several seconds before a button press wakes the device. Thus the top row is unambiguous:

| top (seconds) | middle (timer IRQs) | conclusion |
|---|---|---|
| several | 0 | **The CPU stopped, and the timers stopped with it.** This is the row that real hardware gives, and it is the model of this emulator. |
| several | large | The CPU stopped, but the timers continued and they did not wake it. Thus the wake logic of this emulator is incorrect. |
| 0 | 0 | **The CPU never stopped.** This register is not the stop. A different write in the sequence of the real app is the stop. |
| 0 | small | The CPU stopped, and a timer interrupt woke it almost immediately. Thus the timers continue, and they do wake the CPU. |

If the screen changes **with no button press**, then a source that is not a button wakes the CPU. The RTC is the most probable source, because this test does not mask its interrupt. The real app does mask that interrupt. No run has met this condition. Thus the question is still open.

**The bottom row is not a stop status.** Real hardware returned `0x17`, which is the `CLK_MODE` value that this app sets, with the steady bit (`0x10`) ORed in. Thus both words of this register read back as `CLK_MODE`, and `+0x4` does not read back the value that software wrote to it. `clk_read8` in `core/src/clk.c` now models that behavior. One consequence: this screen cannot show whether the stop bit clears itself, because there is no readable stop status. The run does show that the CPU started again and continued. Thus the stop does not persist across a wake.

**The control run in this emulator** used a stop of five seconds, and it gave top `0x00000005` and middle `0x00000000`. The seconds row compares directly against hardware, because the emulated RTC keeps true 1Hz time. Thus that row must agree with the wall-clock time of the wait, on either side. That run came before the correction to `clk_read8`, thus its readback row is out of date. See [VERIFICATION.md](VERIFICATION.md).

**Recovery.** If the CPU stops and nothing wakes it, the device needs its physical reset button. Nothing in this test writes flash, thus that is the full cost.

### Reading the hex digits

Each row is 8 hex digits, from 0 to 9 and A to F, with the most significant nibble first. A small 3 by 5 pixel font draws each digit. The rows use the full 32-pixel screen width exactly: 8 digits at a 4px pitch is 32px.

A "confirmed slower" result looks like this. The top value is clearly and consistently larger than the bottom value, for example approximately 2 times larger, or one more hex digit of magnitude.

A "confirmed equal" result looks like this. The two numbers are within a few percent of each other. The loop overhead and the call overhead are never perfectly identical between the two sides. Do not expect an exact match, even when the underlying cost is equal.

## Other real-hardware findings

These findings are not screen values. [VERIFICATION.md](VERIFICATION.md) holds each screen value.

**A departure with Action still held started the app again.** A hold on Action opens the continue/exit prompt of this app, and the Action *press* edge confirms EXIT. The departure sequence then ran while the button was still physically down. A press lasts much longer than the exit operation. Thus control returned to the system while the user held Action. The browse screen of the system then read that condition as a new press, and it started this app again immediately.

The correction waits for the release before the departure. It then acknowledges the button sources, thus no latched HOLD survives either. `INT_INPUT` reports a live button level on real hardware. That level lets the wait operation end. The hold-to-open gesture of this app already depends on the same property, because it counts 75000 sequential reads with Action held. The wait is bounded, thus a stuck contact cannot spin forever. Recovery from that condition needs the physical reset button.

`tools/pk_exit_test.c` confirms this behavior completely in the emulator. It holds Action, it opens the prompt, and it selects EXIT. It then confirms the selection while it still holds Action. It confirms that the CPU stays outside BIOS memory until the release of Action. It then confirms that the app exits correctly after the release. A removal of the wait loop makes that same test fail exactly the way that the real-hardware report describes. That failure is a departure into BIOS space with Action still held. The validated departure sequence has no change. The wait only comes before it.

That test needed one correction in the core first. `INT_LEVEL_MASK` in `core/src/intc.h` did not include the buttons. Thus an acknowledge could clear the STATUS bit of a held button, but `docs/hardware-notes.md` gives STATUS as a register that follows the live level. `pk_exit_test` does not exercise that correction, because this app never enables its own button interrupts and never acknowledges them. Thus nothing in this specific flow ever clears STATUS. That correction is still correct. The same recorded behavior supports it, and it has its own unit test, `test_button_status_survives_acknowledge` in `tests/cpu_test.c`. But that correction is not the cause of this exit fault, and it is not the reason that the fault occurs or stops in the emulator. The wait loop above is the full reason.

**This app left two header bytes at zero. Each real app sets both bytes.** Both bytes are inside zero-fill regions in `header.s`, and this project assumed that they were reserved:

- **`0x03` is the block count.** It holds the number of 8KB blocks that the save occupies. Each real app that this project examined sets it correctly. The available reference dumps cover saves of 1, 2, 4, 7, and 13 blocks, and each one declares its own true count. This app declared 0.
- **`0x56` is `0x01` in each of the nine real apps that this project examined.** That value does not change with the block count, the icon style, or the frame count. Thus it is not a frame count. It is most probably a format marker or a version marker. This app declared 0, and it was the only file in that set with that value.

Together, these two bytes corrupted the app-select screen of the real BIOS. LEFT and RIGHT browse between saves correctly, with a horizontal transition. But this app also had a vertical axis on UP and DOWN. An UP press made a transition down into a screen with an error. Each further UP press gave the identical error. A DOWN press made a transition back up to the correctly drawn icon.

The screen with the error draws the contents of body `0x200` to `0x2FF`. A deliberate edit to that region changed the data that appeared. That method confirmed the region as the read target, and not as the cause.

One reference dump isolated this fault. That dump has byte-for-byte the same container shape as this app: one block, a `0x2000` body, and chain link `0xFFFF`. It draws correctly. Thus the container is not the cause, and only the declared header fields remain. With both bytes corrected, the header of this app agrees with a known-good multi-block dump in each field except the block count, which is correctly different.

This work removed two candidates. The first candidate is the unidentified icon data at `0x200` to `0x2FF`. A test filled that area with the bitmap of this app. The error on the screen changed, but it did not stop, and this project reversed that change. The second candidate is the object link order for a manual build. See "Building".

## Known caveats

- A CPSR-based disable of IRQ and FIQ has no effect on real hardware, from unprivileged User mode. The real BIOS always dispatches an app from User mode. The real protection is the INTC-mask write. That write runs first, and it operates at each privilege level. If one run gives a number that is very different from a repeat of the same run, examine an interrupt that got through in the short window before that write lands.
- The "test" measurement of screens 3 and 4, which calls the real BIOS, does not force a specific internal code path inside the real BIOS routine. It calls the routine exactly as it is, with the live kernel RAM state that is present. The WRAM "control" copy reads exactly the same live memory, thus the comparison stays fair. The absolute numbers can still be slightly different between power cycles, if that live state is different.
- **The last 256 bytes of the browse-screen icon are zero-filled in this build.** That range is Title Sector body offset `0x200` to `0x2FF`. This build does not take these bytes from a real reference app. Their real purpose is still unknown. See `docs/app-notes.md`.

  During development, this build held those bytes exactly as they are in a real reference app. A test confirms that version on real hardware. That data is third-party app data, thus it has no place in a committed tool that this project shares.

  The zero-filled version now also operates on real hardware. Each run from 2026-07-30 and later used the committed build, and the browse-screen icon drew correctly at each launch from the card directory. This is no longer an untested condition. Nothing suggests that these bytes matter for a single-frame icon. See the comment in `src/icon.s`.
