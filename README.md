# Tiltburst

A cross-platform digital pinball game for converted real cabinets and
ordinary desktops: neon retro style, particle effects, five original tables,
local 1–4 player multiplayer, and a fully text-based table format that both
LLMs and humans can author.

> [!IMPORTANT]
> LLM disclosure: This codebase was written with substantial help from large language models: AI coding agents working from the [`AGENTS.md`](AGENTS.md) brief in this repo.

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

Tiltburst's own code and content are public domain
([Unlicense](LICENSE)). The Unlicense covers Tiltburst's own files only.
These files in the repository are third-party and keep their own
licenses:

- the fonts in `assets/fonts/` (SIL Open Font License 1.1), each with its
  license text alongside and recorded in `assets/fonts/SOURCES.md`;
- `tests/third_party/picosha2.h` (MIT), used only by the tests and
  recorded in `tests/third_party/SOURCES.md`.

Built binaries link SDL3, Lua, sol2, fmt, nlohmann-json, miniaudio and
stb from vcpkg, which keep their own licenses. No binaries are
distributed yet. The release archives planned for milestone M19 will
carry their notices as `Third-Party-Notices.txt` with a copy of
`LICENSE`, next to the binary or in the macOS app's `Contents/Resources`;
the font license texts stay in `assets/fonts/`.
