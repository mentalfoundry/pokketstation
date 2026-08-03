# Real-hardware verification log

This log holds real, raw output from running `pk_timing_bench.mcs` on real PocketStation hardware. Use it to compare your own unit, or your own emulator's output, against a known-good run.

See [README.md](README.md) for what each screen means, how to build this app, and the full debugging history behind the current build. That history runs: crash, then LCD_MODE/VRAM/FLASH-width fixes, then the Timer0 wraparound fix, then both icons.

Each entry below is one full run. Each entry lists the build state tested, the exact hardware, and every screen's raw hex values, read directly off the device.

---

## 2026-08-03 (later) — retail PocketStation unit (screen 14 re-run, settled)

**Build tested:** the current committed `pk_timing_bench.mcs`. Screen 14's run-mode row now discards one pulse before counting, and averages two, after the previous run's single-pulse reading came back 11% off. Only screen 14 was re-read.

| Screen | Paused (top) | Running (bottom) |
|---|---|---|
| 14 — RTC interrupt-line rates | `0x00000F3F` | `0x00003D00` |

**Interpretation:**

- **The running rate is 1Hz, exactly.** `0x3D00` = 15616 Timer0 ticks at /512 = 7808 ticks/second, so 4 transitions (two full pulses) in exactly 2.000 seconds. **The previous run's 11% was a settling artefact**, as suspected: a single pulse measured immediately after leaving program mode, while the RTC's own divider was still resynchronising. Discarding one pulse removes it completely.
- **The paused rate reproduces.** `0x0F3F` = 3903 ticks against the previous run's `0x0F40` = 3904 — one tick apart, which is this measurement's own resolution (±0.026%). 256 transitions in 0.031230 seconds = 8197 transitions/second = **4098Hz**, against 4096 documented.
- **Both documented RTC figures are now confirmed as waveform rates**, at 1Hz and 4096Hz, with two transitions per pulse. See the previous entry for what that changed in the emulator.

This run had no crashes, no hangs, and no other anomalies.

---

## 2026-08-03 — retail PocketStation unit (adds screens 13 and 14)

**Build tested:** the current committed `pk_timing_bench.mcs`. It adds screen 13 (the `CLK control` stop/standby test — the only interactive measurement here, run with DOWN) and screen 14 (the RTC's two interrupt-line rates) since the previous run. `SCREEN_EXIT_PROMPT` stays at index 12; the screen cycle is now 1…11 → 13 → 14 → 1.

Screen 13, waiting about 10 seconds before pressing a button to wake the device:

| Screen | Seconds stopped | Timer1 IRQs | `CLK control` readback |
|---|---|---|---|
| 13 — `CLK control` stop/standby | `0x0000000A` | `0x00000000` | `0x00000017` |

| Screen | Paused (top) | Running (bottom) |
|---|---|---|
| 14 — RTC interrupt-line rates | `0x00000F40` | `0x00001B28` |

**Interpretation:**

- **`CLK control` (`0x0B000004`) bit 0 stops the CPU.** Ten seconds of RTC time passed across a single store, with the device sitting there until a button was pressed. This screen issues that store *alone* — no `IOP_STOP`, no RTC interrupt mask, no `LCD_MODE` change — so this isolates the register from everything else the real app writes around it, which no amount of tracing that app could do.
- **The timers freeze with it.** Zero Timer1 interrupts across those ten seconds, with Timer1 armed and its interrupt un-masked the whole time. This was the least certain part of the emulator's model and the one that mattered most: waking on any asserted interrupt would not be a stop at all, since a running timer re-asserts within microseconds.
- **`CLK control` does not read back what was written to it.** The readback is `0x17`, which is the `CLK_MODE` value this app sets (7) with the steady bit (`0x10`) ORed in — the `+0x0` register's readback, not `+0x4`'s. The emulator was returning the stored control word, which nothing on hardware ever shows. A consequence: this screen cannot answer whether the stop bit self-clears, because there is no readable stop status. What the run does show is that the CPU resumed and kept running, so the stop does not persist across a wake.
- **The documented 4096Hz paused rate is a WAVEFORM rate, not a transition rate.** `0x0F40` = 3904 ticks at /32 = 124928 ticks/second, so 256 transitions in exactly 0.031250 seconds: 8192 transitions/second, which is 4096 full pulses. The emulator had been treating 4096Hz as the transition rate, running its line at half the real frequency in both modes.
- **This also confirms `CLK_MODE 7` = 3,997,696Hz.** That figure has only ever come from documentation. Landing exactly on 0.031250 seconds is not possible if the real CPU rate differs meaningfully, so this measurement validates the frequency table entry as a side effect.
- **The running row was not trustworthy** at `0x1B28`, implying 1.123Hz — about 11% fast, from a single pulse measured immediately after leaving program mode. See the entry above for the settled re-run.

This run had no crashes, no hangs, and no other anomalies.

---

## 2026-07-31 (yet later) — retail PocketStation unit (adds screen 11)

**Build tested:** the current committed `pk_timing_bench.mcs`. It adds experiment 11 (the real transmit handler's full dispatch chain: acknowledge, nested `ARM` call, `ARM`-to-`Thumb` trampoline, then re-arm - not a bare re-arm) since the previous run. `SCREEN_EXIT_PROMPT` moved from index 11 to 12. Only screen 11 was read this run.

| Screen | Test (top) | Control (bottom) |
|---|---|---|
| 11 — full-dispatch FIQ latency | `0x00000FE4` | `0x000003F8` |

**Interpretation:**

- **Real hardware matches screens 8 and 10 exactly, bit-for-bit, a third time.** `0x0FE4`/`0x03F8` is the identical raw pair both of those screens read. The arithmetic is unchanged: bottom confirms the armed period (`0x3F8` = 1016), top gives an effective period of `4068 * 32 / 64 / 2` = 1017 Timer2 ticks, latency `1017 - 1017` = **0 ticks**. This holds even with the full realistic dispatch chain in the handler, not just a bare re-arm.
- **This is no longer just "another candidate ruled out."** It falsifies the underlying theory screens 8, 10, and 11 were all built to test: that a re-armed timer's next period does not start until its handler reaches the re-arm, so dispatch latency adds to every period. Three real-hardware measurements now show 0 added latency regardless of how much work sits between expiry and the register write. The simplest explanation left standing: Timer2 auto-reloads in hardware the instant it expires, independent of when software services the interrupt, as long as any re-arm write lands before the *next* natural expiry - which, at a ~1017-tick period against a <200-tick dispatch chain, it always does with enormous margin. See docs/hardware-notes.md's "Unresolved" bullet: the 184-tick figure the real app compensates for is very unlikely to be latency compensation at all, and the original disassembly that produced the "1200 − 184 = 1016" reading is worth re-examining for an alternative meaning.

This run had no crashes, no hangs, and no other anomalies.

---

## 2026-07-31 (even later) — retail PocketStation unit (adds screen 10)

**Build tested:** the current committed `pk_timing_bench.mcs`. It adds experiment 10 (expiry-to-re-arm latency over FIQ/Timer2, instead of screen 8's IRQ/Timer1) since the previous run. `SCREEN_EXIT_PROMPT` moved from index 10 to 11. Only screen 10 was read this run.

| Screen | Test (top) | Control (bottom) |
|---|---|---|
| 10 — expiry-to-re-arm latency, FIQ | `0x00000FE4` | `0x000003F8` |

**Interpretation:**

- **Real hardware matches screen 8's real-hardware reading exactly, bit-for-bit.** `0x0FE4`/`0x03F8` is the identical raw pair screen 8 read on 2026-07-31. The arithmetic is the same as screen 8's: bottom confirms the armed period (`0x3F8` = 1016), top gives an effective period of `4068 * 32 / 64 / 2` = 1017 Timer1 ticks, so expiry-to-re-arm latency is **0 ticks**, same as IRQ.
- **FIQ costs the same as IRQ on real hardware.** This was the one remaining candidate this project could think of to explain the ~184-tick IR pulse-width shortfall as a generic interrupt-path cost: screen 8 measured re-arm latency over IRQ (Timer1, a stand-in, since Timer2 is FIQ-routed and this mechanism was not yet built), and screen 10 now measures the same thing over the real FIQ-routed timer. Both come back at 0 ticks. FIQ-specific overhead is ruled out.
- **Every generic memory-access and interrupt-path cost this project can measure now matches real hardware.** Timer reload semantics (screen 6), interrupt entry cost (screen 7), re-arm latency over both IRQ and FIQ (screens 8 and 10), and `IRDA_DATA`'s own write cost (screen 9) all match. None of them explain the shortfall. See docs/hardware-notes.md's "Unresolved" bullet for where this leaves the investigation.

This run had no crashes, no hangs, and no other anomalies. Notably, the build that hung in this emulator (the version that mistakenly reused `register_irq_handler`, slot 1, for a FIQ source) was never sent to this or any real unit — the emulator caught it first.

---

## 2026-07-31 (later) — retail PocketStation unit (adds screen 9)

**Build tested:** the current committed `pk_timing_bench.mcs`. It adds experiment 9 (`IRDA_DATA` write cost vs WRAM write cost) since the previous run. `SCREEN_EXIT_PROMPT` moved from index 9 to 10, since screen 9 is now a real result screen. Only screen 9 was read this run.

| Screen | Test (top) | Control (bottom) |
|---|---|---|
| 9 — `IRDA_DATA` write cost vs WRAM | `0x00002BF2` | `0x00002849` |

**Interpretation:**

- **Real hardware matches this emulator exactly.** Both rows, `0x2BF2` and `0x2849`, are bit-for-bit identical to what this emulator itself produces for the same experiment (see README.md's "Screen 9" table). This is not a close match within jitter; it is the same value.
- **This rules out the fourth and last easy candidate.** Screens 6, 7, and 8 already ruled out timer-reload semantics, interrupt entry cost, and expiry-to-re-arm latency. Screen 9 now rules out `IRDA_DATA`'s own MMIO write cost too: this emulator's generic 2-cycle "I/O" charge for that register already matches real hardware precisely. The ~184-tick IR pulse-width shortfall is not explained by any generic memory-access or interrupt-path cost this project has measured. See docs/hardware-notes.md's "Unresolved" bullet for where this leaves the investigation.

This run had no crashes, no hangs, and no other anomalies.

---

## 2026-07-31 — retail PocketStation unit (adds screen 8)

**Build tested:** the current committed `pk_timing_bench.mcs`. It adds experiment 8 (expiry-to-re-arm latency) since the previous run.

| Screen | Test (top) | Control (bottom) |
|---|---|---|
| 1 — ARM vs Thumb sanity check | `0x00002BF2` | `0x000019A3` |
| 2 — `FLASH_CTRL` vs WRAM | `0x00002BF2` | `0x00002BF2` |
| 3 — real BIOS ARM helper vs WRAM copy | `0x0000802D` | `0x00006A33` |
| 4 — real BIOS Thumb helper vs WRAM copy | `0x0000B71B` | `0x0000B371` |
| 6 — Timer2 period semantics | `0x00003F90` | `0x00003F87` |
| 7 — interrupt cost | `0x00002BF2` | `0x00002D56` |
| 8 — expiry-to-re-arm latency | `0x00000FE4` | `0x000003F8` |

Screen 5 (raw Timer0 diagnostic, not deltas):

| single_before | single_after | full_before | full_after |
|---|---|---|---|
| `0x0000FFEF` | `0x0000FFEE` | `0x00006269` | `0x0000E23C` |

**Interpretation:**

- Screens 1, 3, 4, 5, 6, and 7 all match the previous run (2026-07-30 later) within the same ±1 jitter seen between runs on this unit. Screen 2's two rows are now exactly equal, tighter than the previous run's ±1. Nothing here suggests a different unit or a measurement anomaly.
- **Screen 8 rules out the re-arm-latency hypothesis.** Screen 8's bottom row confirms the armed period: `0x3F8` = 1016, matching `EXP8_TIMER_PERIOD`. The top row, `0xFE4` = 4068, gives an effective period of `4068 * 32 / 64 / 2` = 1017 Timer1 ticks. Real hardware's expiry-to-re-arm latency is `1017 - 1017` = **0 ticks**, not the ~184 ticks the hypothesis needed. This emulator currently measures 31 ticks for the same experiment, which is closer to the hypothesis than real hardware is, not further from it.
- **All three candidate explanations for the ~184-tick IR pulse-width shortfall are now ruled out.** Screen 6 found only 1 tick from timer-reload semantics. Screen 7 found interrupt entry/dispatch cost matches this emulator to within 2.2%. Screen 8 finds expiry-to-re-arm latency is 0 ticks on real hardware, not 184. None of these generic interrupt/timer-path costs explain the shortfall. Whatever is missing is specific to the real transmit handler's own work, most likely the `IRDA_DATA` write itself or other work in that handler this project's synthetic screen 8 handler does not do.

This run had no crashes, no hangs, and no other anomalies.

---

## 2026-07-30 (later) — retail PocketStation unit (adds screen 7)

**Build tested:** the current committed `pk_timing_bench.mcs`. It adds experiment 7 (interrupt cost), the header fixes at `0x03` and `0x56`, and the emulator-side timer off-by-one fix that the earlier screen 6 run motivated.

| Screen | Test (top) | Control (bottom) |
|---|---|---|
| 1 — ARM vs Thumb sanity check | `0x00002BF2` | `0x000019A3` |
| 2 — `FLASH_CTRL` vs WRAM | `0x00002BF2` | `0x00002BF3` |
| 3 — real BIOS ARM helper vs WRAM copy | `0x0000802D` | `0x00006A33` |
| 4 — real BIOS Thumb helper vs WRAM copy | `0x0000B71B` | `0x0000B371` |
| 6 — Timer2 period semantics | `0x00003F90` | `0x00003F87` |
| 7 — interrupt cost | `0x00002BF2` | `0x00002D56` |

Screen 5 (raw Timer0 diagnostic, not deltas):

| single_before | single_after | full_before | full_after |
|---|---|---|---|
| `0x0000FFEF` | `0x0000FFEE` | `0x00006269` | `0x0000E23C` |

**Interpretation:**

- Screen 5 sanity check: `(0x6269 − 0xE23C) mod 65536 = 0x802D`, matching screen 3's test value exactly. The raw counter data is internally consistent, as in both previous runs.
- **Screen 6 confirms the timer fix.** Before the P+1 correction this emulator returned `0x3F80`/`0x3F80` against hardware's `0x3F90`/`0x3F88`, which is 16 and 8 ticks low. It now returns `0x3F91`/`0x3F88` against this run's `0x3F90`/`0x3F87`, i.e. within +1 on both, the same ±1 jitter the hardware shows against itself between runs.
- **Screen 7 measures the per-interrupt cost as ~98 raw cycles**, via `3200 × (1 − top/bottom)`. This emulator returns `0x2BF2`/`0x2D4E` → ~96 raw cycles, a 2.2% difference.
- **This refutes the interrupt-latency hypothesis.** Screen 7 was built to test whether the ~183 ticks left unaccounted for after screen 6 were being spent in the interrupt path. If the per-interrupt cost were the 368 raw cycles implied by a real IR-using app's own 184-tick pacing compensation, screen 7's bottom row would have read ≈ `0x31A8`. It reads `0x2D56`. The emulator's interrupt entry/dispatch/return cost is essentially correct, and is **not** where the missing IR time goes.
- Net effect across screens 1-4: every value within ±1 of hardware, unchanged by any of this work.

This run had no crashes, no hangs, and no other anomalies. The app-select screen glitch described in README's "Real-hardware findings" (header bytes `0x03`/`0x56`) is fixed as of this build.

---

## 2026-07-30 — retail PocketStation unit (adds screen 6)

**Build tested:** the build that added experiment 6 (Timer2 period semantics, polled, no interrupts taken). Screens 1-5 unchanged from the 2026-07-28 build. This run predates experiment 7, so it has no screen 7 row; the committed build has since gained one.

| Screen | Test (top) | Control (bottom) |
|---|---|---|
| 1 — ARM vs Thumb sanity check | `0x00002BF2` | `0x000019A3` |
| 2 — `FLASH_CTRL` vs WRAM | `0x00002BF2` | `0x00002BF2` |
| 3 — real BIOS ARM helper vs WRAM copy | `0x0000802D` | `0x00006A34` |
| 4 — real BIOS Thumb helper vs WRAM copy | `0x0000B71B` | `0x0000B372` |
| 6 — Timer2 period semantics | `0x00003F90` | `0x00003F88` |

Screen 5 (raw Timer0 diagnostic, not deltas):

| single_before | single_after | full_before | full_after |
|---|---|---|---|
| `0x0000FFF0` | `0x0000FFEE` | `0x00006269` | `0x0000E23C` |

**Interpretation:**

- Screens 1-4 all land within ±1 of the 2026-07-28 run on the same unit, so ±1 is this measurement's real run-to-run jitter, not a build difference. Screen 2 again shows test exactly equal to control, re-confirming `FLASH_CTRL`'s fast data rate.
- Screen 5 sanity check: `(0x6269 − 0xE23C) mod 65536 = 0x802D`, which matches screen 3's test value exactly. The raw counter data is internally consistent, as in the previous run.
- **Screen 6: a timer armed with period P expires every P+1 ticks, not P.** This is a new confirmed real-hardware fact, and it is unusually clean:
  - Top (period 1016 × 256 reloads): `0x3F90` = 16272 Timer0 ticks, versus 16256 for an exact P-tick period. Excess 16 Timer0 ticks = 256 Timer2 ticks over 256 reloads = **exactly 1 tick per reload**.
  - Bottom (period 2032 × 128 reloads): `0x3F88` = 16264, versus the same 16256. Excess 8 Timer0 ticks = 128 Timer2 ticks over 128 reloads = **exactly 1 tick per reload**.
  - The same absolute excess at both periods rules out a rate/divisor error and pins it to a fixed per-period off-by-one: the counter runs P, P-1, … 1, 0 and reloads on the tick *after* zero, so zero is a state it actually occupies.
  - `core/src/timer.c` previously consumed exactly `count` ticks per reload, firing every timer one tick early. Fixed, and `tests/cpu_test.c`'s three timer expectations were corrected to match (they had encoded the old off-by-one).
  - After the fix this emulator returns `0x3F91` / `0x3F88` for screen 6. The bottom row is exact, and the top row is within the ±1 jitter above.
- **What screen 6 rules out.** It was built to locate ~184 missing ticks in a real IR-using app's transmit path (see README.md's "Screen 6"). It found only 1 of them in the timer block, so the remaining ~183 are *not* timer behavior: they must be spent after expiry, in the interrupt path (exception entry, BIOS/kernel dispatch, and the app's own handler prologue). That path is still unmeasured, because the bench masks all interrupts.

This run had no crashes, no hangs, and no other anomalies.

---

## 2026-07-28 — retail PocketStation unit

**Build tested:** the current committed `pk_timing_bench.mcs`, a GNU-toolchain build. It includes the Timer0-wraparound 16-bit mask fix, the `FLASH_CTRL` fast-rate emulator update, and both icons: the PocketStation on-device browse icon, and the standard PS1 memory-card icon.

| Screen | Test (top) | Control (bottom) |
|---|---|---|
| 1 — ARM vs Thumb sanity check | `0x00002BF2` | `0x000019A2` |
| 2 — `FLASH_CTRL` vs WRAM | `0x00002BF2` | `0x00002BF2` |
| 3 — real BIOS ARM helper vs WRAM copy | `0x0000802C` | `0x00006A33` |
| 4 — real BIOS Thumb helper vs WRAM copy | `0x0000B71B` | `0x0000B371` |

Screen 5 (raw Timer0 diagnostic, not deltas):

| single_before | single_after | full_before | full_after |
|---|---|---|---|
| `0x0000FFF0` | `0x0000FFEF` | `0x00006269` | `0x0000E23D` |

**Interpretation:**

- Screen 1: top is notably larger than bottom, about 1.7:1. Non-fetch loop overhead dilutes this from a pure 2:1 opcode-fetch signal. The sanity check passes; the measurement methodology is trustworthy.
- Screen 2: test equals control exactly. This confirms `FLASH_CTRL` gets WRAM's fast 1-cycle data-access rate, not the slow rate.
- Screens 3 and 4: both show clean, small values. Neither shows the `0xFFFFxxxx` wraparound artifact. This confirms the 16-bit Timer0 masking fix holds on real hardware, not just in the emulator.
- Screen 5 sanity check: `(0x6269 − 0xE23D) mod 65536 = 0x802C`. This matches screen 3's test value exactly. The raw counter data is internally consistent.
- Both icons were visually confirmed correct: the on-device browse-screen stopwatch icon, and the standard PS1 memory-card icon, as shown by a real console's own manager or PC tools.

This run had no crashes, no hangs, and no other anomalies.
