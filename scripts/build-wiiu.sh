#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd "${script_dir}/.." && pwd)"
devkitpro_dir="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPRO="${devkitpro_dir}"
export DEVKITPPC="${DEVKITPPC:-${DEVKITPRO}/devkitPPC}"
wiiu_cmake="${devkitpro_dir}/portlibs/wiiu/bin/powerpc-eabi-cmake"

if [[ ! -x "${wiiu_cmake}" ]]; then
    echo "Cafe Engine: Wii U toolchain not found at ${wiiu_cmake}" >&2
    echo "Install devkitPro's wiiu-dev package or set DEVKITPRO." >&2
    exit 1
fi

"${wiiu_cmake}" -S "${project_dir}" -B "${project_dir}/build-wiiu" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "${project_dir}/build-wiiu" --parallel
cmake --install "${project_dir}/build-wiiu" \
    --prefix "${project_dir}/dist/wiiu/apps"

echo "Copy dist/wiiu to the SD card root:"
echo "  Aroma:           sd:/wiiu/apps/hello_world.wuhb"
echo "  Homebrew Launcher: sd:/wiiu/apps/hello_world/"

