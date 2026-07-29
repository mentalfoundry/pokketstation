# Vita frontend

This frontend is work in progress and currently broken. This project will revisit Vita support once the core and desktop versions reach feature completion.

This build requires [vitasdk](https://vitasdk.org/) and [vita2d](https://github.com/xerpi/vita2d), installed through `vdpm`.

```
vdpm install vita2d
export VITASDK=/usr/local/vitasdk   # or wherever vdpm installed it
cmake -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build-vita --target pokketstation_vita.vpk
```

Sideload the resulting `.vpk` with VitaShell, on a device running h-encore/HENkaku.

Distribute finished builds through [VitaDB](https://vitadb.rinnegatamante.it/). Do not attach a BIOS dump to a distributed build. The BIOS is copyrighted Sony firmware. Each user must supply it from their own hardware.
