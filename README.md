# pokketstation

An open-source Sony PocketStation emulator core, written in portable C, meant to be reused across a [libretro](https://www.libretro.com/) core, a standalone Windows desktop app, and a PS Vita homebrew port.

## Status

Early scaffold. The memory map, PSX Title Sector app loading, LCD, buttons, and flash are wired up end to end; the ARM7TDMI CPU interpreter itself is a stub (fetches and advances PC but doesn't decode instructions yet) — see `core/src/cpu.c` and [docs/hardware-notes.md](docs/hardware-notes.md) for what's next.

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
docs/hardware-notes.md   hardware reference (memory map, file format, sources)
```

## Building

```
cmake -B build
cmake --build build
ctest --test-dir build
```

The desktop frontend needs SDL2 (`vcpkg install sdl2` or your system package manager) and is skipped with a warning if it's not found. The libretro core fetches `libretro-common` at configure time via `FetchContent`. The Vita frontend only builds when configured with the vitasdk toolchain file — see [frontends/vita/README.md](frontends/vita/README.md).

None of the frontends bundle a PocketStation BIOS — it's copyrighted Sony firmware and must be supplied by the user from their own hardware (desktop: as a CLI argument; libretro: as `pocketstation.bin` in RetroArch's system directory).

## License

GPLv3, see [LICENSE](LICENSE). [docs/hardware-notes.md](docs/hardware-notes.md) has a licensing note on what prior art is safe to reference.
