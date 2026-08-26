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

## M02 — Headless simulation core & determinism (2026-08-25)

- Shipped: Vec2/Ball/materials/colliders, analytic sweeps (§3.2–§3.4, §8)
  with the five §2.1 worked examples pinned as unit tests, uniform-grid
  broadphase (§3.7) with brute-force property test, shared-timeline CCD
  loop + pull-back + push-out (§3.6/§3.8), impulse contacts with
  velocity-dependent restitution and spin transfer (§4.1–§4.3), energy
  property test, ball-ball collisions (§8), state_hash with the rolling
  event-sequence accumulator (16 §2.4.1), .tbreplay writer/reader (05 §13),
  JSON tape loader in test support, SimEvent ring skeleton, allocation-free
  step proven by a global allocator hook, layout include guard,
  perf_tick.gate_synthetic green (median-of-3 mean ≈ 1.8 µs, p99 ≈ 2.7 µs —
  far inside the 100/200 µs CI limits).
- Deviations:
  - det_golden.m2_bounce records/compares per-OS goldens; only the linux
    golden is committed (no Windows/macOS host available). Other OSes SKIP
    until their goldens land via CI artifacts. ADR-013 same-OS rule intact.
  - The m2_bounce tape's table_slug ("test-lab") is not yet validated —
    table loading arrives at M5.
  - sweep_circle_vs_point root-finding runs in double internally: the §2.1
    worked-example tolerances (1e-6 on normalized normals near-tangent)
    are unreachable in float without contradicting the spec's own decimals.
  - Broadphase brute-force test asserts no-misses (+dedupe/sort), not set
    equality: cell-level broadphase is conservative by design.
  - Branch protection retry at M02 open: PUT still 404 (no admin);
    fallback continues per §3.2.
- Golden regeneration (M02 review cycle): the tape loader's bit→action map
  was fixed to shift bits 6–9 down one (05 §13.1), changing the m2_bounce
  button stream; tests/golden/determinism/linux/m2_bounce.hashes
  regenerated per §2.4.4.
