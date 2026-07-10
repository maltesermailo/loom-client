# loom-client

C++20 client for Loom. This repo is being built up in small, reviewed steps;
this session covers only the protocol layer (`proto/`) and its vector adapter.

## Layout
- `proto/` — independent wire-protocol implementation (no QUIC, no I/O).
- `core/` — jitter buffer, session state machine, clock sync (placeholder).
- `tests/` — doctest unit tests (`proto_tests`).
- `spec/` — the normative spec + conformance vectors, pinned as a submodule.

## Decisions
- **Test framework: doctest** (single header, fastest compile), fetched at
  configure time via CMake `FetchContent`, pinned to `v2.4.11`.
- **Error handling: `loom::Result<T,E>`** (`proto/include/loom/proto/result.hpp`)
  — C++20 lacks `std::expected`; a small `std::variant`-backed type keeps the
  protocol layer dependency-free and exception-free across module boundaries.

## Build
```
cmake --preset host-tools
cmake --build --preset host-tools
ctest --preset host-tools
```
`sdl` and `quest` are placeholder presets until those clients are implemented.
