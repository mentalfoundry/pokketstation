# Real-hardware verification log

This log holds real, raw output from running `pk_timing_bench.mcs` on real PocketStation hardware. Use it to compare your own unit, or your own emulator's output, against a known-good run.

See [README.md](README.md) for what each screen means, how to build this app, and the full debugging history behind the current build. That history runs: crash, then LCD_MODE/VRAM/FLASH-width fixes, then the Timer0 wraparound fix, then both icons.

Each entry below is one full run. Each entry lists the build state tested, the exact hardware, and every screen's raw hex values, read directly off the device.

---

## 2026-07-30 (later) — retail PocketStation unit (adds screen 7)

**Build tested:** the current committed `pk_timing_bench.mcs` — adds experiment 7 (interrupt cost), the header fixes at `0x03`/`0x56`, and the emulator-side timer off-by-one fix that the earlier screen 6 run motivated.

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

- Screen 5 sanity check: `(0x6269 − 0xE23C) mod 65536 = 0x802D`, matching screen 3's test value exactly — internally consistent, as in both previous runs.
- **Screen 6 confirms the timer fix.** Before the P+1 correction this emulator returned `0x3F80`/`0x3F80` against hardware's `0x3F90`/`0x3F88` — 16 and 8 ticks low. It now returns `0x3F91`/`0x3F88` against this run's `0x3F90`/`0x3F87`, i.e. within +1 on both, the same ±1 jitter the hardware shows against itself between runs.
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
- **Screen 6 — a timer armed with period P expires every P+1 ticks, not P.** This is a new confirmed real-hardware fact, and it is unusually clean:
  - Top (period 1016 × 256 reloads): `0x3F90` = 16272 Timer0 ticks, versus 16256 for an exact P-tick period. Excess 16 Timer0 ticks = 256 Timer2 ticks over 256 reloads = **exactly 1 tick per reload**.
  - Bottom (period 2032 × 128 reloads): `0x3F88` = 16264, versus the same 16256. Excess 8 Timer0 ticks = 128 Timer2 ticks over 128 reloads = **exactly 1 tick per reload**.
  - The same absolute excess at both periods rules out a rate/divisor error and pins it to a fixed per-period off-by-one: the counter runs P, P-1, … 1, 0 and reloads on the tick *after* zero, so zero is a state it actually occupies.
  - `core/src/timer.c` previously consumed exactly `count` ticks per reload, firing every timer one tick early. Fixed, and `tests/cpu_test.c`'s three timer expectations were corrected to match (they had encoded the old off-by-one).
  - After the fix this emulator returns `0x3F91` / `0x3F88` for screen 6 — bottom exact, top within the ±1 jitter above.
- **What screen 6 rules out.** It was built to locate ~184 missing ticks in a real IR-using app's transmit path (see README.md's "Screen 6"). It found only 1 of them in the timer block, so the remaining ~183 are *not* timer behavior: they must be spent after expiry, in the interrupt path (exception entry, BIOS/kernel dispatch, and the app's own handler prologue). That path is still unmeasured — the bench masks all interrupts.

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
