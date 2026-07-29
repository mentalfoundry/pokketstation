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

This app runs all five measurements once, at startup. After that, you page through five result screens by hand:

- **RIGHT**: next screen
- **LEFT**: previous screen
- Screens wrap around (5 → RIGHT → 1, 1 → LEFT → 5)
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

### Reading the hex digits

Each row is 8 hex digits (0-9, A-F), most-significant nibble first. A small 3×5-pixel font draws each digit. The full 32-pixel screen width is used exactly: 8 digits at a 4px pitch equals 32px.

A "confirmed slower" verdict looks like this: top is clearly and consistently larger than bottom, for example about 2x, or one extra hex digit of magnitude.

A "confirmed equal" verdict looks like this: the two numbers sit within a few percent of each other. Loop and call overhead is never perfectly identical between both sides. Do not expect exact matches, even when the underlying cost is equal.

## Real-hardware findings

This app went through several rounds of real-hardware-only failures, before it produced trustworthy results. Each failure is a confirmed gap between this emulator and real hardware, not just a bug in this app:

1. **Instant crash and hang, on first boot.** A physical reset recovered the device. This project fixed two causes:
   - This app turned on `LCD_MODE` with a blind overwrite (`mov r1,#0x40; str`), instead of a read-modify-write. This emulator's own `LCD_MODE` default already equals `0x40`. This made the bug invisible in every emulator test. The real pre-dispatch value is undocumented. The real BIOS may leave some other bit set, one the blind overwrite silently cleared.
   - The INTC-mask safety-net write now runs as the first thing this app does. It runs ahead of a CPSR-based IRQ/FIQ disable. That CPSR-based disable is a no-op on real hardware, from unprivileged User mode. This ordering shrinks the window where a stale BIOS interrupt could fire against RAM state the dispatch routine just zeroed.
2. **No more crash, but corrupted pixels and scrambled text.** This emulator allows byte-wide (8-bit) access to non-RAM regions. Real hardware does not allow this access:
   - `draw_pixel` did a byte-wide `LDRB`/`STRB` read-modify-write into VRAM. Real VRAM does not tolerate this access: a single intended pixel came out as a full lit row. This project fixed `draw_pixel` to use word-wide (32-bit) access; each VRAM row is already exactly one word.
   - `draw_glyph` read the font table, in FLASH1, one byte at a time. FLASH is documented as 16-bit and 32-bit access only. This project fixed `draw_glyph` to use a word-aligned read, plus an in-register shift.
3. **Screens 1 and 2 came back clean. Screens 3 and 4 showed `0xFFFFxxxx`, instead of small numbers.** Screen 5's raw diagnostic found the root cause. Timer0's real `COUNT` register behaves as 16-bit on real hardware. This emulator instead models a full 32-bit free-running counter, in `core/src/timer.c`. The slower real-BIOS-call loop, the thing being measured, crosses that 16-bit wrap boundary mid-measurement. The faster WRAM-copy control loop never crosses it. This project fixed the bug by masking every measurement's computed delta to 16 bits.

**Final, clean real-hardware results, after all three fixes above.** Here is what they mean:

- **Screen 2: `FLASH_CTRL` and WRAM read back identical.** `FLASH_CTRL` gets WRAM's fast rate. This result overturned this emulator's prior guess of the slow rate. That prior guess came from disassembling an independent third-party emulator's source, before real-hardware evidence existed. This project updated `core/src/memory.c`'s `psemu_region_data_cycles` accordingly.
- **Screens 3 and 4: BIOS ARM calls run measurably slower than WRAM copies, about 1.2x. BIOS Thumb calls run nearly identical to WRAM copies, about 1.02x.** This is the signature of BIOS ROM matching FLASH's documented split: 2 cycles ARM, 1 cycle Thumb. Thumb's rate matches WRAM's rate, so it washes out. ARM's rate is double, so it shows through, diluted by the loop's non-fetch overhead. This result confirmed this emulator's existing BIOS-fetch-cost guess was already correct.

See [docs/hardware-notes.md](../docs/hardware-notes.md) and [docs/app-notes.md](../docs/app-notes.md) for the full disassembly-level detail behind each result.

## Known caveats

- The CPSR-based IRQ/FIQ disable is a no-op on real hardware, from unprivileged User mode. The real BIOS always dispatches apps from User mode. The real mitigation is the INTC-mask write. It runs first, and works regardless of privilege level. If you see an occasional wildly-outlying number, on one run versus a repeat, suspect an interrupt slipping through in the brief window before that write lands.
- Screens 3 and 4's "test" (real BIOS) measurement does not force a specific internal code path inside the real BIOS routine. It calls the routine exactly as-is, using whatever live kernel RAM state happens to be there. The WRAM "control" copy reads the exact same live memory, so the comparison stays fair. Absolute numbers could still differ slightly between power cycles, if that live state differs.
- **The browse-screen icon's trailing 256 bytes are zero-filled in this build.** This range is Title Sector body offset `0x200`-`0x2FF`. This build does not source these bytes from a real reference app. Their real purpose is still unidentified; see `docs/app-notes.md`.

  During development, this build carried those bytes verbatim from a real reference app. That version is confirmed working on real hardware. That data is third-party game data, so it has no place in a committed, shareable tool.

  The zero-filled version is untested on real hardware. If the browse-screen icon ever fails to render, or renders wrong, on a real unit with this exact build, suspect this trailer first.
