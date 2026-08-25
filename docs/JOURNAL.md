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
  libxcrypt port autoreconfs with, plus its libltdl-dev (ADR-018). Both
  spec docs amended in this PR per 03-process.md §3.3. Second run: Windows
  built but FontAssets failed on a CRLF checkout of SHA256SUMS — parser
  strips CR and .gitattributes pins vendored bytes / LF for the sums file.

- Mid-milestone: branch protection PUT on main returned 404 (token lacks
  admin; 2026-08-25). Fallback per 03-process.md §3.2: the §4 per-PR
  CI-green checklist enforces merges; six contexts confirmed verbatim from
  gh pr checks on PR #2. Retry once at M01.

## M01 — App skeleton & fixed-timestep loop (2026-08-25)

- Shipped: core (time/log/rng/config/assert), sim (SimSnapshot +
  TripleBuffer, 1000 Hz SimThread with §6.1 clamp), platform (CLI §2,
  SDL boot §1 subset, window, GPU device w/ FIF=1 + MAILBOX-else-VSYNC),
  render (renderer.h per 06 §2, quad pipeline from committed blobs,
  stb_easy_font overlay, --render-smoke offscreen path), 14 unit tests.
- Deviations:
  - Timebase unified as tb::now_ns() in tb_core over CLOCK_MONOTONIC/QPC
    (05 §3 said SDL_GetTicksNS; 04 M1 said ticks_now_ns — neither fits the
    layering) → ADR-019; both spec snippets amended same PR.
  - SDL_shadercross has no release tags; FetchContent pins commit
    e55cf5e31ced6f3d1be5cc6d0c50e99384f9f4ba instead (ADR-012's "fixed
    release tag" applied as fixed commit).
  - Blobs generated locally: dxc v1.8.2505 → SPIR-V, spirv-cross → MSL;
    DXIL deferred (needs Windows DXC); CI shader compile stays OFF until a
    runner-provisioned DXC exists → committed-blob path per ADR-012.
    Windows/macOS render-smoke will log-skip until DXIL lands; Linux
    exercises lavapipe.
  - --headless runs a bounded 5000-tick probe then exits 0 so CI can
    verify display-less boot; unbounded headless sessions arrive with
    --replay (M4+).
  - Branch protection retry: PUT still 404 (no admin) at M01 open; fallback
    continues per §3.2.
