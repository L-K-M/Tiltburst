# Tiltburst

A cross-platform digital pinball game for converted real cabinets and
ordinary desktops: neon retro style, particle effects, five original tables,
local 1–4 player multiplayer, and a fully text-based table format that both
LLMs and humans can author.

## Status

Implementation in progress, milestone by milestone (see
[PLAN.md](PLAN.md) and [docs/plan/](docs/plan/)). The current tree is an
early scaffold; nothing playable yet.

## Building

Prerequisite: a [vcpkg](https://github.com/microsoft/vcpkg) checkout
anywhere on disk, exported as `VCPKG_ROOT`:

```sh
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh        # Windows: bootstrap-vcpkg.bat
export VCPKG_ROOT=~/vcpkg         # PowerShell: $env:VCPKG_ROOT = "C:\path\to\vcpkg"
```

Then:

```sh
# Linux / macOS
cmake --preset release && cmake --build --preset release && ctest --preset release

# Windows (Visual Studio generator)
cmake --preset windows && cmake --build --preset windows-release && ctest --preset windows-release

# Debug builds and sanitizers
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan
```

The presets are OS-conditioned; `CMakePresets.json` is authoritative.

## License

Public domain ([Unlicense](LICENSE)); third-party assets carry their own
licenses — see `assets/fonts/SOURCES.md` and `tests/third_party/SOURCES.md`.
