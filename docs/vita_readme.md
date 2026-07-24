# Vita frontend

This is very much WIP and broken. Vita will be revisited once the core and desktop versions are feature complete - hopefully soon.

Requires [vitasdk](https://vitasdk.org/) and [vita2d](https://github.com/xerpi/vita2d) installed via `vdpm`.

```
vdpm install vita2d
export VITASDK=/usr/local/vitasdk   # or wherever vdpm installed it
cmake -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build-vita --target pokketstation_vita.vpk
```

Sideload the resulting `.vpk` with VitaShell on a device running h-encore/HENkaku. Distribute finished builds through [VitaDB](https://vitadb.rinnegatamante.it/) rather than attaching a BIOS dump — the BIOS is copyrighted Sony firmware and must be supplied by the user from their own hardware.
