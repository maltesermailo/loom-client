# loom-client — Code Navigation Map

Developer aid (informative), not a contract. Covers every `.hpp`/`.cpp` in `loom-client`
(`proto/`, `core/`, `sdl/`, `tests/`; excludes `spec/` and `build/`). Points at the normative
spec: **PROTOCOL.md**, **ARCHITECTURE.md**, **PAIRING.md**, **VECTORS.md** — the spec wins on any
disagreement. `proto/` here is the C++ twin of the Rust `loom-proto` (`loom-host/CODE_MAP.md`).

**Reading the tables:** file names link to the file; `symbols` are the key items to jump to; the
**§** column lists the spec sections a file implements. Public API is in `*.hpp`; the paired
`*.cpp` holds the implementation.

## Conventions (why the code looks this way)

| Rule | Where |
|---|---|
| C++20, **no exceptions across the protocol boundary** — fallible calls return `Result<T,E>` | [`result.hpp`](proto/include/loom/proto/result.hpp) |
| Wire layer is **dependency-free and I/O-free** (json/doctest only in the adapter + tests) | `proto/` |
| Transports move bytes, `core` decides, `proto` encodes | see diagram |
| Namespaces: `loom::proto`, `loom::core`, `loom::sdl` | — |

## Layout at a glance

```
                    ┌──────────────────────────────────────────────┐
   wire bytes <────>│ proto/  loom_proto  (pure: no I/O, no QUIC)   │
                    │  cbor · datagram · control · reassembly ·     │
                    │  clocksync · keymap · errors · result · hex   │
                    └───────────────▲──────────────────────────────┘
                                    │ encode / decode only
                    ┌───────────────┴──────────────────────────────┐
                    │ core/  loom_core  (sans-io session SM)        │
                    │  Session: on_control_bytes/on_event → poll()  │
                    └───────────────▲──────────────────────────────┘
                                    │ ITransport seam (bytes + events)
        ┌───────────────────────────┴───────────────┐   Quest (M3) reuses core/ +
   QUIC │ sdl/   loom-sdl  (msquic transport + app)  │   proto/ behind its own
  <────>│  transport · msquic_transport · main       │   ITransport — not here yet
        └────────────────────────────────────────────┘
```

**The design rule:** `core::Session` knows nothing about QUIC, so the same object runs under
SDL/msquic here and under msquic-on-Android on the Quest, untouched.

---

## `proto/` — the wire protocol (library `loom_proto`)

Independent C++ implementation of the same contract as the Rust `loom-proto`.

| File (`.hpp` / `.cpp`) | What it is | Key symbols | § |
|---|---|---|---|
| [`result.hpp`](proto/include/loom/proto/result.hpp) | `expected`-style result primitive | `Ok`, `Err`, `Result` | — |
| [`cbor.hpp`](proto/include/loom/proto/cbor.hpp) · [`.cpp`](proto/src/cbor.cpp) | Canonical CBOR value model | `Value`, `encode_canonical`, `decode` | 3.2 |
| [`datagram.hpp`](proto/include/loom/proto/datagram.hpp) · [`.cpp`](proto/src/datagram.cpp) | Video/audio 12-byte header | `DatagramHeader`, `decode`→`DropReason`, `encode_datagram` | 4 |
| [`control.hpp`](proto/include/loom/proto/control.hpp) · [`.cpp`](proto/src/control.cpp) | Control framing + message registry | `encode_frame`, `decode_frame`, `Decoded`, `known_keys` | 3 |
| [`reassembly.hpp`](proto/include/loom/proto/reassembly.hpp) · [`.cpp`](proto/src/reassembly.cpp) | Client receive model (loss recovery) | `Reassembler::push`, `Event`, `Counters` | 6, 3.6 |
| [`clocksync.hpp`](proto/include/loom/proto/clocksync.hpp) · [`.cpp`](proto/src/clocksync.cpp) | Clock offset/RTT min-filter | `ClockFilter::push`, `Estimate` | 7 |
| [`keymap.hpp`](proto/include/loom/proto/keymap.hpp) · [`.cpp`](proto/src/keymap.cpp) | Keymap CSV parse (no I/O) | `Keymap::from_csv`, `get` | 3.5 |
| [`errors.hpp`](proto/include/loom/proto/errors.hpp) | Protocol error *codes* ‹M1.1, header-only› | `kNone`…`kInternal`, `name` | 10 |
| [`hex.hpp`](proto/include/loom/proto/hex.hpp) | Hex ↔ bytes helpers (header-only) | `to_hex`, `from_hex` | — |
| [`vector_adapter.cpp`](proto/src/vector_adapter.cpp) | Conformance adapter — the layer's only I/O | `main`, `cbor_to_json`, per-category runners | VECTORS 2/3 |

**Notes**
- `datagram::decode` returns `DropReason`, never an error — datagram failures are *silent drops* (§6.6).
- `reassembly` (loss recovery) + `clocksync` are proven by the vectors now; wired to the decoder/transport in M1.2/M1.3.

---

## `core/` — client session logic (library `loom_core`)

Transport-agnostic, sans-io. Depends only on `loom_proto` (PUBLIC).

| File | What it is | Key symbols | § |
|---|---|---|---|
| [`session.hpp`](core/include/loom/core/session.hpp) · [`.cpp`](core/src/session.cpp) | **The mirror state machine** | `Session`, `HelloParams`, `SessionConfig`, `Event`, `Action`, `State` | 1.1, 3.4 |

### Inside `session.hpp` — the API surface

| Direction | Member | Purpose |
|---|---|---|
| in | `on_control_bytes(span)` | Feed raw control bytes; buffers + extracts length-prefixed frames |
| in | `on_event(Event)` | `Connected` (→ send HELLO), `ConnectionLost`, `UserBye` |
| out | `poll() → vector<Action>` | Drain `SendControl` / `Established` / `MediaExpected` / `Fatal` / `Closed` |
| obs | `state()`, `host_name()`, `config()` | Current phase + what the host advertised |

### Inside `session.cpp` — the handshake steps

| Step (`Step::`) | On the expected message | Otherwise |
|---|---|---|
| `Welcome` | store host name → `Established` | `PROTOCOL_VIOLATION` |
| `Config` | parse `SessionConfig` → send `CONFIG_ACK` | `PROTOCOL_VIOLATION` |
| `Start` | → `Streaming` + `MediaExpected` | `PROTOCOL_VIOLATION` |
| any | `ERROR`→`Fatal(code)`, `BYE`→`Closed`, unknown type→ignored | — |

---

## `sdl/` — desktop debug client (`loom-sdl`)

The QUIC transport + app. **OFF by default**, excluded from `check.sh` (needs network + a
multi-minute msquic build); enable with the `sdl` CMake preset. SDL2 window + decode + overlay
arrive in M1.2/M1.3 — M1.1 is connect + handshake, headless.

| File | What it is | Key symbols | § |
|---|---|---|---|
| [`transport.hpp`](sdl/src/transport.hpp) | The QUIC seam (keeps `core` transport-agnostic) | `ITransport`, `TransportEvent` | 2 |
| [`msquic_transport.hpp`](sdl/src/msquic_transport.hpp) · [`.cpp`](sdl/src/msquic_transport.cpp) | `ITransport` over msquic (C API) | `MsQuicTransport`, `on_connection`, `on_stream` | 1, 2 |
| [`main.cpp`](sdl/src/main.cpp) | App loop: events → `Session` → actions | `main` | M1.1 |
| [`CMakeLists.txt`](sdl/CMakeLists.txt) | Builds msquic v2.5.9 (ExternalProject) + `loom-sdl` | `msquic_ext`, `loom-sdl` | — |

### Inside `msquic_transport.cpp` — msquic wiring

| Function | Role |
|---|---|
| `open_configuration` | ALPN loom/1, keep-alive 5 s / idle 15 s; dev creds skip cert validation (TODO M7) |
| `start` | ConnectionOpen + ConnectionStart |
| `send_control` | Queue a frame (heap `SendBuf`, freed on SEND_COMPLETE) |
| `on_connection` | CONNECTED → open control stream + push `Connected`; shutdown → `Closed` (close code) |
| `on_stream` | RECEIVE → push `ControlBytes` |

msquic callbacks run on worker threads and only push onto a mutex-guarded queue, so the app loop stays single-threaded.

---

## `tests/` — doctest unit tests (single `proto_tests` binary)

Linked against `loom_core` (which brings `loom_proto`). `smoke_test.cpp` carries the doctest `main`.

| File | Cases | Exercises |
|---|---|---|
| [`smoke_test.cpp`](tests/smoke_test.cpp) | 1 | `Result<T,E>` holds value-or-error even when `T==E` |
| [`cbor_test.cpp`](tests/cbor_test.cpp) | 7 | Canonical encode / tolerant decode, incl. shortest-form floats |
| [`datagram_test.cpp`](tests/datagram_test.cpp) | 5 | Header encode + every `DropReason` path |
| [`control_test.cpp`](tests/control_test.cpp) | 7 | Frame encode/decode vs vectors, unknown-key/type tolerance |
| [`reassembly_test.cpp`](tests/reassembly_test.cpp) | 4 | Loss/reorder/stale/window traces → deliver + IDR events |
| [`clocksync_test.cpp`](tests/clocksync_test.cpp) | 5 | Min-rtt filter + floor division |
| [`keymap_test.cpp`](tests/keymap_test.cpp) | 4 | CSV parse + lookup round-trips |
| [`session_test.cpp`](tests/session_test.cpp) | 10 | Mirror handshake, out-of-order violations, ERROR/BYE, split-frame framing |
| [`hex_util.hpp`](tests/hex_util.hpp) | — | Test-local `to_hex`/`from_hex` helper |

---

## Where to start, by task

| I want to… | Read, in order |
|---|---|
| Understand the wire format | [`result.hpp`](proto/include/loom/proto/result.hpp) → [`control.hpp`](proto/include/loom/proto/control.hpp) → [`datagram.hpp`](proto/include/loom/proto/datagram.hpp) → [`cbor.hpp`](proto/include/loom/proto/cbor.hpp) |
| Understand a client session | [`session.hpp`](core/include/loom/core/session.hpp) → [`session.cpp`](core/src/session.cpp) |
| Understand transport / QUIC | [`transport.hpp`](sdl/src/transport.hpp) → [`msquic_transport.cpp`](sdl/src/msquic_transport.cpp) |
| Trace loss recovery | [`reassembly.hpp`](proto/include/loom/proto/reassembly.hpp) invariants + rules 1–3 |
| Reuse on Quest (M3) | Implement `ITransport` with msquic-on-Android; `core/` + `proto/` unchanged |
| Run the suites | `./check.sh` (build + `ctest` + `vector-adapter`); SDL builds via the `sdl` preset |
