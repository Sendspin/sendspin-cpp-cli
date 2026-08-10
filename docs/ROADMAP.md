# Roadmap

`sendspin-cpp-cli` is being built as an epic: this scaffold brings up the build and
boots a real [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) client, and each
item below is a follow-up task that fills in one part of the player.

The shape to aim for is [squeezelite](https://github.com/ralph-irving/squeezelite): a
headless endpoint that advertises itself, is discovered, and is driven remotely, with a
small flag set for identity, output, discovery, logging, and daemonization.

## What the scaffold already does

- Builds `sendspin-cli`, pulling `sendspin-cpp` via CMake `FetchContent` (pinned tag).
- Boots a `SendspinClient` with the `player` and `metadata` roles, starts its WebSocket
  server, pumps `client.loop()`, and shuts down cleanly on `SIGINT`/`SIGTERM`.
- Speaks both of the protocol's connection modes, and keeps them exclusive as the spec
  requires: it advertises `_sendspin._tcp` over mDNS by default, and any `-s` instead
  makes it dial out — to an address, or to a server discovered on
  `_sendspin-server._tcp` — retrying with a backoff until it answers (item 5).
- Defines the `AudioSink` seam (`src/audio_sink.h`) and plays real audio through it:
  auto-detected ALSA (item 2) and PortAudio (item 3) backends, with the device-less
  null/stdout sink as the fallback, so the binary still runs where there is no sound card.
- Parses the squeezelite-style flag surface: `-o -l -n -s -z -P -d -f --port --buffer-ms
  --no-mdns --mdns-name --help --version`, validating every value at parse time and
  refusing to start on a bad one (item 1).
- Advertises the formats the selected output device will actually take, derived by probing
  it and crossed with what each codec can carry (item 4).

## Child tasks

### 1. CLI argument parser — *shipped*

The flag surface had the flags but not the depth behind them: nothing was validated at
parse time, the parser could not be called twice in one process, and `-l` named devices
without saying what they would play.

**Shipped** in `src/cli.{h,cpp}`, `src/audio_sink.{h,cpp}`, `src/alsa_sink.cpp` and
`tests/`:

- **Parse-time validation that hard-fails.** Every value is checked before the daemon
  starts, and a bad one exits 1 with a single `error:` line naming it. `-s` is the reason:
  it used to warn about a malformed port and dial 8927 anyway, and a player quietly
  talking to the wrong endpoint is harder to diagnose than one that refuses to start.
  `parse_server_url()` rejects an empty host, a non-numeric or out-of-range port, an
  unbracketed IPv6 literal, an unterminated `[`, and any scheme other than `ws://`/`wss://`;
  `-o`, `-n`, `-P` and `-f` reject empty values rather than silently defaulting.
- **A resolved server URL on `Options`**, so nothing downstream re-parses a value that has
  already been accepted.
- **`-o <backend>:<device>` specs**, split on the **first** colon because ALSA device
  names carry their own — `alsa:hw:2,0` is the alsa backend playing `hw:2,0`. Bare PCM
  names (`hw:2,0`, `default`) keep working unprefixed, and `null` / `stdout` / `-` stay
  reserved. Backend names this build lacks are reserved on purpose, so `-o portaudio:2`
  says which backends exist instead of being handed to ALSA as a PCM name.
- **Per-card capability detail in `-l`**: one `snd_pcm_open()` per PCM, then in-memory
  `snd_pcm_hw_params_test_*()` probes for rates, channel counts, and the four formats the
  sink can emit (`S8`, `S16_LE`, `S24_3LE`, `S32_LE`). A busy or unopenable device gets a
  note and enumeration continues. Measured at ~40 ms for 15 PCMs on a PipeWire host, so
  the detail is always on.
- **A testable parser**: getopt's process-global scan state is reset on entry, and every
  diagnostic goes to a caller-supplied `std::FILE*` instead of bare `stderr`.
- **The test harness itself** — GoogleTest via `FetchContent` (pinned tag), wired to
  CTest, with `src/` split into a `sendspin-cli-core` library the tests and the binary
  both link. 35 tests covering the flag surface, the `-s` matrix, `-o` spec resolution,
  and a parse-twice-in-one-process case.
- **A config-file precedence hook**: `Options::was_given()` distinguishes "the user typed
  this" from "this is the default".

**Not in this slice.** Each one belongs to an item that already owns that area:

- **The config file itself** — format, path search, reading → item 8. This task built only
  the hook a precedence layer needs, deliberately stopping short of choosing a format.
- **Per-category log levels** — `-d slimproto=info` still parses the category, ignores it,
  and says so → item 6, which owns logging.
- **A CI workflow** → item 12. This task leaves a harness for it to call; wiring it into a
  build matrix is that item's job.

### 2. `AudioSink` + ALSA backend — *shipped (audible slice)*

The default Linux and Docker backend. In a container this needs only
`--device /dev/snd` — no sound-server socket to forward.

**Shipped** in `src/alsa_sink.{h,cpp}`, built when `find_package(ALSA)` succeeds
(`-DSENDSPIN_CLI_WITH_ALSA=OFF` forces it out):

- `snd_pcm` playback over `SND_PCM_ACCESS_RW_INTERLEAVED`, format negotiated from the
  stream's bit depth (`S16_LE` / `S24_3LE` / `S32_LE`, plus `S8`) at an exact rate, with
  a close/reopen on a mid-stream format change.
- `snd_pcm_delay()`-based playout timing fed back through `on_frames_played`, as the
  microsecond timestamp at which the frames just written finish playing.
- Underrun (`-EPIPE`) recovery via `snd_pcm_prepare()`, and suspend (`-ESTRPIPE`) via
  `snd_pcm_resume()` with a `prepare()` fallback.
- Device enumeration for `-l` through `snd_device_name_hint()`, and `-o <any PCM name>`
  — squeezelite's model, with `null` / `stdout` / `-` still reserved. `-o` defaults to
  `default` wherever the backend is compiled in.
- Software volume: Q32 fixed-point sample scaling on a quadratic taper, matching
  upstream's `PortAudioSink::apply_volume_()`. Item 3 moved it to `src/pcm_volume.{h,cpp}`
  so the PortAudio backend shares one copy rather than carrying a second.
- libasound's own stderr diagnostics routed through the CLI logger at `debug`.

**Not in this slice.** Three things were deliberately left out; each is now tracked on
the item that owns it, rather than as a loose follow-up here:

- The **ALSA hardware mixer** (`snd_mixer_*`, squeezelite's `-V`) → item 15. Software
  scaling was chosen first because the usual `default` device here is PipeWire's ALSA
  plugin, where a hardware mixer element either does not exist or moves something other
  than this stream. A hardware path is worth having for `hw:` output, where it is the
  real volume control.
- **Buffer/period tuning** → item 4, where it shipped as `--buffer-ms`. The ring was a
  fixed 100 ms with 20 ms periods; it is now that by default and settable from one flag.
- **Richer per-card enumeration** → item 1, where it shipped. `-l` now prints each PCM's
  rates, formats, and channel counts alongside its hint.

### 3. PortAudio backend — *shipped (audible slice)*

The cross-platform backend (macOS and Linux), and the only way this player makes noise on
macOS, where there is no ALSA at all. Upstream's `examples/common/portaudio_sink.cpp` was
the reference, but it is not an `AudioSink`, has no device selection, and logs with bare
`fprintf(stderr)` — so it was ported into `src/` rather than compiled out of the
FetchContent tree, whose `examples/` path is not a stable interface.

**Shipped** in `src/portaudio_sink.{h,cpp}`, built when `pkg_check_modules(portaudio-2.0)`
succeeds (`-DSENDSPIN_CLI_WITH_PORTAUDIO=OFF` forces it out):

- A lock-free SPSC ring buffer bridging `write()`'s push model to PortAudio's pull
  callback, lifted from upstream so both implementations buffer alike — but sized by
  **time** (100 ms, matching ALSA's ring) rather than upstream's fixed 16 KB, which is
  85 ms at 48 kHz/16-bit and only 10 ms at 192 kHz/32-bit. Floored at 3x the `outputLatency`
  PortAudio reports for the open stream — the callback asks for a whole device buffer at a
  time, so a smaller ring would starve on every wakeup — and at 1024 frames for a device
  that reports no latency at all.
- Callback DAC-time sync feedback through `on_frames_played`: `outputBufferDacTime`'s
  distance from `currentTime` is an offset into the future, so it carries into our clock
  unchanged, plus the buffer's own duration for when the last frame leaves the DAC.
- `-o portaudio[:<device>]`, the first backend whose device is **optional**: bare means
  this host's default output, `portaudio:2` an index as `-l` prints it, and
  `portaudio:<name>` a full case-insensitive name match. A name matching more than one
  device is refused naming the candidates, since two host APIs can offer the same card
  under one name. The device is re-resolved at every stream, so a bare `-o portaudio`
  follows the host's default as the user changes it.
- `-l` grows a PortAudio section: index, name, host API, output channels, default rate,
  with the system default marked and input-only devices left out. `-o` defaults to
  `portaudio` wherever PortAudio is the only backend, so a bare run plays on macOS.
- 8/16/24/32-bit (`paInt8`/`paInt16`/`paInt24`/`paInt32`). 8-bit is deliberately beyond
  upstream's reference, which refuses it: `AlsaAudioSink` takes `S8`, and a stream one
  backend plays and the other does not would be a difference with no cause. Item 4 made
  these reachable: the advertised list is derived from the device rather than pinned to
  2 ch / 16-bit, so a server can now ask for the other depths.
- Software volume shared with the ALSA backend, extracted to `src/pcm_volume.{h,cpp}`
  rather than copied — ~50 lines of Q32 fixed-point including 24-bit sign extension, now
  with a no-device test suite of its own.

Four divergences from upstream worth knowing, the first three forced by the `AudioSink`
contract:

- **`configure()` does not call `stop()`.** Upstream's does, then clears its own abort
  latch. Ours cannot: `stop()` is contractually "released, during shutdown" and its latch
  must never be cleared, or a shutdown racing a final `on_stream_start()` gets un-latched.
  Split into private `open_stream_()`/`close_stream_()` with the public `stop()` on top,
  as `alsa_sink.cpp` does, so a mid-stream format change cannot poison writes.
- **`configure()` refuses once `stop()` has latched.** The latch is still write-once and
  nothing clears it; `configure()` only *reads* it. Without that, a stream start racing
  shutdown reopens a device `write()` can never feed and the destructor then has to tear
  down.
- **Everything the audio callback reads is mutated only while the callback provably cannot
  run** — before `Pa_StartStream()`, or after `Pa_AbortStream()`/`Pa_CloseStream()` has
  returned. That is what makes the unsynchronised reads of `bytes_per_frame_`, `bits_`, the
  stream's real rate and the ring's own storage legal; upstream writes those from
  `configure()` while its callback may be running, which is a data race. The invariant is
  written down in the header because it is invisible from the code.
- **`write()` carries a stream generation.** It blocks on a condition variable rather than
  holding the mutex throughout, which is how `stop()` stays prompt — but waiting releases the
  mutex, so a `configure()` or `clear()` can complete while a write is parked. Without a
  generation check the loop would resume and feed the rest of that buffer to the *new* stream,
  using the *old* frame size, and report it as consumed — which is exactly what the sync task
  builds its playtime estimate from. `AlsaAudioSink` needs no equivalent: it holds its mutex
  across `snd_pcm_wait()`, so the window does not exist there.

**Not in this slice:**

- **Buffer and latency tuning** → item 4, where the ring became `--buffer-ms`.
  PortAudio's `suggestedLatency` stays pinned to the device's `defaultHighOutputLatency`.
- **Hardware volume** → item 15. Both backends scale in software today.
- **A CI matrix and a sink contract suite** → item 12. This task added parser-level tests
  for the new `-o` forms and a `pcm_volume` suite, neither of which opens a device; a
  suite that exercises `AudioSink` implementations themselves is still that item's job.

**What has and has not been exercised.** The sink was driven directly on macOS — sine tone
through Core Audio at 48 kHz/16-bit, 44.1 kHz/32-bit and 44.1 kHz/24-bit, with exact
`on_frames_played` accounting, a plausible DAC offset against the reported device latency,
volume and mute, the same-format restart, a mid-stream format change, recovery from a refused
format, and the shutdown latch. It has **not** yet been driven by a real Sendspin server, so
the sync loop converging over a live stream, controller volume, and pause/resume/next-track
are unproven in the field. The Linux dual-backend path (`null, stdout, alsa, portaudio`, both
`-l` sections) is likewise unexercised — this was built and tested on macOS. Item 12's build
matrix is where that gets closed.

One known rough edge, and only on Linux: `AlsaAudioSink` routes libasound's own stderr
diagnostics through the CLI logger, but it installs that handler from its *own* `probe()`
and `list_devices()`. So `-o portaudio:...` on a dual-backend host lets PortAudio's ALSA
host API spray raw `ALSA lib pcm.c:...` lines onto stderr during device enumeration.
Deliberately not fixed here: coupling the two sinks together just to suppress a diagnostic
is a poor trade, and Linux hosts should be using `-o` ALSA directly anyway. (`-l` is
already clean on such a host, since the ALSA section runs — and installs the handler —
before the PortAudio one.)

### 4. Output buffer tuning and a device-derived format list — *shipped*

Both backends sized their buffer by a fixed constant, and `supported_formats()` advertised
2 ch / 16-bit whatever the device could do — so the 8/24/32-bit paths items 2 and 3 built
were unreachable by construction.

**Shipped** in `src/cli.{h,cpp}`, `src/audio_sink.{h,cpp}`, both sinks,
`src/supported_formats.{h,cpp}` (new), `src/player_listener.{h,cpp}` and `src/main.cpp`:

- **`--buffer-ms <ms>`**, 10–2000, one figure for both backends. ALSA divides it into five
  periods (so the default is the 100 ms ring / 20 ms period it has always been); PortAudio
  makes it the ring, where the 3× `outputLatency` and 1024-frame floors still win and say
  at `debug` which of the two did. Validated at parse time and hard-failing, per item 1.
  Deliberately **not** squeezelite's `-a`: that flag's `<b>:<p>:<f>:<m>` grammar is
  ALSA-only, and two of its four subfields are already fixed here — the format is
  negotiated from the stream and the access mode is pinned to interleaved. One flag with
  two grammars per backend is the failure `src/audio_sink.h` already refuses for `-o`, so
  `-a` is left unclaimed and the flag is long-only, as `--port` is.
- **A lower ALSA start threshold**: one period rather than a full ring. `snd_pcm_delay()`
  only counts frames queued ahead of a *running* stream, so starting on a full ring meant
  the first ring's worth of timestamps described a stream that had not begun — the "known
  rough edge" item 2 recorded. It is a timing-correctness fix rather than a knob, and it
  rides here because it shares the `sw_params` path with the buffer work.
- **`AudioSink::capabilities()`**, a `SinkCapabilities` of rates, depths and channel counts
  with a permissive default — everything this player can emit — so `NullAudioSink` needs no
  override. `AlsaAudioSink` answers it from the *same* prober that backs `-l`'s per-PCM
  detail, split out of `print_device_capabilities()` so the two cannot drift.
  `PortAudioSink` answers via `Pa_IsFormatSupported()` and grows the same three lines under
  each device in `-l`. A busy, absent or unopenable device yields the permissive set, never
  an empty one — an empty advertisement leaves the player unable to play at all.
- **A derived advertisement**, crossed per codec rather than as one cross product:
  OPUS only at 48 kHz / 16-bit (the decoder writes `int16_t`), FLAC and PCM at every depth
  and rate the device takes. The crossing is a pure function in `src/supported_formats.cpp`
  with its own no-device test suite, and the result is logged at startup — a digest at
  `info`, every entry at `debug`.
- **Loud refusals**, which is why the capability work and this ship together: widening the
  advertisement multiplies the routes into a player that looks healthy and plays nothing.
  A refused `configure()` now names the format *and* the device and says the audio is being
  discarded, and a stream that passes entirely discarded says so again at its end rather
  than leaving one line and silence.

**Two staleness limits the derived list carries**, both commented at the point of probing
rather than only here:

- **PortAudio re-resolves its device at every stream**, so a bare `-o portaudio` follows
  the host's default output as the user changes it — while the advertisement describes
  whichever device was default at *startup*. The loud-refusal path above is what makes a
  later mismatch diagnosable.
- **ALSA's `default` is usually PipeWire's plugin**, so the probe reports what the *plugin*
  accepts, not what the card does. That is still the honest answer to "what can I push
  through this `-o` value", which is the question an advertisement answers — but it is not
  a description of the hardware.

**Not in this slice.** Each was split out of the original item 4 into an item of its own,
carrying the constraints found while scoping this one:

- **`PlayerRoleConfig` wiring** → item 13.
- **PortAudio in-place device recovery** → item 14.
- **The ALSA hardware mixer (`-V`)** → item 15.
- **Mid-stream format changes**, which the original item 4 still asked for, were already
  done: `AlsaAudioSink::configure()` reuses the device on a same-format restart and
  closes/reopens otherwise, and `PortAudioSink` does the same behind its stream-generation
  guard. They shipped in items 2 and 3.

### 5. mDNS advertise/discovery + outbound mode — *shipped*

The player only listened, and said so: `src/main.cpp` logged "This build does not
advertise over mDNS yet", and `-s` fired exactly one `connect_to()` that was never
retried. Both halves of the protocol's connection model land here.

**Shipped** in `src/mdns.h`, `src/mdns_common.cpp`, `src/mdns_dnssd.cpp`,
`src/mdns_null.cpp`, `src/outbound.{h,cpp}`, `src/last_server.{h,cpp}`,
`src/cli.{h,cpp}`, `src/main.cpp` and `tests/`:

- **`_sendspin._tcp` advertisement** on the `--port` port, TXT `path=/sendspin` and
  TXT `name`, via `dns_sd.h` — Bonjour on macOS (built in), `libavahi-compat-libdnssd`
  on Linux. Optional and auto-detected exactly like the audio backends, reported in
  its own configure line, with `src/mdns_null.cpp` keeping a build without it working.
  The register callback is kept rather than upstream's `nullptr`, so the name **logged
  is the name that actually registered** — the daemon renames on a collision. The
  record is withdrawn explicitly during shutdown, before the client disconnects.
- **The two modes, made exclusive**, because the spec says so: "Do not advertise
  `_sendspin._tcp` if the client plans to initiate the connection." Any `-s`
  suppresses the advertisement and logs which flag did it; `--no-mdns` covers the
  no-`-s` case; `--mdns-name` alongside `-s` warns it is unused rather than failing.
  Nothing can turn both on together.
- **`-s mdns:[<name>]`**, discovery on the existing flag via a reserved prefix split
  on the **first** colon — the idiom `-o` already uses for `<backend>:<device>`. Every
  existing `-s` form is untouched, `hifi:8927` is still a host and a port, and a bare
  `-s mdns` is still a host. A separate `--discover` was the alternative; the prefix
  keeps `-s` as the one place a server is named. Discovery is not a bare `-s` because
  the optstring is `s:`, so a bare one would swallow the next word. On a build without
  dns_sd it is refused at **parse** time, as `-o portaudio` already is without PortAudio.
- **Discovery of `_sendspin-server._tcp`**: browse, resolve, then A/AAAA queries, with
  the URL built from the server's own TXT `path`. An instance whose `path` is missing
  or does not start with `/` is skipped and said so at `debug`, matching the reference
  server. A non-link-local IPv4 wins over IPv6, and an IPv6-only server yields a
  bracketed URL. The browse stays open for the daemon's lifetime, and an instance that
  goes away leaves the candidate set rather than staying in it as a dial target.
- **Server selection**, which the spec leaves implementation-defined: the `-s
  mdns:<name>` filter is a hard constraint, and among what survives it the last server
  whose handshake completed wins, else the first to resolve. That preference is
  decidable *before* dialling because the `_sendspin-server._tcp` instance label **is**
  the protocol `server_id` — the reference server registers `f"{id}.{service_type}"`
  and sends the same `id` in `server/hello`, which was confirmed against a live server.
- **Outbound retry**, since the library deliberately does none:
  `ConnectionManager::connect_to()` calls `set_auto_reconnect(false)` and
  `SendspinClientListener` has no connect/disconnect callback. 1 s doubling to a 30 s
  ceiling, reset on a completed handshake, restarted from the floor on a drop, and no
  dialling at all while connected — including when the connection came *in*.
- **A no-background-threads model.** Every `DNSServiceRef` is `poll()`ed with a zero
  timeout from the main loop and `DNSServiceProcessResult()` is called only on a
  descriptor already shown readable, so every callback — and therefore every
  `connect_to()` — lands on the thread both are documented to require. Upstream's
  `examples/tui_client` instead runs a browse thread plus a detached thread per
  resolve.
- **28 new tests** across `tests/discovery_test.cpp`, `tests/last_server_test.cpp` and
  `tests/cli_test.cpp`, none of which opens a socket or the mDNS daemon: the browse
  result is a plain struct and `RetryPacer` is handed its clock.

**Three correctness points worth writing down**, each one a thing that fails silently
if got wrong:

- **Redials are paced from the last `connect_to()`, not from observing "not
  connected".** `is_connected()` only goes true on a *completed handshake*, so it reads
  false for the whole of an in-flight attempt — and `connect_to()` releases any previous
  outbound nursery entry with `ANOTHER_SERVER` before pushing the new one. Redialling
  per tick would therefore cancel the attempt in progress every time. The 30 s ceiling
  matches the library's own `NURSERY_ESTABLISH_TIMEOUT_S`, in its private
  `src/connection_manager.h` — not included from there, so a `SENDSPIN_GIT_TAG` bump
  should re-check it.
- **`DNSServiceGetAddrInfo` is not usable.** It is a Bonjour extension that
  `libavahi-compat-libdnssd` does not implement *at all* — not even as a stub — so the
  obvious resolve chain would take the Linux build with it. `DNSServiceQueryRecord` is
  in the compat layer and is asynchronous, so A/AAAA lookup stays on the one thread;
  upstream's example falls back to a threaded POSIX `getaddrinfo()` instead. Addresses
  are resolved rather than putting the `.local.` target in the URL, because that only
  resolves through `getaddrinfo` where nss-mdns is installed.
- **Resolves are issued on `kDNSServiceInterfaceIndexAny`, not on the browse reply's
  interface.** A multi-homed host gets one browse reply per interface, and a resolve
  pinned to an interface the instance is not really on simply never calls back — a
  silent stall, since there is no negative reply. Only one candidate is kept per
  instance, so the daemon is left to answer from wherever it can.

**Not in this slice:**

- **An Avahi-native backend** → item 16, split out with what was found here.
- **Activities-based inbound admission** → item 17, likewise.
- **A configurable state-file path, and a real `SendspinPersistenceProvider`** → item 8,
  which already owns persistence. This task added only the one small file its own
  tie-break needs, deliberately *not* via that provider: it stores an FNV1 hash computed
  by `ConnectionManager::fnv1_hash()`, which lives in the library's `src/` and is not
  installed, so a browsed candidate could not be matched against it without
  reimplementing a private hash and staying bit-compatible across tag bumps.
- **`-z` daemonization**, which an advertising player wants → item 6, unchanged.

**What has and has not been exercised.** Driven against **two real
`aiosendspin`/Music Assistant servers on macOS**, both modes: the announce mode was
discovered and dialled by a server with no configuration at all (handshake complete,
`connection_reason=discovery`), and `-s mdns:` browsed, resolved both servers to IPv4,
dialled, completed a handshake and received metadata. The remembered-server preference,
the `-s mdns:<name>` filter (matching and non-matching), `--no-mdns`, the 1/2/4/8 s
retry ramp against a closed port, prompt `SIGTERM` during a backoff, and the parse-time
refusal on a `-DSENDSPIN_CLI_WITH_MDNS=OFF` build were each exercised directly. Clean
under `-fsanitize=thread` in the announce mode, in discovery (browse, resolve, address
queries, dial and retry against a locally registered `_sendspin-server._tcp` record), when
dialling a real server by address, and while a discovered server disappears mid-run.

**Two things the suite does not prove, and does not claim to.** `src/mdns_dnssd.cpp` has no
unit coverage at all — the poll/retire/doom lifecycle, the settle window, the interface
refcount and the restart backoff are the riskiest code here and are exercised only by hand,
on macOS. (`Impl` is factored so a fake dns_sd could drive it; that is item 12's job.) And
the property that a per-tick redial would cancel the attempt in flight is reasoned from
`ConnectionManager::connect_to()`'s source, not exercised — what the `RetryPacer` suite
proves is the schedule. **The Linux path is unexercised** —
`libavahi-compat-libdnssd` has not been run against, only read; that is item 12's build
matrix to close, and the Avahi compat findings above are the specific thing it should
check. A mid-session server disappearance and the reconnect-after-drop path were
covered by unit tests rather than in the field.

### 6. Daemonization and logging

`-z` (fork, detach, redirect standard streams), `-P` pidfile locking rather than just
writing, and a real logging framework behind `-d`/`-f`: per-category levels like
squeezelite's `-d slimproto=info`, syslog, timestamps, rotation. `-z` currently exits
with a "not implemented" error rather than silently staying in the foreground.

### 7. Local control channel

A Unix control socket plus `sendspin-cli` subcommands — `pause`, `vol 50`, `status`,
`next` — so the player can be driven from its own host, not only by a remote controller.
This is the deliberate addition to the squeezelite model.

### 8. Config file

Persist identity, output device, and server so a daemon does not need a long flag line.
Also a `SendspinPersistenceProvider` implementation, which the library already supports
for the last-played server and the static delay.

Item 5 shipped a deliberately minimal `src/last_server.{h,cpp}` — one server id, one file
under `$XDG_STATE_HOME` — because discovery needed a tie-break and could not use the
provider. **The constraint it found belongs here:** `SendspinPersistenceProvider` stores an
FNV1 *hash* of the server id, computed by `ConnectionManager::fnv1_hash()`, which lives in
the library's `src/` and is **not installed**. So nothing outside the library can produce a
matching key without reimplementing a private hash and staying bit-compatible with it
across `SENDSPIN_GIT_TAG` bumps. Whatever this item does with the provider, the discovery
tie-break needs the id itself; folding `last_server` into a general config/state layer, and
making its path configurable, is this item's job.

The parser hook this needs is already in place (item 1): `Options::was_given()` reports
which options the user actually typed, so config values can layer *under* the command line
without the parser's own defaults clobbering them. Format, path search, and the precedence
logic itself are all still open.

### 9. Docker

Image and compose file. ALSA with `/dev/snd` passed through for real output, and the
null sink for device-less containers and CI.

### 10. Packaging

`install()` rules, a systemd unit, and distribution packaging.

### 11. Interactive TUI mode

Optional, later. Upstream's `examples/tui_client` shows the shape.

### 12. CI and tests

A build matrix (Linux and macOS) and a smoke test that the binary boots, links, and exits
cleanly on a signal.

The harness this calls already exists (item 1): GoogleTest via `FetchContent` pinned to a
tag, wired to CTest with `gtest_discover_tests()`, defaulting ON only when this is the
top-level project. `cmake -B build && ctest --test-dir build` is the whole invocation. The
parser suite lives in `tests/`, and item 3 added a `pcm_volume` suite for the Q32 scaling
both backends share; item 4 added a `supported_formats` suite for the capability crossing;
item 5 added `discovery` and `last_server` suites. **The sink contract itself is still
untested** — that is what a `NullAudioSink`/`AlsaAudioSink`/`PortAudioSink` suite would
add, alongside the matrix and the smoke test. Nothing in `tests/` opens an audio device, a
socket, or the mDNS daemon, which is what keeps the suite runnable on a bare CI runner.

Two things the matrix specifically owes item 5, since it was built and exercised on macOS
only: that `libavahi-compat-libdnssd` really does implement `DNSServiceQueryRecord` for A
and AAAA as read (it has no `DNSServiceGetAddrInfo` at all), and that the Linux build picks
it up through `find_path`/`find_library`. The `-DSENDSPIN_CLI_WITH_MDNS=OFF` configuration
should be one of the matrix legs, since it changes which translation unit is compiled.

### 13. `PlayerRoleConfig` wiring

`fixed_delay_us` and `extra_startup_silence_ms` are still at library defaults in
`src/main.cpp`, and `set_static_delay_adjustable(true)` is advertised with no
`on_static_delay_changed()` override in `src/player_listener.cpp` — so a controller can
offer the user a static delay this player then ignores. Split out of item 4.

Two constraints found while scoping that item, and the reason this is not a five-minute
change:

- **`fixed_delay_us` must stay 0.** Both sinks already report *future* finish timestamps
  that include their own buffering — `snd_pcm_delay()` on ALSA, `outputBufferDacTime` on
  PortAudio — so folding device latency in here would count it twice.
- **`extra_startup_silence_ms` cannot be chosen until item 4's start-threshold fix has been
  measured on real hardware.** Under a full-ring start threshold you would have been
  measuring the bug rather than the pipeline.

### 14. PortAudio in-place device recovery

`PortAudioSink` latches into discarding once `Pa_IsStreamActive()` goes false, so a USB DAC
unplugged mid-track stays silent until the next stream re-resolves the device. ALSA already
recovers `-EPIPE`/`-ESTRPIPE` inside `write()`; this is the same idea for the other backend,
without waiting for a track boundary. Split out of item 4.

A second, unrelated defect in the same file, found while building item 5 and left there
alone to keep that diff to its own subject: `src/portaudio_sink.cpp:495` calls `.c_str()`
on the temporary `std::string` returned by `this->name()`, so `device_name` can dangle
before it is used. clang reports it as `-Wdangling-gsl`, and it is the only warning our own
sources produce.

Note that the device is *already* re-resolved at every `configure()`, so the gap is
specifically mid-stream. The awkward part is that recovery has to reopen a stream from the
sync task's thread or defer to the main loop, and the sink's threading invariant is that
everything the audio callback reads is mutated only while the callback cannot run.

### 15. ALSA hardware mixer (`-V`)

`snd_mixer_*` where the device has a real control to drive — chiefly `hw:` output straight
to a card, where a hardware mixer is the actual volume rather than a scaling of the samples.
Split out of item 4; items 2 and 3 had both deferred it here.

Keep `src/pcm_volume.{h,cpp}` for plugin devices: the usual `default` PCM is PipeWire's ALSA
plugin, where a mixer element is either absent or moves something other than this stream.
One path or the other per device, chosen at open time — never both stacked, which would
square the taper.

### 16. Avahi-native mDNS backend

Linux only, and a *second* backend rather than a replacement — which is why item 5 shipped
the `dns_sd.h` one first and left this its own item. The `MdnsService` seam
(`src/mdns.h`, one implementation per build chosen by CMake) is what makes it a drop-in.

What it buys, all of it specific to `libavahi-compat-libdnssd`:

- **Daemon-restart recovery that is told to us.** The compat shim reports
  `kDNSServiceErr_ServiceNotRunning` on every ref once `avahi-daemon` restarts; item 5
  handles that by tearing down and re-registering on a backoff, which works but is a
  poll. Avahi's own client has a state callback that says it directly.
- **Collision signalling.** With `flags = 0` the daemon renames on a conflict and the
  register callback reports the new name — good enough, but Avahi names the collision.
- **No compat shim**, and so no `*** WARNING *** The program 'sendspin-cli' uses the Apache
  Portable Runtime...`-style stderr banner the shim prints on some distributions, outside
  the CLI logger entirely.
- **`DNSServiceGetAddrInfo`'s absence stops mattering.** Item 5 works around it with
  `DNSServiceQueryRecord`; Avahi has a first-class address resolver.

### 17. Activities-based inbound admission

**Real spec drift, found while scoping item 5, and inbound-only — so it did not block that
item, but it should not be lost.** The spec has moved inbound arbitration to an
`activities` ranking: `management` > `playback` > `pairing` > empty, provisional until the
first `server/activate`, with "higher or equal is accepted, lower is rejected", plus a
persisted last-*playback* server.

Pinned `sendspin-cpp` v0.7.0 has **no `activities` and no `server/activate` at all**, and
still implements the older `connection_reason` DISCOVERY/PLAYBACK handoff. So this item is
gated on a library that speaks the newer shape, and is likely to arrive with a
`SENDSPIN_GIT_TAG` bump rather than on its own.

Item 5's `src/last_server.{h,cpp}` is the nearest thing that exists today and is
deliberately named for what it observes — the last server whose *handshake* completed, not
its last *playback* server, which v0.7.0 gives no way to know.
