# Real-hardware verification

This file gives the confirmed real-hardware value of each `pk_timing_bench` screen, and the value that this emulator gives for the same screen. Use it to compare a different unit, or the output of a different emulator, against a known-good result.

This file is a reference of current values. It is not a run log. When a new run supersedes a value here, replace the value. The git history of this file holds each superseded per-run entry.

See [README.md](README.md) for the meaning of each screen, for the build method, and for the reason that each experiment exists.

## Where these values come from

**Real hardware:** a retail PocketStation unit, with the values read directly off the 32 by 32 LCD. The date column in each table gives the run that supplied the value.

**This emulator:** one run of `tools/bench_probe.c` on 2026-08-06, against the committed `pk_timing_bench.mcs` and a real J-110 BIOS dump. That tool reads the raw results out of the WRAM result block. Thus it needs no person to read hex digits off the screen. Screens 13 and 14 are the exception. See their sections below.

**Run-to-run jitter** on the same unit is ±1 in the last hex digit. Treat a difference of 1 as agreement. Treat a larger difference as a real difference.

No run recorded here had a crash, a stop, or any other anomaly.

## Screens 1 to 4: memory-access cost

Each value is the number of Timer0 ticks across 30000 iterations of a tight loop.

| Screen | What it compares | Hardware test | Hardware control | Emulator test | Emulator control | Hardware date |
|---|---|---|---|---|---|---|
| 1 | an ARM loop against a Thumb loop | `0x00002BF2` | `0x000019A3` | `0x00002BF2` | `0x000019A2` | 2026-07-31 |
| 2 | `FLASH_CTRL` against WRAM | `0x00002BF2` | `0x00002BF2` | `0x00002BF2` | `0x00002BF2` | 2026-07-31 |
| 3 | a real BIOS ARM helper against a WRAM copy | `0x0000802D` | `0x00006A33` | `0x0000802D` | `0x00006A33` | 2026-07-31 |
| 4 | a real BIOS Thumb helper against a WRAM copy | `0x0000B71B` | `0x0000B371` | `0x0000B71B` | `0x0000B371` | 2026-07-31 |

Screens 2, 3, and 4 agree exactly. The control value of screen 1 is 1 tick apart, which is inside the jitter.

## Screen 5: the raw Timer0 diagnostic

These are raw counter snapshots, and not deltas.

| Source | single_before | single_after | full_before | full_after | Date |
|---|---|---|---|---|---|
| Real hardware | `0x0000FFEF` | `0x0000FFEE` | `0x00006269` | `0x0000E23C` | 2026-07-31 |
| This emulator | `0x0000FFF0` | `0x0000FFEF` | `0x0000626A` | `0x0000E23D` | 2026-08-06 |

The sanity check for this screen is `(full_before − full_after) mod 65536`. Both sources give `0x802D`, which agrees with the test value of screen 3 exactly. Thus the raw counter data is internally consistent on both sides.

## Screens 6 to 11: timer and interrupt cost

| Screen | What it measures | Hardware test | Hardware control | Emulator test | Emulator control | Hardware date |
|---|---|---|---|---|---|---|
| 6 | the Timer2 period behavior | `0x00003F90` | `0x00003F87` | `0x00003F91` | `0x00003F88` | 2026-07-31 |
| 7 | the cost of one interrupt | `0x00002BF2` | `0x00002D56` | `0x00002BF2` | `0x00002D4E` | 2026-07-31 |
| 8 | the expiry-to-arm latency over IRQ | `0x00000FE4` | `0x000003F8` | `0x00001060` | `0x000003F8` | 2026-07-31 |
| 9 | the write cost of `IRDA_DATA` against WRAM | `0x00002BF2` | `0x00002849` | `0x00002BF2` | `0x00002849` | 2026-07-31 |
| 10 | the expiry-to-arm latency over FIQ | `0x00000FE4` | `0x000003F8` | `0x00001080` | `0x000003F8` | 2026-07-31 |
| 11 | the same latency, with the full dispatch chain | `0x00000FE4` | `0x000003F8` | `0x00001103` | `0x000003F8` | 2026-07-31 |

The control row of screens 8, 10, and 11 is the armed period, which is a constant that the app writes. That value is `0x3F8`, which is 1016. It is identical on both sides by construction.

**The derived values of screen 7**, through `3200 * (1 - test/control)`: real hardware gives approximately 98 raw cycles for one interrupt. This emulator gives approximately 96 raw cycles. That is a difference of 2.2%.

**The derived values of screens 8, 10, and 11**, through `top * 32 / 64 / 2` against an armed period of 1017 ticks:

| Screen | Hardware effective period | Hardware latency | Emulator effective period | Emulator latency |
|---|---|---|---|---|
| 8 | 1017 | **0 ticks** | 1048 | 31 ticks |
| 10 | 1017 | **0 ticks** | 1056 | 39 ticks |
| 11 | 1017 | **0 ticks** | 1088.8 | 71.8 ticks |

The period is 1017, and not 1016, because a timer that is armed with P runs P+1 ticks. See screen 6.

## Screen 13: does bit 0 of CLK control stop the CPU?

This screen is interactive. A person presses DOWN to run it, and then waits before a button press wakes the device. Thus `bench_probe` cannot run it. The emulator row below comes from an interactive control run.

| Source | Seconds stopped | Timer1 IRQs | `CLK control` readback | Date |
|---|---|---|---|---|
| Real hardware, with a wait of approximately 10 seconds | `0x0000000A` | `0x00000000` | `0x00000017` | 2026-08-03 |
| This emulator, with a stop of five seconds | `0x00000005` | `0x00000000` | see below | 2026-08-03 |

The seconds row compares directly, because the emulated RTC keeps true 1Hz time. Thus that row must agree with the wall-clock time of the wait, on either side.

**The readback row of the emulator needs a new run.** The recorded run gave `0x00000000`, and that value came from the model that returned the stored control word. `clk_read8` in `core/src/clk.c` now models the measured hardware behavior: both words read back as `CLK_MODE` with the steady bit (`0x10`) ORed in. This app sets `CLK_MODE 7`, thus the modeled readback is now `0x17`. No interactive run has confirmed that value since the correction.

## Screen 14: the two interrupt-line rates of the RTC

This screen runs at startup, but `bench_probe` does not read its two WRAM slots. The emulator row below comes from a recorded control run.

| Source | Paused (top) | Running (bottom) | Date |
|---|---|---|---|
| Real hardware | `0x00000F3F` | `0x00003D00` | 2026-08-03 |
| This emulator | `0x00000F44` | `0x00003D00` | 2026-08-03 |

Real hardware gave the paused value twice, as `0x0F40` and then `0x0F3F`. Those two values are one tick apart, which is the resolution of this measurement, at 0.026%.

## What these values settle

- **`FLASH_CTRL` gets the fast 1-cycle data-access rate of WRAM.** Screen 2 gives a test value that is exactly equal to its control value. It does not get the slow 2-cycle rate.
- **A BIOS opcode fetch follows the ARM and Thumb division of FLASH.** Screen 3 gives a real BIOS ARM helper that is clearly slower than its WRAM copy. Screen 4 gives a real BIOS Thumb helper that is almost equal to its WRAM copy.
- **The measurement method is sound.** Screen 1 gives a ratio of approximately 1.7:1 between ARM and Thumb. The loop overhead and the call overhead dilute the ratio from a pure 2:1 opcode-fetch signal.
- **The 16-bit Timer0 mask correction holds on real hardware.** Screens 3 and 4 give clean, small values. Neither one shows the `0xFFFFxxxx` wraparound artifact.
- **A timer that is armed with period P expires each P+1 ticks.** Screen 6 measures exactly 1 extra tick for each reload, at two different periods, and the extra time is the same absolute value at both. Thus a rate error and a divisor error are not the cause. `core/src/timer.c` has this correction, and three timer values in `tests/cpu_test.c` were corrected with it.
- **The interrupt entry and dispatch cost of this emulator is essentially correct.** Screen 7 gives approximately 98 raw cycles on hardware, against approximately 96 raw cycles here.
- **A timer expiry reaches the arm operation of its handler with 0 added ticks.** Screens 8, 10, and 11 give that same result over IRQ, over FIQ, and with the full realistic dispatch chain of the real transmit handler. Thus Timer2 reloads in hardware at the moment that it expires, independently of the time when software services the interrupt. The arm write must only occur before the *next* natural expiry.
- **The write cost of `IRDA_DATA` is not special.** Screen 9 agrees with this emulator bit for bit. The general 2-cycle "I/O" cost that this emulator gives that register is already correct.
- **Bit 0 of `CLK control` (`0x0B000004`) stops the CPU, and the timers stop with it.** Screen 13 makes that store alone, with no other part of the power-down sequence of the real app. Ten seconds of RTC time passed, and there were zero Timer1 interrupts across those seconds, with Timer1 armed and unmasked for the full time. No quantity of tracing of the real app can give that separation.
- **`CLK control` does not read back the value that software wrote to it.** Screen 13 returns `0x17`, which is the `CLK_MODE` value of the app with the steady bit ORed in. One consequence: this screen cannot answer whether the stop bit clears itself, because there is no readable stop status. The run does show that the CPU started again and continued. Thus the stop does not persist across a wake.
- **Both documented RTC rates are waveform rates, at 1Hz and 4096Hz, with two transitions for each pulse.** Screen 14 gives 256 transitions in 0.031250 seconds while the RTC is paused, and four transitions in exactly 2.000 seconds while it runs.
- **`CLK_MODE 7` really is 3,997,696Hz.** That figure had only ever come from documentation. A screen 14 result of exactly 0.031250 seconds is not possible if the real CPU rate is meaningfully different.

## Known differences between this emulator and real hardware

- **This emulator applies too much cost to a dispatch that reaches an arm operation.** Screens 8, 10, and 11 give 31, 39, and 71.8 ticks here, against 0 ticks on hardware. The cost grows with the complexity of the dispatch, and real hardware shows no such growth. This is a real cycle-cost fault. See "Known open questions and unconfirmed behavior" in [docs/hardware-notes.md](../docs/hardware-notes.md).
- **The paused RTC rate of this emulator is 0.1% under hardware.** Screen 14 gives `0x0F44` here, against `0x0F3F` on hardware. Integer rounding in `RTC_TICK_CYCLES_PAUSED` causes that difference.
- **The interrupt cost of this emulator is 2.2% under hardware.** See screen 7 above.
- The remaining differences are 1 tick or less, which is inside the run-to-run jitter of the hardware itself.
