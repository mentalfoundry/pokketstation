#!/bin/sh
# Builds the core as one JavaScript file for a web page.
#
# The WebAssembly module is inside the JavaScript file (SINGLE_FILE). Some static hosts do not
# accept a .wasm file, and a single file has no path to configure.
#
# The module is in base64 (SINGLE_FILE_BINARY_ENCODE=0). The binary encoding is correct only when
# the host gives the file a UTF-8 charset, and some static hosts give no charset.
#
# The build uses the core from git HEAD, not from the working tree. Thus a published build
# contains only committed code.
#
# The module has no threads. Some static hosts permit only blob: workers, and the machine is
# small enough to run in the animation loop of the page.
#
# Usage: build.sh [output.js]
# Needs: emcc on the PATH (Emscripten), git, tar.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=${1:-"$here/pokketstation-core.js"}

src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
git -C "$repo" archive HEAD core | tar -x -C "$src"

emcc -O3 -std=c99 -DNDEBUG \
    -I"$src/core/include" -I"$src/core/src" \
    "$src"/core/src/*.c \
    -sMODULARIZE=1 \
    -sEXPORT_NAME=createPokketstationCore \
    -sSINGLE_FILE=1 \
    -sSINGLE_FILE_BINARY_ENCODE=0 \
    -sENVIRONMENT=web \
    -sFILESYSTEM=0 \
    -sEXPORTED_FUNCTIONS=_malloc,_free,_psemu_create,_psemu_destroy,_psemu_reset,_psemu_load_bios,_psemu_load_content,_psemu_identify_content,_psemu_set_buttons,_psemu_run,_psemu_get_framebuffer,_psemu_get_audio_samples,_psemu_flash_data,_psemu_save_flash_image,_psemu_save_app_image,_psemu_cpu_faulted,_psemu_settings_offsets_known,_psemu_app_running,_psemu_set_datetime \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP16 \
    -o "$out"

echo "core $(git -C "$repo" rev-parse --short HEAD) -> $out"
