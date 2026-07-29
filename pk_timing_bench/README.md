# pk_timing_bench

A PocketStation homebrew app that measures real ARM7TDMI memory-timing behavior directly on hardware (or in [this project's emulator](../core)), using the device's own Timer0 as a stopwatch, and displays raw results on the 32×32 LCD. It exists to **justify this emulator's memory-timing model with real-hardware evidence** instead of documentation guesses alone, and to let anyone else verify their own PocketStation emulator's timing against the same real device behavior.

It answers two questions that had only best-guess values in `core/src/memory.c` before this app existed:

1. Does BIOS ROM opcode-fetch really split 2 cycles ARM / 1 cycle Thumb, the same way FLASH is documented to?
2. Does the `FLASH_CTRL` register window (`0x06000000`-`0x063FF`) get WRAM's fast 1-cycle data-access rate, or the slow 2-cycle rate everything else gets?

**Both are now answered, via real retail hardware** - see "Real-hardware findings" below, and [docs/hardware-notes.md](../docs/hardware-notes.md)'s "Memory access timing" section / [docs/app-notes.md](../docs/app-notes.md) for the full writeup. Raw output from actual hardware runs is logged in [VERIFICATION.md](VERIFICATION.md), for anyone comparing their own unit or their own emulator against a known-good result.

## Building

Requires the [GNU Arm Embedded Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) (`arm-none-eabi-as`/`ld`/`objcopy` - the zip package needs no installer/admin rights, just extraction) and any C compiler for the small packer program:

```
make
```

Writes `pk_timing_bench.mcs` into this directory (8320 bytes: a real PS1 single-save directory frame + one 8KB PSX Title Sector app block). Load it via any of this project's frontends (`psemu_load_content`), or a real PocketStation's own memory-card-loading pipeline.

No Python, and no assembler-specific scripting - the actual ARM/Thumb source lives in [`src/`](src/) as real, editable GNU-syntax assembly, [`link.ld`](link.ld) places every piece at the exact body offset the runtime logic depends on, and [`pack.c`](pack.c) is the only non-assembly code (it just wraps the assembled/linked binary in the PS1 directory-frame header). Override `AS`/`LD`/`OBJCOPY`/`CC` on the `make` command line if your toolchain binaries aren't on `PATH`.

A prebuilt `pk_timing_bench.mcs` is committed in this directory, so you don't need the toolchain at all unless you want to modify the source.

## Two separate icons

This app has **two independent icons**, read by two completely different things:

- **The PocketStation's own on-device browse-screen icon** (Title Sector body offset `0x100`, a direct 32×32 1bpp bitmap) - what you see on the device's own LCD when browsing to this file before launching it. See [`src/icon.s`](src/icon.s) and `docs/app-notes.md`'s "Browse-screen icon/graphic" section for how this format was reverse-engineered.
- **The standard PS1 memory-card icon** (body offset `0x60`: a 16-color BGR555 palette, then `0x80`: a 16×16 4bpp bitmap) - what a real PS1 console's own memory-card manager, or PC-side memory-card management tools, render when browsing a card. The PocketStation itself never reads this one. Found by diffing a real icon-embedding tool's output against this project's own build byte-for-byte: only 27 bytes differed, all within `0x60`-`0xFF`, confirming the format (this project's icon-frame-count byte at body offset `0x02`, already `0x11` = "1 frame, no animation", turned out to double as the standard field that field actually gates).

The standard PS1 icon is built automatically from [`assets/card_icon.bmp`](assets/card_icon.bmp) (a 16×16, 24-bit, uncompressed BMP) by [`icon_convert.c`](icon_convert.c), which `make` runs before assembling `header.s`. To change it: edit/replace `assets/card_icon.bmp` (must stay 16×16/24-bit/uncompressed, and use 16 or fewer distinct colors - the converter errors out with a clear message otherwise) and rebuild. `icon_convert` samples the top-left pixel as the background color (palette index 0), matching the convention seen in a real reference icon.

## Controls

The app runs all five measurements once at startup, then lets you page through five result screens by hand:

- **RIGHT**: next screen
- **LEFT**: previous screen
- Screens wrap around (5 → RIGHT → 1, 1 → LEFT → 5)
- ACTION/UP/DOWN are not used

## What each screen shows

Screens 1-4 share the same layout:

- Top-left: a single digit (1-5) telling you which screen you're on.
- Middle row of 8 hex digits: the "test" raw tick count.
- Bottom row of 8 hex digits: the "control" raw tick count.

Each value is the number of Timer0 ticks elapsed across 30000 (`0x7530`) iterations of a tight loop - a RAW elapsed-time count, not pre-subtracted. Bigger number = slower.

**Screen 1 - sanity check (read this one first).** Test (top) = ARM-mode loop delta; control (bottom) = Thumb-mode loop delta. Both loops do the exact same thing (read a fixed WRAM address in a tight branch-back loop, N=30000) - the only difference is whether the loop's own instructions are ARM- or Thumb-encoded. This re-derives an already-documented fact that FLASH opcode fetch is 2 cycles/instruction in ARM mode, 1 cycle/instruction in Thumb mode. If this methodology is sound, top should be noticeably bigger than bottom (loop/call overhead dilutes the ratio well below a clean 2:1 - see "Real-hardware findings" for actual numbers). If screen 1 comes back close to 1:1 or some other unexpected ratio, don't trust screens 2-4 - something about the measurement technique itself (Timer configuration, CLK_MODE, interrupt interference) isn't behaving as expected on that unit.

**Screen 2 - FLASH_CTRL vs WRAM data-access cost.** Test (top) = reading `FLASH_CTRL+0x100` (`F_BANK_VAL[0]`) 30000x; control (bottom) = reading a WRAM scratch address 30000x. If `FLASH_CTRL` gets WRAM's fast rate, the two numbers should be close/equal.

**Screen 3 - real BIOS ARM helper vs a WRAM copy of the same code.** Test (top) = calling the real BIOS's "get selected app slot" helper (`0x04001BC8`, in BIOS ROM) 30000x; control (bottom) = calling an identical instruction sequence copied into WRAM, 30000x. If BIOS opcode-fetch matches FLASH's documented 2-cycle ARM rate, top should be noticeably bigger than bottom.

**Screen 4 - real BIOS Thumb helper vs a WRAM copy.** Same idea as screen 3, but with a Thumb-mode BIOS routine (`0x04001320`, the directory-frame-marker check) instead of ARM.

**Screen 5 - raw Timer0 diagnostic** (added after a real-hardware anomaly - see "Real-hardware findings"). Four rows of raw (not delta) Timer0 snapshots: before/after a single isolated real BIOS call, then before/after the full 30000-iteration measurement screen 3 already does.

### Reading the hex digits

Each row is 8 hex digits (0-9, A-F), most-significant nibble first, drawn with a small 3×5-pixel font - the full 32-pixel screen width is used exactly (8 digits × 4px pitch = 32px). A "confirmed slower" verdict looks like top clearly, consistently larger than bottom (e.g. ~2x, or one extra hex digit of magnitude). A "confirmed equal" verdict looks like the two numbers within a few percent of each other - loop/call overhead that isn't perfectly identical between both sides means don't expect exact matches even when the underlying cost really is equal.

## Real-hardware findings

This app went through several rounds of real-hardware-only failures before producing trustworthy results - each one is a real, confirmed gap between this project's emulator and actual silicon, not just a bug in this app:

1. **Instant crash and hang on first boot**, recovered via physical reset. Two causes, fixed:
   - `LCD_MODE` was turned on with a blind overwrite (`mov r1,#0x40; str`) instead of a read-modify-write. The emulator's own `LCD_MODE` default already equals `0x40`, so this was invisible in every emulator test - but the real pre-dispatch value is undocumented, and the real BIOS may leave some other bit set that the blind overwrite silently cleared.
   - The INTC-mask safety-net write now happens as the literal first thing the app does (ahead of a CPSR-based IRQ/FIQ disable that's actually a no-op on real hardware from unprivileged User mode), shrinking the window where a stale BIOS interrupt could fire against RAM state the dispatch routine just zeroed.
2. **No more crash, but corrupted pixels and scrambled text.** This emulator is more permissive than real hardware about byte-wide (8-bit) access to non-RAM regions:
   - `draw_pixel` did a byte-wide `LDRB`/`STRB` read-modify-write into VRAM; real VRAM doesn't tolerate that (a single intended pixel came out as a full lit row). Fixed to word-wide (32-bit) access - each VRAM row is already exactly one word.
   - `draw_glyph` read the font table (in FLASH1) one byte at a time; FLASH is documented as 16/32-bit-only. Fixed to a word-aligned read plus an in-register shift.
3. **Screens 1/2 clean, screens 3/4 showed `0xFFFFxxxx` instead of small numbers.** Root cause (found via screen 5's raw diagnostic): Timer0's real `COUNT` register behaves as effectively 16-bit on real hardware, not the full 32-bit free-running counter this emulator models (`core/src/timer.c`). The slower real-BIOS-call loop (exactly what's being measured) crosses that 16-bit wrap boundary mid-measurement; the faster WRAM-copy control loop never does. Fixed by masking every measurement's computed delta to 16 bits.

**Final, clean real-hardware results** (after all three fixes above), and what they mean:

- **Screen 2: `FLASH_CTRL` and WRAM read back identical.** `FLASH_CTRL` gets WRAM's fast rate - this **overturned** the emulator's prior guess (slow, based on disassembling an independent third-party emulator's source, since real hardware evidence didn't exist yet). `core/src/memory.c`'s `psemu_region_data_cycles` has been updated accordingly.
- **Screens 3/4: BIOS ARM calls measurably slower than WRAM copies (~1.2x); BIOS Thumb calls nearly identical to WRAM copies (~1.02x).** This is exactly the signature of BIOS ROM matching FLASH's documented split (2 cycles ARM, 1 cycle Thumb) - Thumb's rate matches WRAM's, so it washes out; ARM's rate is double, so it shows through, diluted by the loop's non-fetch overhead. This **confirmed** the emulator's existing BIOS-fetch-cost guess was already correct.

See [docs/hardware-notes.md](../docs/hardware-notes.md) and [docs/app-notes.md](../docs/app-notes.md) for the full disassembly-level detail behind each of these.

## Known caveats

- The CPSR-based IRQ/FIQ disable is a real no-op on real hardware from unprivileged User mode (where the real BIOS always dispatches apps) - the actual mitigation is the INTC-mask write, which runs first and works regardless of privilege level. If you see an occasional wildly-outlying number on one run vs. a repeat, an interrupt slipping through in the brief window before that write lands is the likely explanation.
- Screens 3/4's "test" (real BIOS) measurement does not force a specific internal code path inside the real BIOS routine - it calls the routine exactly as-is, using whatever live kernel RAM state happens to be there. The WRAM "control" copy reads the exact same live memory, keeping the comparison fair, but absolute numbers could differ slightly between power cycles if that live state differs.
- **The browse-screen icon's trailing 256 bytes (Title Sector body offset `0x200-0x2FF`) are zero-filled in this build**, not sourced from a real reference app. Their real purpose is still unidentified (see `docs/app-notes.md`) - this build originally carried those bytes verbatim from a real reference app during development, which is confirmed working on real hardware, but that's third-party game data with no place in a committed, shareable tool. Zero-filling instead is untested on real hardware. If the browse-screen icon ever fails to render (or renders wrong) on a real unit with this exact build, this trailer is the first thing to suspect.
