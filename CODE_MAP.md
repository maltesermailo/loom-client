# loom-client — Code Navigation Map

**Status:** Informative developer aid, not a contract. Regenerate/extend as files land.
**Scope:** every `.hpp`/`.cpp` in `loom-client` (`proto/`, `core/`, `sdl/`, `tests/`);
excludes the `spec/` submodule and `build/`.
**Normative sources it points at:** `spec/PROTOCOL.md`, `spec/ARCHITECTURE.md`,
`spec/PAIRING.md`, `spec/VECTORS.md`. When this map and the spec disagree, the spec wins.
**Sibling doc:** the Rust host has its own `CODE_MAP.md`; `proto/` here is the C++ twin of
`loom-proto` there, kept in agreement byte-for-byte by the shared conformance vectors.

## How to use this

Each entry is **path** — one-line purpose, then key items to jump to and the spec sections it
implements. `file:NN` references are clickable in most editors. Public API lives in headers
(`*.hpp`); the paired `*.cpp` holds the implementation.

## Conventions (why the code looks the way it does)

- **C++20, no exceptions across the protocol boundary.** Fallible calls return
  `loom::Result<T, E>` (there is no `std::expected` until C++23). See `proto/result.hpp`.
- **The wire layer is dependency-free and I/O-free.** `loom_proto` touches no filesystem, no
  sockets; only the vector-adapter binary and tests pull in nlohmann/json + doctest.
- **Namespaces:** `loom::proto` (wire), `loom::proto::cbor|clocksync|keymap|hex`,
  `loom::core` (session logic), `loom::sdl` (desktop transport + app).

## The 30-second mental model

```
                    ┌──────────────────────────────────────────────┐
   wire bytes <────>│ proto/  loom_proto  (pure: no I/O, no QUIC)   │
                    │  cbor · datagram · control · reassembly ·     │
                    │  clocksync · keymap · errors · result · hex   │
                    └───────────────▲──────────────────────────────┘
                                    │ encode/decode only
                    ┌───────────────┴──────────────────────────────┐
                    │ core/  loom_core  (sans-io session SM)        │
                    │  Session: on_control_bytes/on_event → poll()  │
                    └───────────────▲──────────────────────────────┘
                                    │ ITransport seam (bytes + events)
        ┌───────────────────────────┴───────────────┐   (Quest reuses core/ + proto/
   QUIC │ sdl/   loom-sdl  (msquic transport + app)  │    behind its own ITransport,
  <────>│  transport.hpp · msquic_transport · main   │    added in M3 — not here yet)
        └────────────────────────────────────────────┘
```

The design rule: **transports move bytes, `core` decides, `proto` encodes.** `core::Session`
knows nothing about QUIC, so the same object runs under SDL/msquic here and under
msquic-on-Android on the Quest, untouched.

---

# `proto/` — the wire protocol (library `loom_proto`)

Independent C++ implementation of the same contract as the Rust `loom-proto`. Headers under
`proto/include/loom/proto/`, sources under `proto/src/`.

### `proto/include/loom/proto/result.hpp` — the error-handling primitive ★ start here
- `Ok<T>` / `Err<E>` wrappers + deduction guides; `Result<T, E>` (`variant`-backed) with
  `has_value()`, `value()`, `error()`. Never throws across module boundaries.

### `proto/include/loom/proto/cbor.hpp` + `proto/src/cbor.cpp` — canonical CBOR (§3.2)
- `class Value` — the CBOR shapes the control protocol uses; named factories
  (`integer/text/bytes/array/map/…`) avoid bool/int overload traps; `type()` + typed accessors.
- `encode_canonical(Value) -> bytes` — canonical form (definite lengths, shortest ints, sorted
  keys, shortest-form floats). `decode(bytes) -> optional<Value>` — tolerant; rejects
  indefinite-length items.
- `.cpp`: factories/accessors (`cbor.cpp:9-21`), the hand-rolled encoder, and the decoder.

### `proto/include/loom/proto/datagram.hpp` + `proto/src/datagram.cpp` — datagram framing (§4)
- Constants `kMagic=0x4C`, `kFlagKeyframe`, `kFlagLastFragment`, `kHeaderLen=12`,
  `kMaxDatagramLen=1350`.
- `struct DatagramHeader` (keyframe, last_fragment, stream_id, frame_seq, frag_index, frag_count);
  `make_header` derives `last_fragment` from position; `encode_header` / `encode_datagram`.
- `enum class DropReason` + `to_string` — the stable strings the vectors assert; failures are
  **silent drops** (§6.6), so `decode(bytes) -> Result<DecodedDatagram, DropReason>`
  (`datagram.cpp:69`) validates in the normative order.

### `proto/include/loom/proto/control.hpp` + `proto/src/control.cpp` — control framing + registry (§3)
- `kMaxFrameBody=65536`; message-type constants `kHello`…`kPairResult` (§3.3).
- `known_keys(msg_type)` (`control.cpp:5`) — defined body keys per type; unknown keys stripped,
  unknown types → `Decoded::Kind::Ignored` (§3.2).
- `struct Decoded { Kind, msg_type, body }`; `enum class ControlError { protocol_violation }`.
- `encode_frame(msg_type, body) -> bytes` (`control.cpp:42`) — canonical `[msg_type, body]` +
  `u32` length prefix. **`core` and the tests build frames only through here.**
- `decode_frame(bytes) -> Result<Decoded, ControlError>` (`control.cpp:65`).

### `proto/include/loom/proto/reassembly.hpp` + `proto/src/reassembly.cpp` — client receive model (§6, §3.6)
- `struct Fragment` (header subset), `struct Event { Deliver | IdrRequest }`,
  `struct Counters { dropped_incomplete, discarded_gap, stale_fragments }` — all `==`-comparable
  for vector assertions.
- `class Reassembler::push(t_ms, frag)` (`reassembly.cpp:7`) with private `deliver` / `maybe_idr` —
  Rule 1 staleness, Rule 2 ≤2 in-flight frames, Rule 3 decode gating + ≥250 ms IDR throttle.
  Invariants are spelled out in the header's top comment.
- *This is the video-loss recovery brain; wired to the decoder in M1.2.*

### `proto/include/loom/proto/clocksync.hpp` + `proto/src/clocksync.cpp` — clock min-filter (§7)
- `struct Estimate { rtt, offset }`; `class ClockFilter::push(t0,t1,t2,t3)` (`clocksync.cpp:13`) —
  rtt/offset with floor division (`floor_div2`, `:7`), keeps the min-rtt sample over 16 (ties →
  most recent). *Wired live in M1.3.*

### `proto/include/loom/proto/keymap.hpp` + `proto/src/keymap.cpp` — keymap tables (§3.5)
- `class Keymap::from_csv(text)` (`keymap.cpp:25`) — parses `from,to` rows (`#`/blank skipped,
  malformed skipped); `get(code) -> optional`; **parses only, no file I/O**. Helpers `trim`,
  `parse_int`.

### `proto/include/loom/proto/errors.hpp` — protocol error codes (§10)  ‹added M1.1, header-only›
- `kNone`…`kInternal` constants + `name(code)` — used as ERROR (0x40) body codes and QUIC
  application close codes. The single DRY home for these; `core` and `sdl` import them.

### `proto/include/loom/proto/hex.hpp` — hex helpers (header-only)
- `to_hex` / `from_hex` — vectors carry raw bytes as hex; used by the adapter and tests only.

### `proto/src/vector_adapter.cpp` — conformance adapter (VECTORS.md §2/§3)
- The **only** I/O in the wire layer. Reads a vector's JSON on stdin, runs each `op` against the
  library, prints `{"results":[…]}`. `cbor_to_json` (`:58`) + per-category runners
  `datagram_encode/decode`, `control_encode/decode`, `reassembly_trace`, `clocksync_series`,
  `keymap_lookup`; dispatch at `:217`. Driven by `spec/vector-check` (check.sh step 3).

---

# `core/` — client session logic (library `loom_core`)

Transport-agnostic, sans-io. Depends only on `loom_proto` (PUBLIC), so linking `loom_core`
pulls the wire layer transitively.

### `core/include/loom/core/session.hpp` + `core/src/session.cpp` — the mirror state machine ★ the heart
- The client half of the handshake: `Connected → HELLO → WELCOME → CONFIG → CONFIG_ACK → START
  → Streaming; ERROR → Failed; BYE → Closed` (§1.1, §3.4).
- `enum class State { Connecting, Negotiating, Streaming, Closed, Failed }`.
- `struct HelloParams` (client name, codecs, max res/refresh, feature bits) — advertised in HELLO.
- `struct SessionConfig` — media params parsed from the host's CONFIG.
- `enum class Event { Connected, ConnectionLost, UserBye }`.
- `struct Action { SendControl | Established | MediaExpected | Fatal | Closed }` — drained by the driver.
- `class Session`:
  - **inputs** `on_control_bytes(span)` (buffers + extracts length-prefixed frames itself),
    `on_event(Event)`;
  - **output** `poll() -> vector<Action>`;
  - **observers** `state()`, `host_name()`, `config()`.
- `.cpp` internals: `on_event` builds HELLO/BYE via `loom_proto`; `on_control_bytes` does the
  frame reassembly loop; `handle_frame` dispatches by `Step { Welcome, Config, Start }`; anonymous
  `find_int/find_text/find_pair_elem` read decoded bodies. Never touches a CBOR byte directly.

---

# `sdl/` — desktop debug client (`loom-sdl`)

The QUIC transport + app. OFF by default and excluded from `check.sh` (needs network + a
multi-minute msquic build); enable with the `sdl` CMake preset. SDL2 window + libavcodec decode
+ overlay arrive in M1.2/M1.3 — M1.1 is connect + handshake, headless.

### `sdl/src/transport.hpp` — the QUIC seam ★ start here
- `struct TransportEvent { Connected | ControlBytes | Closed }` (Closed carries the §10 close code).
- `class ITransport` — `start(host,port)`, `send_control(frame)`, `next_event()` (poll),
  `close(code)`. Poll-based to match `core::Session`; **no protocol logic here**. The Quest client
  implements this same interface with msquic-on-Android and reuses `core/` untouched.

### `sdl/src/msquic_transport.hpp` + `sdl/src/msquic_transport.cpp` — `ITransport` over msquic
- `class MsQuicTransport` owns the msquic registration, configuration (ALPN loom/1, dev creds),
  connection, and the single client-initiated bidirectional **control stream**.
- msquic callbacks run on its worker threads and only push `TransportEvent`s onto a
  mutex-guarded queue (`push`), so the app loop stays single-threaded.
- `.cpp`: `open_configuration` (settings: keep-alive 5 s / idle 15 s; creds:
  `NO_CERTIFICATE_VALIDATION` — TODO(M7)); `start` (ConnectionOpen/Start); `send_control` (heap
  `SendBuf` freed on SEND_COMPLETE); `on_connection` (CONNECTED → open control stream + push
  Connected; peer/transport shutdown → Closed); `on_stream` (RECEIVE → ControlBytes); C
  trampolines `conn_trampoline` / `stream_trampoline`.

### `sdl/src/main.cpp` — the app loop
- Parses `host`/`port` (defaults `127.0.0.1:47800`), constructs `MsQuicTransport` + `Session`,
  `start`s, then loops: drain `transport.next_event()` → feed `Session` → drain `Session::poll()`
  → `send_control` / log `Established` (host name) / print `MediaExpected` (STREAMING params) /
  report `Fatal`/`Closed` by `errors::name`. 10 s timeout; exits 0 on STREAMING.

### `sdl/CMakeLists.txt`
- `ExternalProject` builds msquic **v2.5.9** from source (tools/tests/perf off), exposed as the
  imported `msquic` target; `loom-sdl` links `loom_core` + `msquic`. Guarded by `LOOM_BUILD_SDL`
  (top-level `CMakeLists.txt`), enabled by the `sdl` preset.

---

# `tests/` — doctest unit tests (single `proto_tests` binary)

Registered in `tests/CMakeLists.txt`, linked against `loom_core` (which brings `loom_proto`).
`smoke_test.cpp` carries `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`.

| File | Cases | Exercises |
|---|---|---|
| `smoke_test.cpp` | 1 | `Result<T,E>` holds value-or-error even when `T==E` |
| `cbor_test.cpp` | 7 | canonical encode / tolerant decode, incl. shortest-form floats |
| `datagram_test.cpp` | 5 | header encode + every `DropReason` decode path (§4) |
| `control_test.cpp` | 7 | frame encode/decode vs vectors, unknown-key/type tolerance (§3) |
| `reassembly_test.cpp` | 4 | loss/reorder/stale/window traces → deliver + IDR events (§6) |
| `clocksync_test.cpp` | 5 | min-rtt filter + floor division (§7) |
| `keymap_test.cpp` | 4 | CSV parse + lookup round-trips (§3.5) |
| `session_test.cpp` | 10 | mirror handshake, out-of-order violations, ERROR/BYE, split-frame framing |
| `hex_util.hpp` | — | test-local `to_hex`/`from_hex` helper header |

---

## Where to start reading, by task

- **Understand the wire format** → `proto/result.hpp` → `control.hpp` → `datagram.hpp` → `cbor.hpp`.
- **Understand a client session** → `core/session.hpp` (the API + Actions) then `core/src/session.cpp`.
- **Understand transport/QUIC** → `sdl/src/transport.hpp` (the seam) then `msquic_transport.cpp`.
- **Trace loss recovery** → `proto/reassembly.hpp` invariants + `reassembly.cpp` rules 1–3.
- **Run the suites** → `./check.sh` (build + `ctest` + `vector-adapter` vs `spec/vectors/`); the SDL
  target builds separately via the `sdl` preset.
- **Reuse on Quest (future, M3)** → implement `ITransport` with msquic-on-Android; `core/` + `proto/`
  are consumed unchanged.
