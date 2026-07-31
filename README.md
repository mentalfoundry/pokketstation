# pokketstation

This is an open-source Sony PocketStation emulator core, written in portable C. It is meant for reuse across a [libretro](https://www.libretro.com/) core, a standalone Windows desktop app, and a PS Vita homebrew port (work in progress).

## Status

This project is stable, broadly tested by the community, and as cycle-accurate as possible. If you have requests for missing features, please [raise an issue](https://github.com/mentalfoundry/pokketstation/issues).

IR now works locally! Be sure to set the recieving side into recieving mode first within your apps. The send side does not appear to be very forgiving (like in the real world). Tested against my favorite app - you're mileage may vary.

Do not rely on save state compatibility between versions just yet. Apologies but there are still likely a few more iterations of the internal registries to go.

See [docs/hardware-notes.md](docs/hardware-notes.md) for the technical details.

**Known gaps:**
- IR communication timing is unverified - I only have 1 pocketstation device so this is as good as I can make it without another one. The core is hardware tested behavior while the desktop emulator does the heavy lifting. I will try to procure a second device once the opportunity arises.
- Per-instruction cycle timing follows the documented memory-access-time table, with one assumed default where the documentation itself gives no answer (see "Memory access timing" in `docs/hardware-notes.md`).
- A handful of edge cases are deliberately simplified: low-battery detection, `F_BANK_VAL` entries that map multiple physical blocks to the same virtual slot, and the BIOS's pre-remap boot phase.

See the "Known open questions and unconfirmed behavior" list at the end of `docs/hardware-notes.md` for specifics.

If you hit a crash or an unrecognized-opcode freeze, please [open an issue](https://github.com/mentalfoundry/pokketstation/issues). Include a [diagnostic report](docs/desktop_readme.md#diagnostic-reports-for-bug-reports) if you can; it makes finding the real bug far easier.

## Usage

Download the latest [release](https://github.com/mentalfoundry/pokketstation/releases) package for your platform. Then follow the usage guide for whichever frontend you downloaded. **Neither bundles a PocketStation BIOS.** The BIOS is copyrighted Sony firmware; you must supply your own dump, extracted from real hardware.

- [Desktop app](docs/desktop_readme.md#usage)
- [Libretro core](docs/libretro_readme.md#usage)

### Reaching a single loaded app

A single-app load (`.pss`/`.mcs`, either frontend) boots through the real BIOS the same way a full memory card does. There is no shortcut past it.

**The real, hardware-confirmed button sequence** (see `docs/hardware-notes.md`):

1. Wait for the HELLO/heart/beep power-on animation to finish.
2. Press **Down** once, then **Action**, to get past the date/time screen.
3. Press **Right** once, to move from the clock screen to the app.
4. Press **Action** to launch it.

Real button taps are brief, approximately 40ms. A deliberate, clean press-and-release reads more reliably than mashing the button.

## Layout

```
core/                    portable C99 emulation core, no OS/graphics dependencies
  include/psemu/psemu.h  public API
  src/                   CPU, memory map, LCD, buttons, flash, IR
frontends/
  libretro/              libretro core wrapper
  desktop/               SDL2-based Windows/Linux/macOS desktop app
  vita/                  PS Vita port (vita2d), built via the vitasdk toolchain
tests/                   smoke test exercising the public API
docs/hardware-notes.md   hardware reference (memory map, file format, sources), frontend specific readmes
pk_timing_bench/         homebrew memory-timing benchmark app (build source + real-hardware findings)
```

## Building

Full build instructions (prerequisites, Windows/Linux/macOS steps) live in the [desktop frontend's build guide](docs/desktop_readme.md#building). The core, tools, tests, and the libretro core all build the same way, with no extra dependencies beyond what is documented there.

- [Desktop app](docs/desktop_readme.md): usage, building, and diagnostic reports.
- [Libretro core](docs/libretro_readme.md): usage and building. One extra step versus the desktop app: it fetches `libretro-common` on first configure, and needs internet access for that.

## License

Licensed GPLv3; see [LICENSE](LICENSE). [docs/hardware-notes.md](docs/hardware-notes.md) has a licensing note on what prior art is safe to reference.
