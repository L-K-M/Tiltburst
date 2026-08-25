# Tiltburst Journal (append-only)

Newest entries at the bottom. Never edit or delete past entries;
corrections are new entries. Format: 03-process.md §3.1.

## M00 — Repository scaffold & CI (2026-08-25)

- Shipped: CMake scaffold with all canonical targets, vcpkg manifest
  (baseline 00c5775211f45cd08b37fce0484b4cb940e422ab), CMakePresets,
  tb-setup composite action + build-test.yml workflow (format / build-test ×3 OS /
  asan / perf-gates / weekly-deep / det-soak), tiltburst --version +
  exit-2 catch-all, three tool stubs, unit_scaffold.sanity,
  FontAssets.VendoredFontsPresentAndParse, vendored fonts + picosha2,
  CHANGELOG/JOURNAL seeds.
- Deviations:
  - Orbitron static Bold no longer exists upstream; vendored the variable
    font would violate 13-art-direction.md §5.1, so the orbitron role is
    filled by Chakra Petch Bold per §5.1's substitution clause → ADR-015.
    Logical font name stays `orbitron`.
  - picosha2.h is MIT-licensed upstream (plan said "public-domain");
    license recorded verbatim in tests/third_party/SOURCES.md.
- New ADRs: ADR-015, ADR-016, ADR-017, ADR-018.
- Worries: local dev box has no sudo; CI is the primary build gate for
  Windows/macOS until a machine with MSVC/Xcode is available.
- Mid-milestone: first CI run red — windows-latest migrated to VS2026
  (generator fixed per ADR-017) and Linux jobs lacked the autotools the
  libxcrypt port autoreconfs with (ADR-018). Both spec docs amended in this
  PR per 03-process.md §3.3.

