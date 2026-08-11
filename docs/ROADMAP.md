# Roadmap

`sendspin-cpp-cli` is being built as an epic: this scaffold brings up the build and
boots a real [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) client, and each
item below is a follow-up task that fills in one part of the player.

The shape to aim for is [squeezelite](https://github.com/ralph-irving/squeezelite): a
headless endpoint that advertises itself, is discovered, and is driven remotely, with a
small flag set for identity, output, discovery, logging, and daemonization.

## What the scaffold already does

- Builds `sendspin-cli`, pulling `sendspin-cpp` via CMake `FetchContent` (pinned tag).
- Boots a `SendspinClient` with the `player`, `metadata` and `controller` roles, starts its
  WebSocket server, pumps `client.loop()`, and shuts down cleanly on `SIGINT`/`SIGTERM`.
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
- Runs as a real daemon: `-z` forks and detaches, `-P` holds a locked pidfile that refuses a
  second instance and needs no stale cleanup, and every log line carries a level letter and
  a subsystem tag — timestamped and `SIGHUP`-reopenable under `-f` (item 6).
- Advertises the formats the selected output device will actually take, derived by probing
  it and crossed with what each codec can carry (item 4).
- Can be driven from its own host: a `0600` Unix control socket in a user-private directory,
  polled from the main loop, and `sendspin-cli <subcommand>` on the same binary covering the
  whole of `controller@v1` — `status`, `play`/`pause`/`stop`/`next`/`prev`, `vol`, `mute`,
  `seek`, `seek-rel`, `repeat`, `shuffle`, `switch` (item 7).

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
  and says so → item 6, which owns logging, and which closed the question rather than
  answering it: the library has no log sink hook, so per-line tags plus `grep` are the
  filtering that works.
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
are unproven in the field. Item 12's matrix now builds the Linux dual-backend configuration
on every push and asserts from the configure output that it really is `null, stdout, alsa,
portaudio` — but building it is all that proves. Neither `-l` section nor a note out of
`AlsaAudioSink` has been exercised on Linux: this was written and heard on macOS.

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
- **`-z` daemonization**, which an advertising player wants → item 6, where it shipped.

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
proves is the schedule. The Linux path is no longer only read: item 12's matrix advertises,
browses and resolves through a real `avahi-daemon` on every push, which is what confirms the
Avahi compat findings above — a `ws://` URL cannot be built without an address, so it cannot
be logged unless `DNSServiceQueryRecord` answered. A mid-session server disappearance and the
reconnect-after-drop path were covered by unit tests rather than in the field.

### 6. Daemonization and logging — *shipped*

`-z` was parsed and then refused: `src/main.cpp` printed "not implemented yet" and exited
1. `-P` was a plain `fopen`/write/remove with no lock, no stale handling and no "already
running" refusal — and it opened with `"w"`, so a second instance truncated the running
one's pid on its way to discovering it had lost. And no log line said which part of the
binary produced it.

**Shipped** in `src/daemon.{h,cpp}`, `src/log.{h,cpp}`, `src/cli.{h,cpp}`, `src/main.cpp`,
the five files that log, and `tests/daemon_test.cpp`:

- **`-z`: one `fork()`, `setsid()`, `chdir("/")`, `umask(0022)`, fd 0 and fd 1 to
  `/dev/null`.** A second fork is deliberately absent: `setsid()` already leaves the child
  a session leader with no controlling terminal, and the classic second fork only stops a
  session leader re-acquiring one by *opening* a tty, which this daemon never does.
  `daemon(3)` is not used — deprecated on macOS, so it cannot compile clean under this
  repo's `-Wall -Wextra -Wpedantic`, and it gives no way to keep fd 2 on an already-opened
  logfile.
- **No readiness handshake; the failure boundary moved instead.** Everything cheap,
  deterministic and resource-free was hoisted *above* the fork, so it still fails at the
  terminal: the parse, opening `-f`, and a probe of the `-P` lock. Everything after the
  fork reports into the log, and `README.md` says which is which rather than leaving an
  exit 0 to be discovered. A self-pipe handshake was the alternative and buys nothing the
  hoist does not.
- **The fork-ordering rule is written at the fork site as an invariant** — fork before the
  process acquires *any* resource — rather than as a list of three current call sites. All
  three are live: `make_audio_sink()` already probes the device
  (`src/audio_sink.cpp:258,267`) and `PortAudioSink` then holds a `PortAudioGuard` that
  brings up the CoreAudio HAL's mach ports and helper threads, `start_server()` starts the
  sync task's `std::thread`, and a `DNSServiceRef` is a per-process connection to
  `mDNSResponder`/`avahi-daemon`. Only the forking thread survives a fork, so an item 7
  control socket or an item 8 config file cannot quietly land above the line without
  reading why it must not. Item 7's socket did land below it, for exactly that reason.
- **Two conflicts `-z` introduces, both closed before the fork.** `-z` with `-o stdout` or
  `-o -` is refused at **parse** time: a daemon's stdout is `/dev/null`, so the PCM sink
  would silently become a second discard sink. `-z` without `-f` is *not* refused — a
  supervisor that captures nothing is a legitimate way to run it — but warns, because
  otherwise the silence reads exactly like a crash.
- **A relative `-P` or `-f` under `-z` is resolved against the invoking directory** at
  parse time. Without that, `chdir("/")` splits one flag value into two files: the parent's
  probe creates the one the operator typed, and the child writes `/<name>` — which for a
  non-root user fails *after* the terminal has already been told 0.
- **`-P` is an exclusive `flock()` held for the process lifetime**, in the one order that
  works: `open` without `O_TRUNC` → `flock(LOCK_EX|LOCK_NB)` → `ftruncate` → `write`. The
  lock is what makes stale detection free — a crashed process has its descriptor closed by
  the kernel, so a leftover file simply has no lock and is reused, with no pid parsed and
  no `kill(pid, 0)` and therefore no pid-reuse race. `EWOULDBLOCK`/`EAGAIN` is reported as
  "already running", every other errno as itself. No `FILE*` anywhere in the path, since
  `fclose()` would release the lock. The parent probes and the child acquires, which
  sidesteps flock-across-fork semantics that Linux and the BSDs document differently. The
  lock is taken **above** `make_audio_sink()`, so two instances racing collide on the
  pidfile rather than on the sound card.
- **Every log line carries a level letter and a tag**: `<L> <tag>: <message>`, with `cli`,
  `audio`, `mdns`, `discovery`, `outbound`, `player` and `metadata`. The tail is
  byte-identical to what the library's `SS_LOG*` macros emit, so `grep 'I mdns:'` and
  `grep 'I sendspin.ws_server:'` work on the same file. The 96 existing `cli_log` call
  sites were not touched: each file declares one `static constexpr const char* LOG_TAG`
  — mirroring the library's own per-file `TAG` idiom — that a macro picks up, and the
  handful of sites speaking for a second subsystem call `log_line()` with the tag
  explicitly. `__attribute__((format(printf)))` survives on `log_line()`.
- **A UTC timestamp under `-f` only.** That is the case where nothing else records *when*;
  a foreground run under systemd or Docker is already stamped by journald or the container
  runtime.
- **`SIGHUP` reopens the `-f` path**, handing rotation to `logrotate` and `newsyslog`. The
  handler sets a `volatile sig_atomic_t` and the main loop does the work, which flushes the
  old stream and then logs the result -- neither of those being async-signal-safe is what
  keeps it off the handler. The handler is installed **only** with `-f`: `SIGHUP`'s default
  disposition is terminate, which is what a foreground run whose terminal has closed should
  keep doing.
- **`PidFile` and the stream redirection left `main.cpp`** for `src/daemon.{h,cpp}` and
  `src/log.{h,cpp}` inside `sendspin-cli-core`, which is what makes the riskiest code in
  this item testable at all — the entry point is not linked by the test binary. `main.cpp`
  now holds only the call order.
- **17 new tests** — 10 in `tests/daemon_test.cpp` and 7 in `tests/cli_test.cpp`, taking the
  suite from 130 to 147 — none of which forks, opens a device or a socket, or touches the
  mDNS daemon.

**Three asks removed from this item rather than deferred**, all for one reason worth
writing down: sendspin-cpp v0.7.0 logs through `src/platform/logging.h`'s bare
`fprintf(stderr, "E %s: " fmt "\n", tag, ...)` macros, gated on a single global
`int ss_host_log_level`, with **no callback or sink hook**. Library lines cannot be
reformatted, redirected or filtered per tag by any API call this layer can make.

- **syslog / `os_log`** would carry our half of the log and leave the library's half on
  stderr, splitting one narrative across two destinations — worse than the single file
  `-f` already gives.
- **Per-category `-d` levels** have no coherent answer for library lines: raising the
  global level to serve `-d mdns=debug` floods the log with unrelated library debug, and
  not raising it shows nothing from the library. `-d` keeps its warn-and-ignore, and the
  warning now names the tags and points at `grep`, which *is* per-category filtering after
  the fact and works on the library's lines too.
- **The workaround was considered and rejected**: a pipe over fd 2 plus a reader thread
  reassembling and re-emitting lines does work, and costs a thread, a pipe, partial-line
  buffering and a shutdown-ordering problem in order to prepend twenty bytes — while
  breaking interactive foreground use. The correct owner of a log sink is sendspin-cpp's
  `SS_LOG*` macros. Per `AI_POLICY.md` no upstream issue was opened here.

**In-process rotation** is declined on different grounds: it duplicates `logrotate` and
`newsyslog` wholesale, and `SIGHUP`-reopen hands them the job in about ten lines.

**What has and has not been exercised.** `-z` is not in the suite — forking inside a gtest
process leaves two runners reporting results — so it was driven **by hand on macOS**, and
these were each checked directly: the shell returning immediately; `ps` showing the daemon
reparented to pid 1 with no controlling terminal; `lsof` showing cwd `/`, fd 0 and 1 on
`/dev/null` and fd 2 on the logfile; an unopenable `-f` and a `-P` under a missing
directory both failing at the terminal with exit 1; a second instance refused at the
terminal while the first's pidfile stayed byte-identical; `SIGKILL` followed by a clean
restart with no cleanup; the pidfile removed on `SIGTERM` from both a foreground and a
detached run; a relative `-P` landing where it was typed rather than under `/`; a post-fork
device failure appearing in the log with exit 0 at the terminal; an `mv` plus `SIGHUP`
producing a fresh file at the original path with nothing more written to the old
descriptor; and `SIGHUP` to a foreground run with no `-f` still terminating it (128+1).
The pidfile lock, stale reuse, the `ftruncate` case, the probe, and the reopen are covered
by `tests/daemon_test.cpp` — affordable without forking because an `flock` lock belongs to
the *open file description*, so two `open()` calls conflict inside one process exactly as
two instances would (`fcntl` record locks would not, and that is why they were not used).

- **`log_fatal()`, for the errors that stop the player coming up.** Tagged and stamped like
  every other line, so under `-f` the one line explaining why a daemon never started is the
  one `grep 'E '` finds — but past the level gate, because `-d none` means "do not narrate",
  not "exit 1 without saying why". Emitted under the tag of the subsystem that failed rather
  than of the startup phase, so a device that will not open joins the `audio` thread. Two
  kinds of diagnostic stay plain `error:` lines on their own stream -- the flag parser's and
  the pre-fork pidfile probe's -- because both answer a command line rather than recording a
  run, and both are printed before there is a log to write them to.
- **`open()` + `dup2()` rather than `freopen()`** for both the `-f` open and the SIGHUP
  reopen. `freopen()` closes the stream even when it fails, which had cost three
  workarounds: an injected diagnostics stream because the complaint could not go to the
  stderr that had just died, that complaint landing on **stdout** where a `2>` capture
  misses it and where `-o stdout` puts PCM, and a `/dev/null` rescue so later log lines were
  not written through a dead `FILE*`. All three are gone: a failed open leaves stderr
  untouched, and a failed reopen keeps the log on the descriptor it already had — so it can
  report itself, into the file `logrotate` has just moved.

**This branch was verified on Linux only against a patched tree, and that is worth being
precise about.** The code *in* this item builds warning-free and passes 148/149 on Debian
bookworm against real `libasound`, `portaudio` and `libavahi-compat-libdnssd`; the one
failure is `LastServer.AnUnwritableDirectoryFails`, which fails only because the container
runs as root and root ignores directory permissions — it passes as a normal user in the same
container. The `src/mdns_dnssd.cpp` break that made `main` itself uncompilable on Linux, and
that had to be patched out in a throwaway copy to get this far, is fixed under item 12 — the
whole tree now builds and tests on Linux in CI.

**Two things this item does not claim.** The library's own lines are **not timestamped**
under `-f` and cannot be, for the same missing-sink-hook reason its category cannot be
filtered — so a `-f` file has stamped lines from us and unstamped lines from the library.
And "the WebSocket port is already taken" turns out **not** to be a post-fork failure at
all: two instances on one `--port` both report listening, which is pre-existing
sendspin-cpp/IXWebSocket behaviour and is not addressed here.

### 7. Local control channel — *shipped*

The player could only be driven by a remote controller, and `CMakeLists.txt` pinned
`SENDSPIN_ENABLE_CONTROLLER OFF` with a comment saying the role was "for driving *other*
clients, which this daemon does not do". That was wrong, and it was the thing blocking this
item: `controller@v1` carries the transport verbs for the group this client is *part of*, and
the `player` role has none of them — only this endpoint's own volume, mute and static delay.
So the role is now on, and the deliberate addition to the squeezelite model lands here.

**Shipped** in `src/control.h`, `src/control_common.cpp`, `src/control_socket.cpp`,
`src/control_client.cpp`, `src/cli.{h,cpp}`, `src/main.cpp`, `src/player_listener.{h,cpp}`,
`src/log.h`, `CMakeLists.txt`, `tests/control_test.cpp`, `tests/scoped_env.h` and
`scripts/smoke_test.sh`:

- **The whole of `controller@v1`, one subcommand each**: `status`, `play`, `pause`, `stop`,
  `next`, `prev`, `vol`, `mute`, `seek`, `seek-rel`, `repeat`, `shuffle`, `switch`. `repeat`
  and `shuffle` are the two that are not pass-throughs — the mode *is* the command
  (`REPEAT_OFF`/`ONE`/`ALL`, `SHUFFLE`/`UNSHUFFLE`), so `shuffle off` is its own command
  rather than a false-valued parameter. A test walks the table against the library's enum, so
  a protocol command with no subcommand behind it fails the build's own suite.
- **`vol` is documented as *group* volume**, and `status` prints `group volume` and
  `player volume` as two named lines rather than one `volume:`. The server spreads a group
  volume across the group and clamps per player, so a squeezelite refugee's expectation that
  `vol 50` moves *this* box is wrong — and one ambiguous line would leave them unable to see
  that. `switch` is documented for what the spec's switch cycle actually does (re-home this
  client between groups), not as the "switch playback source" its library comment suggests.
- **No thread and no command queue.** `ControlSocket::poll(now_ms)` runs from the main loop
  beside `mdns.poll()`, carrying the same THREAD SAFETY note `src/mdns.h` carries. That is
  forced rather than chosen: `send_command()` reaches `SendspinClient::send_text()` and
  `ConnectionManager::current()`, which is documented main-thread-only (`current_shared()`
  exists for off-thread callers), and reading is no safer — `get_controller_state()` returns a
  reference to a vector `drain_events()` move-assigns from inside `client.loop()`, so the
  daemon copies it into a plain `ControllerSnapshot`. A round trip is therefore bounded by
  `LOOP_INTERVAL_MS`; that is stated rather than fixed by shortening the tick.
- **Three failure modes kept distinct**, with three exit statuses, because they need three
  different actions: nothing listening (3), the player up but with no server connection (4),
  and a command absent from the server's `supported_commands` (5). The ordering is the load-
  bearing part — `on_controller_state_clear()` *empties* `supported_commands` on a disconnect,
  so a gate that consulted it first would answer "pause is not supported" when the truth is
  that nothing is connected, sending the operator to read their server's capabilities instead
  of its connection. `SendspinClient::send_text()` also no-ops silently with no connection, so
  nothing may report success for a command that never left the process.
- **A `0600` socket at `$XDG_RUNTIME_DIR/sendspin-cli-<port>.sock`**, the port in the leaf so
  two players on one host each get their own. The mode is set by bracketing `bind()` with a
  `umask()` rather than a later `chmod()`: `bind()` applies the umask, `daemonize()` sets
  `umask(0022)`, and a post-`bind()` `chmod()` leaves a window where any local account can
  connect. Linux enforces socket-inode permissions on `connect()` and macOS/BSD historically
  do not, which is the second reason the parent directory must be user-private.
- **Two sources for the default directory, and no `/tmp` among them.** `$XDG_RUNTIME_DIR`
  wherever it is set, on every platform, trusted because it is the user's own declaration. Then
  the platform's own equivalent, which on macOS is `confstr(_CS_DARWIN_USER_TEMP_DIR)` — the
  per-user `/var/folders` directory launchd already provides. That second source exists because
  launchd sets no `$XDG_RUNTIME_DIR` *at all*, not even in an interactive shell, so without it
  the default path never resolved on the one platform the PortAudio backend exists to serve.
  Deliberately **not `$TMPDIR`**, which usually names the same directory: `confstr()` reads
  nothing from the environment, so unlike `$TMPDIR` it cannot be pointed at a directory someone
  else can write — which is precisely what stops it being the `/tmp` fallback this design
  refuses. And verified rather than trusted, by `is_private_runtime_dir()`: a directory, owned by
  the effective uid, with neither the group- nor the other-write bit set. That check is
  load-bearing rather than decorative, since macOS and the BSDs do not enforce socket-inode
  permissions on `connect()` at all — on those platforms the directory is the only thing between
  another local account and this player's transport controls.
- **With neither source available it is still non-fatal.** One `warn` naming the reason and
  `--control-socket`, and the player carries on serving audio — the shape a failed mDNS
  advertisement already has. In practice that is now the Linux systemd *system* unit case, which
  wants `RuntimeDirectory=` paired with `--control-socket`; `README.md` says so. There is
  deliberately no third source: a world-writable directory would let any local account pause
  playback and `switch` this endpoint out of its group.
- **A sibling `<path>.lock` under `flock()`**, through the *same* helper `-P` uses. The
  open/`flock`/classify sequence and its two messages moved out of `PidFile` into
  `daemon.h`'s `lock_file()`, because there are now two callers and both `README.md` and this
  file **assert** that the two "already running" refusals are worded alike — sharing the code
  is what makes that true rather than a coincidence two files apart. Held for the process's
  life, with `unlink()` + `bind()` underneath it, which is what makes "stale" and "in use"
  different answers rather than a guess: `unlink()`-then-`bind()` alone races a *live* daemon's
  socket away, and connect-to-probe is a TOCTOU. Its own lock rather than a second use of `-P`,
  so `--control-socket` does not acquire a dependency on `-P`. A lock already held is the one
  control-socket failure that is **fatal**; everything else warns and carries on. Taken above
  `make_audio_sink()`, for the reason the pidfile is: two instances racing should collide on a
  lock, not on the sound card.
- **And probed before the fork, so `-z` refuses a duplicate at the terminal.** The socket
  itself must be bound *after* `daemonize()` — it is one of the resources that invariant exists
  for — which leaves the refusal in a log the shell has stopped watching, and with no `-f`
  nowhere at all. So `probe_control_socket()` mirrors `probe_pidfile()`: the parent takes and
  drops the lock, the child acquires it for real, and only the lock is ever probed. Without it
  the parity `README.md` claims with `-P` was true of the wording and false of the place.
- **`argv[1]` split off before `getopt_long()`.** Not by reading getopt's leftovers: glibc
  permutes a positional argument out of the way and the BSDs stop at it, and `seek-rel -5000`
  is indistinguishable from a flag cluster to getopt regardless — so the subcommand and its
  arguments leave argv by *count*, from the table's own arity, before the scan. A subcommand
  after the flags is told to move rather than called junk. Every argument is validated at
  parse time, so `vol 500` fails at the terminal like a bad `--buffer-ms` rather than on the
  wire.
- **`stream` and the output format are two separate facts**, read separately, because a stream
  whose format the device *refused* has no format and is still a stream — and that is the case
  where knowing audio is arriving matters most, since it is arriving and being discarded.
  Inferring one from the other reported it as `stream: idle` at exactly the moment
  `PlayerListener` raises an ERROR about the opposite, answering "nothing is being sent to me"
  to an operator diagnosing "nothing is coming out". So `PlayerListener` owns an explicit flag
  set above every guard in `on_stream_start()`, and `streaming() && !stream_format()` is the
  refused case rather than an inconsistency. `StreamFormat` lives in `src/audio_sink.h`, next to
  the `configure()` argument list it mirrors, rather than in the control channel's header.
- **A one-command-per-connection wire format**: a line in, `ok` or `error <kind>: <reason>`
  and any payload out, then the daemon closes. The kind is one machine token so the client can
  map a refusal onto an exit status without parsing the reason, and the line still reads as
  English to `socat`. Nothing to version and nothing parsed twice.
- **104 new tests** (147 to 251), none of which binds a socket: the argv split, every
  subcommand's argument parse and its protocol mapping, a request round-trip through the wire
  form, the refusal predicate against hand-built snapshots (including the empty-
  `supported_commands` disconnected case), the `status` formatter, the reply status line, line
  framing — partial reads, no trailing newline, CRLF, an empty line, an over-long line, an
  embedded NUL, and bytes after the first newline — and every rejection path of the directory
  check, including both symlink directions and `/tmp`. They touch the filesystem, which this
  suite's boundary allows: what it forbids is opening a device, a socket or the mDNS daemon.
  `tests/scoped_env.h` was lifted out of `last_server_test.cpp` rather than copied so both
  suites share one `$XDG` helper.

**One bug this found, worth writing down**, because it was silent and
platform-specific: rebuilding argv without POSIX's `argv[argc] == NULL` sentinel breaks BSD
`getopt_long()`. Its long-option path does `optarg = nargv[optind++]` unconditionally and then
tests `optarg == NULL`, so the sentinel is the *only* thing that tells it a required value is
missing — without it, `--port` with no value read one past the end of the array and accepted
whatever was in memory. It surfaced as a test that segfaulted in a different case on each run.

**Also fixed here**, because it stood between the smoke test and these checks:
`check_default_mdns_boot` chose its expected outcome from whether the *host* had an mDNS
daemon, ignoring whether the *build* had mDNS at all — so the whole script failed partway
through against a `-DSENDSPIN_CLI_WITH_MDNS=OFF` build on a host with a daemon, which is a
configuration item 12 really produces. It now reads the build's own ready log too.

**Not in this slice:**

- **The socket path in a config file** → item 8, which owns configuration. `--control-socket`
  is a flag, and `Options::was_given(Opt::ControlSocket)` is the hook a precedence layer needs.
- **Hardware volume** → item 15. `vol` is a group command and does not touch the sink.
- **A `player`-scoped volume subcommand.** The `player` role's own volume and mute are
  reachable in the library and have no subcommand here, deliberately: every verb this item
  ships moves the *group*, which is what a controller does. This is **not** item 15, which is
  about driving a hardware mixer rather than about which scope a command addresses — so if a
  local "set just this box's level" verb is wanted, it is new scope on this item rather than
  something already tracked elsewhere.
- **An interactive TUI over this socket** → item 11. The wire format is deliberately one
  command per connection, so a long-lived subscribing client is a new shape rather than an
  extension of this one.
- **`--no-control` is kept but is not load-bearing.** Unlike `--no-mdns`, which the spec
  requires of a dialling client, nothing forces it now that the socket is `0600` inside a
  user-private directory. It stays because it silences the missing-`$XDG_RUNTIME_DIR` warning
  for a systemd system unit that is only ever driven by its server.

**What has and has not been exercised.** The socket was driven by hand on macOS against a real
daemon: every subcommand dispatched; `status` against a disconnected player; the
not-connected refusal for each transport verb; `--no-control`; the contradiction between
`--no-control` and `--control-socket`; a second instance refused before it opened a device or
a port; a `SIGKILL`ed daemon's stale socket taken over on restart; the socket gone after
`SIGTERM`; the `0600` mode read off the inode; the connection cap and the 5 s idle deadline;
the macOS fallback resolving with `$XDG_RUNTIME_DIR` unset and binding 0600
under `/var/folders`; and, through a raw socket, an empty line, an unknown command, an
out-of-range argument, an
embedded NUL, an over-long line, two lines in one write, CRLF, extra whitespace and a request
with no trailing newline. Clean under `-fsanitize=thread` while every subcommand was driven.
`ctest` and `scripts/smoke_test.sh` both pass on macOS, in the default and the
`-DSENDSPIN_CLI_WITH_MDNS=OFF` configurations.

**Driven against a real server.** A live `aiosendspin`/Music Assistant instance on the LAN, with
this player dialling out via `-s`, so the paths that need a server are now observed rather than
reasoned about: `play` and `pause` moving real playback; `mute on`/`off` round-tripping through the
server and coming back as state on *both* the group and the player line; `seek 60000` moving the
position; `seek` past the server's published `seek_max_ms` (231000, exactly the track duration)
refused locally with its own exit status; the `status` block filled in from real metadata —
`server: Music Assistant (connected)`, the track, the duration, the transport state from
`playback_speed`; `status` correctly reading `unknown` in the window before the first metadata
arrives; the socket bound at the macOS default path with **no flags at all**, gone after `SIGTERM`,
and a subcommand afterwards exiting 3.

**Position is only meaningful behind a sink that paces itself.** Under `-o null` the position
saturates at the track duration within seconds, and this is not a `status` bug: the null sink
consumes instantly and leaves `on_frames_played` unset, so nothing paces the stream and the server
runs through the whole track as fast as it can send it — `status` then faithfully reports a server
that believes the track has finished. Behind `-o portaudio`, where the device supplies real playout
timing, the position advances at 1x (five samples, 3 s of wall clock per 3 s of position). Worth
knowing before reading a position off a device-less run.

**And one bug in this item, which only listening caught.** `status` reported `player volume: 0`
at a player that was audibly playing at full output. The report was faithful to
`PlayerRole::get_volume()` and `get_volume()` was the wrong source: the role stores 0 until a
server sends a volume command and advertises that 0 in `client/state`, while `sink.set_volume()`
is reached *only* from `on_volume_changed()` — so every sink sits at `DEFAULT_SINK_VOLUME` (full)
until one arrives. An untouched player therefore plays at full while telling everyone it is at
zero, and `status` repeated the claim to the one person who could hear otherwise.

`status` now reports the gain the sink is really applying, tracked in `PlayerListener` — the only
caller of `set_volume()`, so the only thing that knows what the sink was told — and marks it
`(default; no server has set it)` so a server that deliberately chose full output is still
distinguishable from one that never spoke. `DEFAULT_SINK_VOLUME` replaces the bare `{100}` the
three sinks each spelled separately, with the disagreement written down beside it.

**The underlying incoherence is left for item 13**, which already owns advertised state that does
not match reality: we tell the server 0 while playing at full, so the first volume command a
server sends is heard as a *cut* from full rather than a rise from silence. Fixing it means
either seeding the role from the sink at startup (`update_volume(DEFAULT_SINK_VOLUME)`, which
changes what we advertise) or seeding the sink from the role (which makes a fresh player silent
until a server speaks). Both are behaviour changes wider than a control channel, and neither is
this item's to make.

**One server-side gap found, and it is worth reporting upstream.** `seek-rel` is accepted and does
nothing. The evidence that this is not ours: the local gate refuses any command absent from
`supported_commands`, and `seek-rel` exits 0, so the server *advertises* `seek_relative`;
`format_client_command_message()` writes `offset_ms` for `SEEK_RELATIVE` symmetrically with the
`position_ms` it writes for `SEEK`; and absolute `seek` through that same path demonstrably
moves playback. Three offsets were tried, including a negative one, with no effect. Per
`AI_POLICY.md` no issue was opened from here.

**One thing still not claimed.** The **kernel's `listen()` backlog is tied to
`MAX_CONTROL_CONNECTIONS`** rather than the two being independent, because whichever is smaller
is the real limit and only our own cap can explain itself in the log — with a backlog below the
cap, a burst of concurrent subcommands got `ECONNREFUSED` from the kernel and arrived at the
operator as "nothing is listening" on a healthy player. `ECONNREFUSED` and `ENOENT` are now
reported differently for the same reason, and a connection refused by the cap is *answered* in
the protocol's own shape rather than hung up on — the socket is 0600 inside a user-private
directory, so the peer is this user, and one short write is the difference between a subcommand
saying why it failed and reporting that the daemon dropped it.

**One caveat the macOS path carries.** The OS prunes `/var/folders` on a schedule, so a very
long-lived player could in principle have its socket unlinked from under it, which shows up as
subcommands reporting no daemon until it is restarted. `$XDG_RUNTIME_DIR` on Linux is tmpfs
cleared at logout, so it is the same class of impermanence rather than a macOS quirk, and
`--control-socket` is the answer to both.

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

Item 7 added a second path worth persisting and left it a flag: `--control-socket`, whose
default is derived from `$XDG_RUNTIME_DIR` and `--port`. Its `Opt::ControlSocket` entry is
already in the `was_given()` set, so it layers like the rest. Note the ordering constraint it
carries: the path's length is validated against `sockaddr_un::sun_path` at parse time, so
whatever supplies it from a config file has to be resolved before that check rather than
after it.

### 9. Docker

Image and compose file. ALSA with `/dev/snd` passed through for real output, and the
null sink for device-less containers and CI.

### 10. Packaging

`install()` rules, a systemd unit, and distribution packaging.

Item 6 shipped what a unit file needs and stopped at documenting it: `-z` forks so
`Type=forking` with `PIDFile=` pointing at `-P` works, and the foreground default suits
`Type=simple`. `README.md` says which; no unit is in the tree. `sd_notify` is not wired up,
which is what `Type=notify` would want instead.

**macOS distribution is the other half, and it is more than layout.** The binaries item 12
publishes are ad-hoc signed — `codesign` reports `adhoc, linker-signed`, which is the
minimum an arm64 Mach-O needs to execute at all and carries no identity — so `spctl -a -t
exec` rejects them and a user who unpacked in Finder is told the developer cannot be
verified. `README.md` answers that today with `xattr -d com.apple.quarantine`, which is a
workaround rather than a fix.

Clearing it properly needs a Developer ID Application certificate (so, a paid Apple
Developer Program enrolment) and notarization. The part that decides this belongs *here*
rather than with the matrix: `xcrun stapler` takes `.app`, `.dmg` and `.pkg` and **refuses a
bare Mach-O**, so notarizing the loose binary alone would still leave Gatekeeper asking
Apple on first run — no good for an offline Pi or Mac. The `.pkg` this item owes is what
makes the ticket stapleable, and therefore what makes the signing worth doing once.

Note also that a public repo gets no secrets on pull requests from forks, so any signing
step has to be conditional rather than assumed.

### 11. Interactive TUI mode

Optional, later. Upstream's `examples/tui_client` shows the shape.

### 12. CI and tests — *shipped (matrix and smoke test; sink contract still owed)*

`.github/workflows/ci.yml` builds and tests every push and pull request on `ubuntu-24.04`,
`ubuntu-24.04-arm` and `macos-14`, plus a fourth `ubuntu-24.04` leg configured
`-DSENDSPIN_CLI_WITH_MDNS=OFF` — which compiles `src/mdns_null.cpp` instead of
`src/mdns_dnssd.cpp`, so that translation unit is built rather than assumed. Every leg
configures `-DSENDSPIN_CLI_WERROR=ON` and runs the CTest suite, the no-mDNS leg included:
`discovery_test.cpp` links whichever `MdnsService` went in, and that configuration has no
other coverage. Each leg also asserts against its own configure output that it found the
backends it expects — a missing `-dev` package does not fail a configure, since every
backend is optional and auto-detected, so without that assertion the matrix would go green
on a null-sink-only, mDNS-less binary. The three platform legs additionally run the smoke
test and upload the binary they built, kept 14 days.

The unit harness is item 1's and unchanged: GoogleTest via `FetchContent` pinned to a tag,
wired to CTest with `gtest_discover_tests()`, defaulting ON only when this is the top-level
project. Nothing in `tests/` opens an audio device, a socket, or the mDNS daemon, which is
what keeps the suite runnable on a bare CI runner — and is why the smoke test is
`scripts/smoke_test.sh` rather than another suite. It covers what a gtest process cannot:
`--version`/`--help`, a foreground run reaching its ready log and exiting 0 on `SIGTERM`,
`-z` forking with `-P` writing a live pidfile and refusing a second instance holding the
same lock, a default mDNS-on run surviving a daemon it cannot reach, and — item 7's, since a
listening socket and a `connect()` to it are two processes by definition — the control socket
appearing at its default path as `0600`, a `status` round trip, `--no-control`, stale-socket
takeover after a `SIGKILL`, removal on `SIGTERM`, and a second instance refused both in the
foreground and, through the pre-fork probe, at the terminal under `-z`. It is runnable by hand
against any build, which is what item 6's hand-driven `-z` checks became.

**Both breaks this entry used to predict are fixed.** `src/mdns_dnssd.cpp` no longer names
`kDNSServiceErr_ServiceNotRunning` and `kDNSServiceErr_Timeout` unconditionally.
`#ifdef` guards — what this entry previously recommended — would **not** have worked: both
are enumerators of an anonymous `enum` rather than macros, so the test is false on every
platform, and it would have dropped the two cases on macOS as well as Linux. CMake compiles
a use of each against the header the build will really use and defines
`SENDSPIN_CLI_HAVE_ERR_*` only where it is genuinely there. The dangling
`this->name().c_str()` at `src/portaudio_sink.cpp:495` is gone as well, which is what lets
`-Werror` hold at all.

**Both runtime claims this owed item 5 are now made rather than read.** The linux-x86_64 leg
starts a real `avahi-daemon`, browses our own `_sendspin._tcp` advertisement back to a
resolved address, then publishes a `_sendspin-server._tcp` instance and has the player
discover it. That second half is the one that matters: a `ws://` URL cannot be built without
an address, so it cannot succeed unless `libavahi-compat-libdnssd` really does implement
`DNSServiceQueryRecord` for A and AAAA — the call it provides in place of the
`DNSServiceGetAddrInfo` it lacks entirely. The Linux legs' configure assertion is what pins
down the other half, that `find_path`/`find_library` pick the compat library up.

**Still owed.** The sink contract remains untested: a `NullAudioSink`/`AlsaAudioSink`/
`PortAudioSink` suite is what would cover it, and is the largest gap left in `tests/`.

**And a ThreadSanitizer leg, which item 7 made more than a nicety.** Every no-background-thread
argument in this repo is a threading argument — item 5's `poll()`-from-the-main-loop discovery
and item 7's control socket both rest on library calls documented as main-loop-only — and the
only verification either has on record is a developer running `-fsanitize=thread` by hand, on
macOS. A leg that configures `-DCMAKE_CXX_FLAGS=-fsanitize=thread` and runs
`scripts/smoke_test.sh` under it would turn the largest correctness claim in the tree from
reasoning into a check. It is cheap: the smoke test already drives the whole boot, socket and
shutdown path, and produced zero reports when run that way by hand.

The matrix also has no armv7 or 32-bit Pi leg, and no macOS x86_64 leg. Artifacts are per-commit
workflow artifacts only — tagged releases, `install()` rules and distribution packaging all
belong to item 10, which will replace the workflow's hand-rolled tar with a staged
`cmake --install` payload.

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
