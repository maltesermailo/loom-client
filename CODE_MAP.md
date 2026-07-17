# loom-client — Code Navigation Map

Developer aid (informative), not a contract. Covers every `.hpp`/`.cpp` in `loom-client`
(`proto/`, `core/`, `sdl/`, `quest/`, `tests/`; excludes `spec/` and `build/`). Points at the
normative spec: **PROTOCOL.md**, **ARCHITECTURE.md**, **PAIRING.md**, **VECTORS.md** — the spec
wins on any disagreement. `proto/` here is the C++ twin of the Rust `loom-proto`
(`loom-host/CODE_MAP.md`).

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
        ┌───────────────────────────┴────────────────┐
   QUIC │ sdl/  loom-sdl  (transport + decode + show) │
  <────>│  msquic · video_pipeline · decoder · render │
        └────────────────────────────────────────────┘
        ┌────────────────────────────────────────────┐  quest/ does not touch
        │ quest/  loom-quest  (OpenXR · GLES3 · NDK) │  proto/ or core/ yet —
        │  xr_app · egl_context · gl_scene           │  it wires them in M3.3
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
| [`session.hpp`](core/include/loom/core/session.hpp) · [`.cpp`](core/src/session.cpp) | **The mirror SM + clock sync + STATS** | `Session::{on_tick,clock,encode_stats}`, `Action`, `State`, `StatsInput` | 1.1, 3.4, 7 |

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

The QUIC transport + media display. **OFF by default**, excluded from `check.sh` (needs network,
a multi-minute msquic build, SDL2 + libavcodec); enable with the `sdl` CMake preset. As of M1.2 it
opens a window and shows the streamed video; the on-screen overlay arrives in M1.3.

| File | What it is | Key symbols | § |
|---|---|---|---|
| [`transport.hpp`](sdl/src/transport.hpp) | The QUIC seam (keeps `core` transport-agnostic) | `ITransport`, `TransportEvent` (Connected/ControlBytes/Datagram/Closed) | 2, 4 |
| [`msquic_transport.hpp`](sdl/src/msquic_transport.hpp) · [`.cpp`](sdl/src/msquic_transport.cpp) | `ITransport` over msquic (C API) | `MsQuicTransport`, `on_connection`, `on_stream` | 1, 2, 4 |
| [`video_pipeline.hpp`](sdl/src/video_pipeline.hpp) · [`.cpp`](sdl/src/video_pipeline.cpp) | datagrams → reassembly → decode thread; counters | `VideoPipeline::{feed_datagram,take_frame,counters}` | 6, 3.6, 4.1 |
| [`decoder.hpp`](sdl/src/decoder.hpp) · [`.cpp`](sdl/src/decoder.cpp) | libavcodec HEVC decode; times each frame | `HevcDecoder::decode`, `DecodedFrame{capture_ts,decode_us}` | 5 |
| [`renderer.hpp`](sdl/src/renderer.hpp) · [`.cpp`](sdl/src/renderer.cpp) | SDL2 window + YUV texture + SDL_ttf overlay | `Renderer::{present,draw_overlay,poll_quit}` | 7 |
| [`metrics.hpp`](sdl/src/metrics.hpp) | Single-thread stats aggregator (overlay + §3.7 window) | `Metrics::{on_frame,take_window,overlay}` | 3.7 |
| [`clock.hpp`](sdl/src/clock.hpp) | One client monotonic clock (ping t0 / pong t3 / e2e) | `now_us` | 7 |
| [`main.cpp`](sdl/src/main.cpp) | App loop: events → `Session` + pipeline → render; timers, e2e, STATS | `main` | M1.3 |
| [`tests/latency_test.cpp`](sdl/tests/latency_test.cpp) | e2e latency accuracy ±2 ms through real decode | `sdl_tests` | M1.3 |
| [`CMakeLists.txt`](sdl/CMakeLists.txt) | msquic (ExternalProject) + SDL2/ffmpeg (pkg-config) | `msquic_ext`, `loom-sdl` | — |

### Inside `msquic_transport.cpp` — msquic wiring

| Function | Role |
|---|---|
| `open_configuration` | ALPN loom/1, keep-alive 5 s / idle 15 s, datagrams on; dev creds skip cert validation (TODO M7) |
| `start` / `send_control` | ConnectionOpen+Start / queue a control frame (heap `SendBuf`, freed on SEND_COMPLETE) |
| `on_connection` | CONNECTED → open control stream + `Connected`; DATAGRAM_RECEIVED → `Datagram`; shutdown → `Closed` |
| `on_stream` | RECEIVE → `ControlBytes` |

### Inside `video_pipeline.cpp` — the receive pipeline

| Step | Role |
|---|---|
| `feed_datagram` (main thread) | Stash fragment payload; push header to `reassembly`; on Deliver assemble AU (strip §4.1 capture_ts) → decode queue; on IdrRequest → callback sends IDR_REQUEST |
| `decode_loop` (decode thread) | Pop AU → `HevcDecoder::decode` → publish newest frame |
| `take_frame` (main thread) | Hand the newest decoded frame to the renderer |

msquic callbacks run on worker threads and only push onto a mutex-guarded queue, so the app loop stays single-threaded; decode runs on its own thread.

---

## `quest/` — Quest 3 client (`loom-quest`)

The NDK/OpenXR app. **Built by Gradle, not by a CMake preset** — the OpenXR loader ships only as a
Maven AAR consumed via AGP prefab, so Gradle owns the dependency graph and drives
`quest/CMakeLists.txt` through `externalNativeBuild`. `check.sh` format-checks `quest/src` but does
not build it. Build/deploy commands and the pinned toolchain: [`quest/README.md`](quest/README.md).

As of M3.2 this decodes a looped local bitstream onto the cylinder, but still has **no dependency
on `proto/` or `core/`** — the session and transport arrive in M3.3. The decode path's shape (a
decode thread feeding access units, the render thread latching the newest frame) is what M3.3
keeps; only the access-unit *source* changes, from the looped file to `core`'s reassembly output.

| File | What it is | Key symbols | § |
|---|---|---|---|
| [`AndroidManifest.xml`](quest/AndroidManifest.xml) | NativeActivity + `com.oculus.intent.category.VR` + WiFi permissions | — | — |
| [`LoomActivity.kt`](quest/kotlin/com/loom/quest/LoomActivity.kt) | **The entire JVM surface**: holds `WIFI_MODE_FULL_LOW_LATENCY` (no NDK equivalent) | `LoomActivity` | ARCH 6.1 |
| [`main.cpp`](quest/src/main.cpp) | `android_main`; Android lifecycle + the frame loop | `android_main`, `on_app_cmd` | — |
| [`xr_app.hpp`](quest/src/xr_app.hpp) · [`.cpp`](quest/src/xr_app.cpp) | OpenXR session/swapchains; the two layers; OES→cylinder blit | `XrApp::{create,render_frame,start_decoder}` | ARCH 6.2 |
| [`egl_context.hpp`](quest/src/egl_context.hpp) · [`.cpp`](quest/src/egl_context.cpp) | EGL context; no window surface (the compositor owns the display) | `EglContext::create` | — |
| [`gl_scene.hpp`](quest/src/gl_scene.hpp) · [`.cpp`](quest/src/gl_scene.cpp) | Floor grid + generated test texture (static fallback) | `FloorGrid`, `create_test_texture`, `Mat4` | — |
| [`hevc_decoder.hpp`](quest/src/hevc_decoder.hpp) · [`.cpp`](quest/src/hevc_decoder.cpp) | AMediaCodec HEVC decode, surface mode, decode thread + R5 metrics | `HevcDecoder`, `DecodeMetrics` | ARCH 6.1, §5 |
| [`surface_texture.hpp`](quest/src/surface_texture.hpp) · [`.cpp`](quest/src/surface_texture.cpp) | JNI `SurfaceTexture` → `ASurfaceTexture` → OES texture; new-frame latch | `SurfaceTexture::{create,update,window}` | ARCH 6.2 |
| [`au_splitter.hpp`](quest/src/au_splitter.hpp) · [`.cpp`](quest/src/au_splitter.cpp) | Annex-B → per-frame access units (offline path only) | `split_access_units`, `AccessUnit` | §5.5 |
| [`log.hpp`](quest/src/log.hpp) | `loom` logcat tag | `LOOM_LOGI`, `LOOM_LOGE` | — |

**Notes**
- The desktop is *not* drawn into the eye buffers (ARCHITECTURE §6.2). The projection layer holds
  only the floor grid; the cylinder layer's swapchain is where decoded video lands.
- The cylinder is repainted **only when the decoder produced a new frame** (`SurfaceTexture::update`
  timestamp-delta); otherwise the compositor re-samples the last image for free, so head motion
  stays smooth through a decode/network stall (§6.4 freshness).
- **Decoder feed must never burst to catch up** after a stall — that builds a permanent backlog and
  wrecks latency (the R5 measurement bug; see `reviews/M3.2-mediacodec-r5.md`). R5 result:
  1 frame in flight, ~9 ms decode; low-latency is structural to the §5 stream, not the codec flag.
- The cylinder is posed in **LOCAL** space, so a runtime recenter re-origins the space and the
  screen follows the user with no code of ours involved.
- `main.cpp`'s looper timeout must not block while resumed: OpenXR session-state events arrive via
  `xrPollEvent`, not the Android looper, so blocking there stalls the session before it starts.

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
| Trace the display path | datagram → [`video_pipeline.cpp`](sdl/src/video_pipeline.cpp) → [`decoder.cpp`](sdl/src/decoder.cpp) → [`renderer.cpp`](sdl/src/renderer.cpp) |
| Understand clock sync / stats | [`session.hpp`](core/include/loom/core/session.hpp) (`on_tick`/`clock`) → [`metrics.hpp`](sdl/src/metrics.hpp) → overlay in [`main.cpp`](sdl/src/main.cpp) |
| Trace loss recovery | [`reassembly.hpp`](proto/include/loom/proto/reassembly.hpp) invariants + rules 1–3 |
| Reuse on Quest (M3.3) | Implement `ITransport` with msquic-on-Android; `core/` + `proto/` unchanged |
| Build/run the Quest app | [`quest/README.md`](quest/README.md) — Gradle + wireless adb, JDK 21 |
| Run the suites | `./check.sh` (build + `ctest` + `vector-adapter`); SDL builds via the `sdl` preset |
