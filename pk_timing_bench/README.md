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

This app runs all eight measurements once, at startup. After that, you page through eight result screens by hand:

- **RIGHT**: next screen
- **LEFT**: previous screen
- Screens wrap around (8 → RIGHT → 1, 1 → LEFT → 8)
- This app does not use ACTION, UP, or DOWN.

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
| Real hardware, IF the app's own 184-tick budget is this latency | ~`0x12C4` | ~1201 | ~184 ticks |

A result near `0x12C4` confirms the emulator's interrupt path reaches the handler far too quickly, and would account for the whole remaining IR shortfall. A result near `0x1060` means the latency is not where the missing time goes either, and the 184 is something else again.

### Reading the hex digits

Each row is 8 hex digits (0-9, A-F), most-significant nibble first. A small 3×5-pixel font draws each digit. The full 32-pixel screen width is used exactly: 8 digits at a 4px pitch equals 32px.

A "confirmed slower" verdict looks like this: top is clearly and consistently larger than bottom, for example about 2x, or one extra hex digit of magnitude.

A "confirmed equal" verdict looks like this: the two numbers sit within a few percent of each other. Loop and call overhead is never perfectly identical between both sides. Do not expect exact matches, even when the underlying cost is equal.

## Real-hardware findings

**Departing with Action still held relaunched the app.** Holding Action opens this app's continue/exit prompt, and EXIT is confirmed on the Action *press* edge. The departure sequence then ran while the button was still physically down. A press lasts far longer than the departure takes, so control returned to the system with Action held, the system's own browse screen read that as a fresh press, and it launched this app again immediately.

Fixed by waiting for the release before departing, then acknowledging the button sources so no latched HOLD survives either. `INT_INPUT` reports a live button level on real hardware, which is what lets that wait terminate; this app's own hold-to-open gesture already depends on the same property, since it counts 75000 consecutive polls with Action held. The wait is bounded so a stuck contact cannot spin forever, since recovering from that would need the physical reset button.

This emulator latches button STATUS on the press edge rather than tracking a live level, so it cannot reproduce the bug or verify the fix. The validated departure sequence itself is unchanged; the wait only precedes it.


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
