# Changelog

All notable changes to Tiltburst are documented here. Format:
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning: the
project versions the product, not the library API; v1.0.0 is milestone M20.

## [Unreleased]

### Added

- M00: CMake/vcpkg build scaffold with all canonical targets, CI workflow
  (3-OS matrix, format, ASan, perf gates), `tiltburst --version`, tool
  stubs, first tests, vendored OFL fonts with provenance and SHA-256 pins.

### Fixed

- M00: Windows preset generator follows the runner image's Visual Studio
  2026 (ADR-017); tb-setup installs the autotools required by vcpkg's
  libxcrypt port (ADR-018); vendored `third_party/` headers exempt from the
  clang-format gate (ADR-016).

