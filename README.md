# pokketstation

An open-source Sony PocketStation emulator core, written in portable C, meant to be reused across a [libretro](https://www.libretro.com/) core, a standalone Windows desktop app, and a PS Vita homebrew port (WIP).

## Status

Stable, broadly community tested and as cycle accurate as I can make it. LGTM and I hope you will agree.

See [docs/hardware-notes.md](docs/hardware-notes.md) for the technical details.

**Known gaps:** IR communication timing is unverified, per-instruction cycle timing is still approximate rather than cycle-accurate (see `PSEMU_ASSUMED_CPU_HZ` in `docs/hardware-notes.md`), and a handful of edge cases (low-battery detection, `F_BANK_VAL` entries mapping multiple physical blocks to the same virtual slot, the BIOS's pre-remap boot phase) are deliberately simplified — see the "Confirmed real gaps" list at the end of `docs/hardware-notes.md` for specifics.

If you hit a crash or an unrecognized-opcode freeze, please [open an issue](https://github.com/mentalfoundry/pokketstation/issues) — include a [diagnostic report](docs/desktop_readme.md#diagnostic-reports-for-bug-reports) if you can, it makes tracking down the real bug far easier.

## Usage

Download the latest [release](https://github.com/mentalfoundry/pokketstation/releases) package for your platform, then follow the usage guide for whichever frontend you downloaded. **Neither bundles a PocketStation BIOS** — it's copyrighted Sony firmware, so you need to supply your own dump extracted from real hardware.

- [Desktop app](docs/desktop_readme.md#usage)
- [Libretro core](docs/libretro_readme.md#usage)

### Reaching a single loaded app

A single-app load (`.pss`/`.mcs`, either frontend) boots through the real BIOS the same way a full memory card does — there's no shortcut past it. The real, hardware-confirmed sequence (see `docs/hardware-notes.md`): after the HELLO/heart/beep power-on animation, press **Down** once then **Action** to get past the date/time screen, then **Right** once to move from the clock screen to the app (**Action** launches it from there). Real taps are brief (~40ms) but a deliberate, clean press-and-release reads more reliably than mashing.

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
```

## Building

Full build instructions (prerequisites, Windows/Linux/macOS steps) live in the [desktop frontend's build guide](docs/desktop_readme.md#building) — the core, tools, tests, and the libretro core all build the same way, with no extra dependencies beyond what's documented there.

- [Desktop app](docs/desktop_readme.md) — usage, building, and diagnostic reports
- [Libretro core](docs/libretro_readme.md) — usage and building; one extra step versus the desktop app (fetches `libretro-common` on first configure, needs internet access)

## License

GPLv3, see [LICENSE](LICENSE). [docs/hardware-notes.md](docs/hardware-notes.md) has a licensing note on what prior art is safe to reference.
