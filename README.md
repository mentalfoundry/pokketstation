# pokketstation

This is an open-source Sony PocketStation emulator core, written in portable C. It is made for use in a [libretro](https://www.libretro.com/) core, a standalone Windows desktop app, and a PS Vita homebrew port (in progress).

## Status

This project is stable, and the community has tested it widely. It is as cycle-accurate as possible. If you need a feature that is not present, please [raise an issue](https://github.com/mentalfoundry/pokketstation/issues).

IR now operates locally. First, set the receive side into receive mode in your apps. The send side is not very tolerant, the same as on real hardware. The tests used two apps; your results can be different.

Do not depend on save-state compatibility between versions yet. The internal registries need more changes.

The cycle timing of each instruction follows the recorded memory-access-time table, and the standard ARM7TDMI instruction-class formulas. It does not use an approximation of 1 cycle for each instruction. Tests on real hardware, with the `pk_timing_bench` app of this project, confirm the two values that the documentation left unclear. See "Memory access timing" in `docs/hardware-notes.md`.

See [docs/hardware-notes.md](docs/hardware-notes.md) for the technical data.

**Known gaps:**
- The IR communication timing is unverified. I have only 1 PocketStation device, thus this is the best result that is possible without a second device. The core has hardware-tested behavior, and the desktop emulator does most of the work. I will try to get a second device when I have the opportunity.
- This emulator makes a few edge cases simpler than the real hardware: low-battery detection, `F_BANK_VAL` entries that map more than one physical block to the same virtual slot, and the pre-remap boot phase of the BIOS.

See the "Known open questions and unconfirmed behavior" list at the end of `docs/hardware-notes.md` for the details.

If the emulator stops, or if it stops with an unrecognized-opcode fault, please [open an issue](https://github.com/mentalfoundry/pokketstation/issues). Include a [diagnostic report](docs/desktop_readme.md#diagnostic-reports-for-bug-reports) if you can. A report makes the fault much easier to find.

## Usage

Download the latest [release](https://github.com/mentalfoundry/pokketstation/releases) package for your platform. Then follow the usage guide for the frontend that you downloaded. **Neither package contains a PocketStation BIOS.** The BIOS is copyrighted Sony firmware. You must supply your own dump, from real hardware.

- [Desktop app](docs/desktop_readme.md#usage)
- [Libretro core](docs/libretro_readme.md#usage)

The release packages hold the desktop app and the libretro core for Windows, Linux, and
macOS. For Android and the other systems that libretro supports, get the core from the
[pipelines of the libretro
repository](https://git.libretro.com/libretro/pokketstation/-/pipelines). See [Prebuilt
cores](docs/libretro_readme.md#prebuilt-cores) for the steps.

### How to reach a single loaded app

A single-app load (`.pss` or `.mcs`, in either frontend) boots through the real BIOS, the same way as a full memory card. There is no method to go past the BIOS.

**The real button sequence, which hardware tests confirm** (see `docs/hardware-notes.md`):

1. Wait for the end of the HELLO, heart, and sound power-on animation.
2. Press **Down** one time, and then **Action**, to go past the date/time screen.
3. Press **Right** one time, to move from the clock screen to the app.
4. Press **Action** to start the app.

A real button press is short, approximately 40ms. A clean press and release is more reliable than many fast presses.

## Layout

```
core/                    portable C99 emulation core, with no OS or graphics dependencies
  include/psemu/psemu.h  the public interface
  src/                   CPU, memory map, LCD, buttons, flash, IR
frontends/
  libretro/              the libretro core wrapper
  desktop/               the SDL2 desktop app for Windows, Linux, and macOS
  vita/                  the PS Vita port (vita2d), built with the vitasdk toolchain
tests/                   a smoke test that exercises the public interface
docs/hardware-notes.md   the hardware reference (memory map, file format, sources), and the readme files for each frontend
pk_timing_bench/         a homebrew memory-timing benchmark app (build source and real-hardware results)
```

## Building

The [build guide of the desktop frontend](docs/desktop_readme.md#building) gives the full build instructions: the prerequisites, and the steps for Windows, Linux, and macOS. The core, the tools, the tests, and the libretro core all build the same way. They need no dependencies except the dependencies in that guide.

- [Desktop app](docs/desktop_readme.md): usage, building, and diagnostic reports.
- [Libretro core](docs/libretro_readme.md): usage and building. It has one extra step: it gets `libretro-common` at the first configure operation, and that step needs internet access.

## License

The license is GPLv3. See [LICENSE](LICENSE). [docs/hardware-notes.md](docs/hardware-notes.md) has a licensing note about the prior art that is safe to reference.
