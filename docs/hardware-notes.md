# PocketStation hardware notes

Reference documentation for this emulator's hardware model. Facts here are drawn from three sources, in descending order of trust: (1) direct testing against real hardware, (2) tracing real BIOS/app execution with `tools/inspect.c` against real dumps (never committed - see `testdata/`, gitignored), (3) official register documentation. Where something is inferred or still unconfirmed, it's called out explicitly in "Known open questions and unconfirmed behavior" at the end rather than stated as fact.

This implementation has been validated by tracing real BIOS + real app execution (a real copy of Chocobo World, and a real ID-editing homebrew) for hundreds of millions of instructions combined, with zero unrecognized-opcode faults once the fixes described below were applied.

## CPU

ARM7TDMI (ARM/Thumb), Atmel-fabricated. Reset starts in Supervisor mode, ARM state; the first ~12 instructions visit FIQ then IRQ then back to SVC, initializing each mode's banked SP in turn (confirmed via real BIOS trace).

**Real ARM7TDMI behaviors confirmed against real hardware/apps** (each was a genuine gap in this emulator's CPU core, found via real execution rather than synthetic tests, and is now fixed):

- **Thumb `BL` tags the return address's bit 0**, even on plain ARMv4T (not an ARMv5 `BLX`-only feature) - `(R15+2)|1`, so a later `BX LR` correctly stays in Thumb. Fixed in `exec_long_branch_link`; see `test_thumb_bl_bx_lr_stays_thumb`.
- **`LDM` with the `^` suffix on a register list including `PC`** (e.g. `LDM SP!,{r1-r12,lr,pc}^`, the real SWI handler's return idiom) restores the *entire* CPSR from the current mode's SPSR, not just `PC` - a distinct encoding from `MOVS/SUBS PC,LR`. Fixed in `exec_block_transfer`; see `test_arm_ldm_exception_return`.
- **A misaligned `LDRH`** (odd address) reads the aligned-down halfword and then rotates the result right by 8 bits (swaps its two bytes) before it reaches the register - it does not just silently round down and lose the low address bit. A misaligned `LDRSH`, by contrast, does *not* get the same rotate-then-sign-extend treatment: real silicon instead behaves as a sign-extended **byte** load (`LDRSB`) from the odd address. Fixed in `exec_halfword_transfer`; see `test_arm_ldrh_misaligned_quirks`. (Found via a real ID-editing homebrew's font-glyph routine, which reads a byte-packed table one byte at a time via `LDRH Rd,[Rn],#1` - post-incrementing by 1, not 2 - masked to the low byte; this only works given the rotation.)
- **FIQ takes strict priority over IRQ**, and FIQ entry sets *both* `CPSR.F` and `CPSR.I` (IRQ/SWI/aborts only ever set `I`) - see "Interrupt controller" below.

Variable clock via `CLK_MODE` - see that section below.

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
| `0x0C800000`+ | — | IR registers (protocol/send-receive mode at `+0`, beam on/off at `+4`). Register bit-layout only - see "Known open questions". |
| `0x0D000000` | 0x8 | `LCD_MODE`: bit6 `DISON` (display on/off), bit7 `ROT` (rotate 180°). |
| `0x0D000100` | 128B | LCD VRAM. |
| `0x0D800000` | 0x10 | IOP power control: IOP_CTRL(+0x0, unmodeled, no known effect), IOP_STOP/IOP_STAT(+0x4, W/R, sets bits), IOP_START(+0x8, W, clears bits), IOP_DATA(+0xC, unused by the real BIOS). Bit5 = Sound Enable. |
| `0x0D800010` | 0x10 | DAC: `ctrl`(+0x0, bit0 enable), `data`(+0x4, bits6-15 signed 10-bit `DACV`). |

BIOS ROM and FLASH2 are only readable in 16/32-bit units; RAM is freely 8/16/32-bit.

## LCD

32×32 pixels, 1 bit per pixel. VRAM is 128 bytes (32 rows × 32-bit words). Bit 0 of each word is the leftmost pixel; 0 = white, 1 = black. Hardware refreshes at roughly 32Hz once the CPU writes a row.

`LCD_MODE` (`0x0D000000`, separate from VRAM): bit6 `DISON` controls whether anything is displayed at all (blank when clear); bit7 `ROT` rotates the presented image 180° (reversed scanline order, each scanline's bits reversed left-right) - real hardware keeps this in sync with the docking flag so the screen reads right-side-up whether handheld or docked. `psemu_get_framebuffer` returns this post-processed view, not raw VRAM. Default `mode` has `DISON` set - this is this emulator's own safe default (the real POR value isn't documented), chosen so an app that never touches the register still renders. See `test_lcd_mode_dison_and_rotate`.

## Buttons

5 face buttons (Up, Right, Down, Left, Fire) plus a physical reset, read as bits 0–4 of `INT_INPUT` at `0x0A000004`.

A button's `hold` bit is a momentary edge pulse per physical press, **not** a sustained level for the whole time it's held down - unlike genuine level-triggered sources (IOP, battery, timers). This matters because the BIOS's generic system-tick callback is a fixed-priority chain (IOP > battery > Timer1 > Action > RTC) that only reaches the RTC-driven redraw step if none of the earlier bits are set in `hold` - a continuously-asserted Action bit would permanently starve RTC processing for as long as the button stayed physically held. `status` still tracks the live level for any code polling it directly. Implemented via `intc_clear_hold_only` (`core/src/intc.c`), called from `psemu_set_buttons` for a still-held button with no new press edge. See `test_button_hold_pulses_not_sustained`.

Buttons are processed via the interrupt (`hold`)/callback path, not by directly polling `status`.

## App file format: PSX Title Sector

PocketStation apps are **not** GME (that's the DexDrive whole-card dump format, `.gme` — needs its container header stripped down to the raw 128KB before it'll load here). The real container is the **PSX Title Sector**, an 80-byte header followed by icon data, an optional snapshot, a function table, and the executable body:

- Offset `0x00–0x4F`: standard PS1 memory-card title/icon header fields.
- Offset `0x50–0x5F`: PocketStation-specific — icon frame counts, an `"MCX0"`/`"MCX1"` identifier at `0x52`, entry point + THUMB-mode flag (bit 0) at `0x5C`.
- If the identifier is `MCX1`: an `0x800`-byte snapshot follows (saved ARM register/RAM state), then the function table and executable body. `MCX0` skips the snapshot.

A PocketStation app extracted as a single PS1 save (DuckStation, MemcardRex, and most other save managers export these as `.mcs`) is this same Title Sector body, just prefixed with one real PS1 directory frame (`0x80` bytes: in-use marker, little-endian data size at `0x04`, link/filename fields). `psemu_load_mcs` strips that frame and validates the size against it, then defers to the same Title Sector loading `psemu_load_app` does - both frontends try `.pss` first and fall back to `.mcs` regardless of the file's actual extension.

**PocketStation apps are frequently bundled as multi-block linked memory-card files** together with the game's regular PS1 save, not as a standalone single 8KB block. A naive per-block scan for the `MCX0`/`MCX1` magic will find the right *first* block but silently miss the rest of the app if you don't follow the card's actual directory (block 0): each of the 15 data-block frames is 128 bytes with allocation state at offset `0x00` (`0x51`=first/solo, `0x52`=middle, `0x53`=last, `0xA0`=free), total filesize at `0x04`, and a next-block link at `0x08` (a little-endian u16, 0-based among the 15 data blocks - add 1 to get the physical block number, `0xFFFF` sentinel = end of chain).

## Flash memory

**FLASH2** (`0x08000000`, physical, 128KB): 16 blocks of 8KB. Block 0 is reserved for a real PS1-style memory-card directory (16×128-byte frames: frame 0 is the card header, frames 1-15 describe blocks 1-15).

**FLASH1** (`0x02000000`, virtual): a 16-slot banked window onto FLASH2, resolved live against two `FLASH_CTRL` registers:
- `F_BANK_FLG` (`FLASH_CTRL+8`): a bitmask of which *physical* 8KB blocks are enabled for the current app.
- `F_BANK_VAL` (`FLASH_CTRL+0x100`-`0x13C`, 16 words, reset value 0): says, **per physical block**, which virtual bank slot (0-15) it appears at - `table[physical]=virtual`, the "backwards" direction from a typical page table. Resolving virtual→physical requires a reverse search over the 16 entries (`flash_resolve_physical_bank`). When `F_BANK_VAL` is untouched (its reset value for every physical bank - which is every case the real BIOS's own app-dispatch routine has been observed to trigger, since it only ever writes `F_BANK_FLG`, never `F_BANK_VAL`), resolution falls back to a contiguous linear mapping starting at the lowest-numbered enabled physical block. See `test_flash_bank_val_remapping` for a genuine non-contiguous/reordered mapping, and `test_flash_bank_select` for the fallback case.

`FLASH_CTRL` (`0x06000000`) also has: `+0` a command/commit trigger (write `2` to commit a bank-select change; a real BIOS routine then busy-waits on this same address's bit 0 reading back `1` - this emulator's commits are synchronous, so reads of `+0` always OR in bit 0). `+0x10` (`F_WAIT2`, waitstates/flash-write-status) is polled by a real app's flash-write routine expecting bit 2 to read back set once a write completes - always reports "not busy" here, since writes complete instantly. See `test_flash_ctrl_busy_wait_bits`.

**`F_KEY1` (`0x08002A54`) and `F_KEY2` (`0x080055AA`) are flash-unlock command-latch addresses, not data storage.** Real flash hardware intercepts a write of the documented `FFAAh`/`FF55h`/`FFA0h` sequence there as an unlock command (arming the chip's write-unlock state machine) rather than storing it - the byte physically "at" that address is unaffected. `flash_write8`/`flash1_write8` (`core/src/flash.c`) no-op writes to either 16-bit-wide key address, in both the FLASH2 physical path and the FLASH1 virtual window. See `test_flash_key_addresses_are_not_data_storage`.

### App-selection and dispatch

Reverse-engineered directly from real BIOS disassembly:

1. A helper returns a selected app-slot index from RAM: `*(u16*)0x000000D0` if nonzero, else `*(u8*)0x000000CE`.
2. **`FLASH2` carries the exact same directory structure as a PS1 memory card** - the kernel reads the directory frame at `FLASH2 + slot*128` and walks the chain via the frame's next-block link, accumulating a bitmask of every physical block belonging to that app.
3. Writes that bitmask to `F_BANK_FLG` and commits (`2` to `FLASH_CTRL+0`).
4. Reads the entry point from `FLASH2 + slot*8192 + 0x5C` (the resolved physical block).
5. Validates the block's header a second time via two Thumb helper functions (reached through classic ARM↔Thumb `add lr,pc,#1 / bx lr` interworking trampolines) before deciding whether to jump fresh or resume from an `MCX1` snapshot.
6. Clears user RAM (`0x200-0x7FF`), switches to User mode, sets User SP to `0x800`, and `BX`es to the entry point.

**A single loaded app (`.pss`/`.mcs`) needs a synthesized directory to be reachable this way** - `flash_load_app` builds one: a card header, one directory frame per app block chained starting at slot 1, and the app's own data starting at physical block 1. **Directory frame offset `0x10` (byte 6 of the filename field) must be ASCII `'P'`**, or the BIOS's menu-browsing code (separate from, and running before, the dispatch routine above) won't let a user navigate to/select the slot at all - every other filename byte can be garbage. This mirrors a real PS1 convention: a save bundling a PocketStation app has its product-code's mandatory hyphen (e.g. `SLUS-00892`) replaced with `P`. See `test_flash_load_app_synthesizes_directory`.

## SWI (syscall) mechanism

The vector table at RAM `0x00000000-0x0000001C` is 7 identical `LDR PC,[PC,#0x18]` entries plus one filler; the real handler addresses live in the literal pool right after (reset, undef, SWI, prefetch-abort, data-abort, reserved, IRQ, FIQ, in that order). SWI is at `0x08`.

The real SWI handler: saves `r1-r12,lr`; reads `SPSR`'s `T` bit to compute the original SWI instruction's own address in `lr` (`-2` if Thumb, `-4` if ARM); re-reads that instruction's low byte as a syscall number; looks up a function pointer from a dispatch table whose base address is stored at RAM `0x000000E0` (`table[syscall_number]`); calls it via an interworking `BX`; returns via `LDM SP!,{r1-r12,lr,pc}^` (see "CPU" above).

## Interrupt controller

`0x0A000000`: `hold`, `status`, `enable`, `mask` registers. `intc_irq_asserted`/`intc_fiq_asserted` (`core/src/intc.c`) compute `hold & enable & INT_IRQ_MASK`/`INT_FIQ_MASK` on demand. Bit-to-source mapping (`INT_BTN_*`/`INT_TIMER*`/`INT_RTC`/`INT_IOP`/`INT_IRDA`) matches the official 14-source table exactly - FIQ sources are bit6 (`COM`) and bit13 (`Timer2`); every other source is IRQ.

Both IRQ and FIQ are level-triggered - polled every CPU step (not a one-shot latched request), so the CPU keeps re-entering the handler for as long as the line stays asserted. **FIQ is checked with strictly higher priority than IRQ**, and FIQ entry additionally sets `CPSR.F` (IRQ entry only sets `CPSR.I`) - see `arm7tdmi_step` in `core/src/cpu.c`. `intc_fiq_asserted` itself was implemented and verified correct from early on, but nothing called it until this was fixed; before the fix, FIQ was never delivered by this emulator at all, for any app (see "Hardware ID (F_SN)" below for how this was found). See `test_fiq_delivery_and_priority` and `test_fiq_takes_priority_over_irq`.

Button/RTC sources (`INT_STATUS_MASK`) latch into **both** `hold` and `status` on assertion, and clear from both together on de-assertion.

## Timers

`0x0A800000`+, 3 channels, 0x10 bytes apart: `period`(+0x0), `count`(+0x4), `control`(+0x8: bits0-1 clock divisor - `0`/`3`=`/2`, `1`=`/32`, `2`=`/512`; bit2 enable). `count` only decrements once per `divisor` raw cycles, not once per raw cycle.

Timer1 drives the BIOS's own audio-generation loop **and** doubles as a general GUI tick (e.g. the date-setting screen's blink, the HELLO boot animation) - it is not an audio-exclusive IRQ source. Timer2 is the FIQ-driven timer (see "Interrupt controller" above).

Timer `count` follows raw, `CLK_MODE`-scaled cycles - real timers are clocked by the System Clock, genuinely tied to the CPU's variable clock (see "CLK_MODE" below), unlike RTC/DAC.

## RTC

`0x0B800000`: `mode` bit 0 (`PRGSEL`) selects Run (`0`, ticks at 1Hz, auto-advances the clock) vs Program/pause (`1`, ticks at ~4096Hz, does *not* auto-advance - lets a manual adjust-write step one field without the clock moving underneath it); `mode` bits 1-3 (`CNTSEL`) select which BCD field a `control`/`RTC_ADJUST` write adjusts.

Auto-advance cascades seconds → minutes → hours → day-of-week. It does **not** cascade into `date` on a day rollover - this gap isn't independently documented either way, so it's inherited rather than confirmed-correct.

Real silicon power-on-reset values: `RTCClock = 0x04000000` (day-of-week BCD 4, 00:00:00), `RTCCalendar = 0x00980101` (1998-01-01). `RTC_DATE` bits 24-31 are an unused/unknown field, not a "year-hi"/century byte - the real century lives in battery-backed kernel RAM, surfaced only via the `GetBcdDate` SWI.

RTC ticks at a flat real 1Hz (Run mode) regardless of `CLK_MODE` - a genuinely separate, CPU-clock-independent oscillator.

The BIOS's own "resets itself to Jan 1 1999" behavior ("The RTC Problem", Sony's documented software workaround for inaccurate clock hardware) is a separate, software-driven action taken via the normal `RTC_ADJUST` mechanism - not baked into this emulator's reset state.

## CLK_MODE

`0x0B000000`: bits 0-3 index a 16-entry CPU-frequency table (the real, exactly-documented `PMFrequency`/`SetCpuSpeed` values - not a naive doubling ladder; e.g. mode 1 = 63488 Hz not 65536, mode 7 = 3997696 Hz not 4194304, mode 8 = 7995392 Hz not 8388608, and mode 5→6 only steps ~1.97x, not 2x). Mode 0 = 32.768kHz, treated as the idle default; modes 9-15 alias mode 8's rate. See `core/src/clk.c` for the full table. Readback ORs in a "steady"/PLL-locked bit (`0x10`) - always reports stable, since mode changes are instantaneous here.

Real firmware never actually issues `CLK_MODE=0` - confirmed via a real 20M-instruction boot trace, its very first act is `CLK_MODE=7`, and every subsequent write cycles only between modes 7, 4, and 3. (Official documentation describes mode `00h` as "hangs hardware", an invalid/reserved setting, rather than giving it a frequency - this emulator's own idle-default use of it is harmless in practice since real firmware never triggers that code path.)

**What tracks `CLK_MODE`:** overall CPU instruction throughput (`psemu_run`'s cycle budget), and Timer's count-down rate. **What doesn't** (pinned to real elapsed time instead): RTC (a genuinely separate oscillator) and DAC (this emulator's own audio resampling needs a fixed real-time output rate regardless of the app's chosen CPU speed) - see `test_clk_mode_scales_run_speed`, `test_timer_scales_with_clk_mode`, `test_clk_mode_keeps_rtc_dac_on_real_time`.

## DAC / audio

`0x0D800010`: `ctrl`(+0x0, bit0 enable), `data`(+0x4, bits6-15 signed 10-bit `DACV`, rescaled ×64 to a full `int16` sample range). Real hardware has no square-wave/noise generator or sound DMA channel - software bit-bangs every tone by writing new `DACV` levels directly to `DAC_DATA` at audio rates (typically via the Timer1 IRQ). `dac_tick` resamples the currently-held level (zero-order hold) into a ring buffer at a fixed internal rate, `PSEMU_AUDIO_SAMPLE_RATE_HZ` (8000Hz) - independent of `CLK_MODE`. `PSEMU_ASSUMED_CPU_HZ` (1,056,000 = 33000×32) is the reference cycle rate this conversion, and `psemu_run`'s own per-frame budget (33000 cycles at a 32Hz refresh), are calibrated against - keep both in sync if either changes.

Audio additionally requires `IOP_STOP`/`IOP_START` bit5 ("Sound Enable", `0x0D800004`/`0x0D800008`) to be open, alongside `DAC_CTRL` bit0 - both gates must be open for non-silent output. `IOP_STOP` ORs bits into a shared mask, `IOP_START` ANDs them out; real code writes these via single-byte stores (not always a full 32-bit store), and both registers apply each byte's effect immediately rather than deferring to a full-word write. See `test_iop_sound_gate_mutes_dac`, `test_iop_stop_start_take_effect_via_single_byte_writes`.

## Diagnostics

`arm7tdmi_t` keeps a 256-entry ring buffer of the most recently executed `(pc, cpsr)` pairs, plus a monotonic instruction counter, updated on every step regardless of caller. `psemu_write_crash_report`/`psemu_cpu_faulted` (public API) dump full register state, the fault opcode and its real fetch address if faulted, and this trace. The desktop frontend writes a timestamped `psemu_report_*.log` automatically on a CPU fault, and on-demand via an **F12** hotkey. See `test_crash_report_contents`, `test_cpu_faulted_flag`, `test_faulted_cpu_stops_advancing`.

## Hardware ID (F_SN)

Real hardware carries a per-unit 32-bit serial number in `F_EXTRA` (`FLASH_CTRL+0x300`, 256 bytes): `F_SN_LO`/`F_SN_HI` (`+0x300`/`+0x302`, two 16-bit halves - real code reads them via two separate 16-bit `LDRH`s, never a single 32-bit `LDR`) and `F_CAL` (`+0x308`, LCD calibration). Implemented in `core/src/flash.c`/`flash.h` (`flash_get_serial`/`flash_set_serial`); see `test_flash_serial_number_register_access`.

**Read**: real apps use `SWI 0Ah` (`FlashReadSerial`), or read `F_EXTRA` directly.

**Write**: `SWI 0Fh` (`FlashWriteSerial`) only works on the `061` BIOS revision - see "BIOS/kernel revisions" below; it hangs the CPU on the retail `110` revision. The only working write path on real (retail) hardware is a direct, real 3-step NOR-flash unlock sequence at fixed physical addresses (`F_KEY2`=`0xFFAA`, `F_KEY1`=`0xFF55`, `F_KEY2`=`0xFFA0`) followed by writes to **physical `FLASH2` offset `0`/`2`/`8`** - not `F_EXTRA` where the value is read from. This is documented (psx-spx: *"At physical address 08000000h: `[8000000h]=new F_SN_LO value [8000002h]=new F_SN_HI value`"*) and confirmed by disassembling a real ID-editing homebrew's flash-write routine, and confirmed **working end-to-end on real retail hardware** this session.

Implemented as a **gated** redirect in `flash_write8` (`core/src/flash.c`), not an unconditional address alias - chosen to avoid misrouting any other legitimate write that happens to land on the same three offsets, rather than assuming address alone is a safe-enough signal. An `unlock_step` field on `flash_t` tracks progress through the real 3-step unlock sequence purely by which key address is hit next (the actual values written aren't validated, matching how these addresses have always been treated as commands rather than data). Once armed, a write to physical offset `0`/`2`/`8` redirects to `F_SN_LO`/`F_SN_HI`/`F_CAL` instead of `flash->data[]`; the armed state persists across the 3 halfword writes a real header update performs, and disarms on the first write to any other offset. (One real implementation pitfall along the way: state initially advanced on every *byte* of each key halfword rather than once per halfword, since `psemu_bus_write16`/real `STRH` issues two separate 8-bit bus writes - fixed by only advancing on the low byte.) See `test_flash_header_write_via_unlock_sequence`, `test_flash_header_write_requires_unlock_first`, `test_flash_header_write_disarms_after_unrelated_write`, `test_flash_header_write_requires_correct_key_order`.

**Verified end-to-end** against the real homebrew, a real BIOS, and a real button sequence (`tools/inspect.c`, `button_sim=9`): navigating the on-screen digit cursor to its last position, editing the digit, and committing correctly updates `F_SN` through this gated redirect (`0x410000D3 -> 0x410000D4` after a single edit). This also surfaced the FIQ delivery bug described in "Interrupt controller" above: the homebrew's post-write confirmation beep configures Timer2 (a FIQ source), which produced silence until that fix, and now plays correctly (`DAC_CTRL`/`DAC_DATA` show genuine, continuously-varying activity immediately after the write commits, at Timer2's configured rate).

### Human-readable ID format

A real PocketStation prints its hardware ID on a sticker under the front cover as one ASCII letter followed by 8 decimal digits (user-reported from a real unit: `"A02374684"`) - the letter is `F_SN`'s high byte, the 8 digits are its low 24 bits (max `16777215`) as decimal. This happens to be the same masking Chocobo World's own rank calculation performs against `F_SN`: it reads the register via `SWI 0Ah`, masks off the high byte, and uses the last 3 decimal digits of what remains as its rank-determining "ID" stat (confirmed by disassembling a real copy of the game) - community-documented as best at `211` (also FF8's Japanese release date, 2/11).

`psemu_parse_hardware_id`/`psemu_format_hardware_id` (`core/src/psemu.c`) accept/produce exactly **8 plain hex digits** (`0-9`, `A-F`/`a-f`) - matching exactly what a real ID-editing homebrew itself displays and edits, and able to represent every value the hardware actually allows (confirmed via real-hardware testing: writing `"EEEEEEEE"` persists correctly, a value the letter-prefixed sticker shape can't even represent - `0xEE` isn't an ASCII letter). The sticker form is deliberately **not** accepted by this parser, on input or output - a persisted hardware-ID string (`hardware_id.cfg`) is exactly the raw value, nothing hidden or translated. A sticker-to-raw-value converter, if ever wanted, belongs as a separate desktop-app feature. See `test_hardware_id_string_conversion`.

**Default value: `0x410000D3` (`"410000D3"` in hex form)** - low 24 bits `211`, trivially ending in decimal "211" for the reasons above, giving every fresh Chocobo World save the best rank out of the box. Purely this emulator's own choice (real units ship with an arbitrary factory-assigned serial). There's no other persistent store for this value, since it lives outside the ordinary 128KB card image entirely - the desktop frontend persists it in `hardware_id.cfg` (8-hex-digit form) next to the executable; libretro has no such mechanism and always gets the core default.

## BIOS/kernel revisions

Two documented BIOS/kernel ROM revisions exist, identified by ASCII tag strings baked into the ROM itself: Core Kernel Version at BIOS offset `0x1DFC` (`"C061"` or `"C110"`), Japanese GUI Version at `0x3FFC` (`"J061"` or `"J110"`) - but only `110` was ever shipped in a retail unit. The `061` dump in circulation is documented as a prototype-hardware dump that doesn't work correctly with some games, not a real retail BIOS; the real BIOS dump used for this project's own testing is the `110` revision. These are factory mask-ROM revisions, not a user/field-applied patch - `BIOS_ROM` is genuine ROM, not the writable `FLASH` region, and no update mechanism (disc-based flasher, service program) is documented anywhere. `"110"` contains "patches" relative to `"061"` while preserving the same SWI dispatch-table addresses - a deliberately binary-compatible bugfix revision, consistent with `061` predating retail release.

The concrete, confirmed difference: `SWI 0Fh` (`FlashWriteSerial`) only works on the `061` revision - on `110` it's "padded with jump opcodes which hang the CPU in endless loops." Not merely unsupported; calling it on a `110` BIOS freezes the device. A real ID-editing homebrew never calls it anywhere in its code (confirmed by exhaustively scanning its binary for the `SWI #0xF` opcode - zero hits), with no runtime BIOS-version detection in the binary at all - it avoids the call unconditionally, simply because it was always built that way.

## Known open questions and unconfirmed behavior

- **Chocobo World event-screen crash (counter overflow), not fully confirmed.** A real, reproducible CPU fault (a stale-`LR` wild branch into an orphaned Thumb `BL` half-instruction) was traced to a command-dispatcher call with an out-of-range value (`0x200`) - itself the current value of a small RAM-resident counter (`0x332`, in the app's own private user-RAM state, not a documented kernel construct) that's consumed one at a time and normally expected to stay within `0`-`0x13`. This was reached only after a very long (1.3-billion-instruction), deterministically-seeded pseudo-random button-mashing run - well-supported but **not confirmed**: the counter's increment site was never traced, so whether it genuinely requires faster-than-human input to overflow (a latent bug in Chocobo World's own code, not this emulator) remains a hypothesis. If revisited: watch RAM `0x332` for writes across a fresh `button_sim=6` run in `tools/inspect.c`, and check whether increments are tied to button input (supports the hypothesis) or to a timer/frame tick (would instead implicate this emulator's own timing accuracy).
- **Chocobo World's own in-game sound beyond a single ~17ms launch chime is unconfirmed.** No further DAC activity was observed across 200M+ instructions of generic automated exploration after that one chime. Plausibly gated behind real gameplay states this project's generic button-mashing exploration doesn't reach - needs real interactive play focused on deeper gameplay to confirm either way.
- **IR (`0x0C800000`+)** only stores `IRDA_MODE`/`IRDA_DATA` under their documented bit layout - no pulse-level timing, no `INT_IRDA` firing, and no transport between two emulator instances. Left unimplemented: nothing in this project's own test corpus (200M+ traced instructions) has ever touched these registers, and there's no second real PocketStation to validate a modeled protocol against.
- **`F_BANK_VAL` mapping multiple physical blocks to the same virtual slot** has explicitly undocumented real hardware behavior ("maybe the data becomes ANDed together"). `flash_resolve_physical_bank` just returns the lowest-indexed matching physical block in that case - a reasonable but arbitrary choice among the documented unknowns, not a confirmed-correct model.
- **`FLASH_CTRL+0`'s "always read back bit0=1" behavior** is empirically necessary (it unblocks a real BIOS busy-wait) but doesn't cleanly correspond to the official `REGRemap` register's documented `GENREM`/`FLASHVIR` bit semantics - don't assume it's spec-accurate.
- **The real BIOS-mirrored-at-address-0 + `GENREM` remap dance** (BIOS ROM aliased to `0x00000000` until the kernel remaps RAM in) isn't modeled - `psemu_reset` starts the CPU directly at `PSEMU_BIOS_BASE`, skipping the pre-remap phase entirely. A deliberate simplification with no demonstrated behavioral cost across 150M+ traced instructions, not an oversight.
- **`BATT_CTRL`** (`0x0D800020`, low-voltage detection) is unmodeled (reads 0, writes no-op) - no real behavior depends on battery-level sensing in an emulator with no actual battery.
- **Per-instruction cycle timing is approximate, not cycle-accurate** (see `PSEMU_ASSUMED_CPU_HZ` above) - real-time pacing (audio, animation) has been tuned to look/sound right, not derived from a cycle-exact model.

## Licensing note

This project is GPLv3 ([LICENSE](../LICENSE)). BSD-3 code can be referenced/adapted into a GPLv3 project with attribution — that direction is compatible. Do not pull code from another open-source implementation (unlicensed) or lean on other closed-source docs beyond documentation-level facts (all closed-source, no code available anyway).
