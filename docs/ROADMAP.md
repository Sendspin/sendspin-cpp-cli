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
  auto-detected ALSA (item 2), PortAudio (item 3), PulseAudio (item 18) and PipeWire
  (item 19) backends, with the device-less null/stdout sink as the fallback, so the
  binary still runs where there is no sound card.
- Parses the squeezelite-style flag surface: `-o -l -n -s -z -P -d -f --port --buffer-ms
  --no-mdns --mdns-name --help --version`, validating every value at parse time and
  refusing to start on a bad one (item 1).
- Runs as a real daemon: `-z` forks and detaches, `-P` holds a locked pidfile that refuses a
  second instance and needs no stale cleanup, and every log line carries a level letter and
  a subsystem tag — timestamped and `SIGHUP`-reopenable under `-f` (item 6).
- Advertises the formats the selected output device will actually take, derived by probing
  it and crossed with what each codec can carry -- in priority order, so a server picking
  automatically lands on FLAC 2 ch 16-bit @ 48 kHz (item 4).
- Can be driven from its own host: a `0600` Unix control socket in a user-private directory,
  polled from the main loop, and `sendspin-cli <subcommand>` on the same binary covering the
  whole of `controller@v1` — `status`, `play`/`pause`/`stop`/`next`/`prev`, `vol`, `mute`,
  `seek`, `seek-rel`, `repeat`, `shuffle`, `switch` (item 7).
- Runs stream hooks: `--hook-start`/`--hook-stop` shell commands on stream start and stop,
  with the event's facts in the same `SENDSPIN_*` environment variables the Python CLI
  exports, spawned and reaped without ever blocking the audio path (item 22).
- Reads a config file whose keys are the long flag names, layered under the command line, and
  remembers its own state across restarts — the last server, the static delay a server set, and
  its volume and mute — in a separate file it writes atomically at `0600` (item 8).
- Installs: `cmake --install --component sendspin-cli` lays down the binary, an annotated
  example config, and on Linux a systemd system unit that pairs `RuntimeDirectory=` and
  `StateDirectory=` with the flags they exist for — and CI publishes that same payload,
  staged with `DESTDIR`, instead of a hand-rolled tar (item 10).

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
  `snd_pcm_resume()` with a `prepare()` fallback. Anything either of those cannot clear is
  device loss, handled through `SinkRecovery` — see item 14.
- Device enumeration for `-l` through `snd_device_name_hint()`, and `-o <any PCM name>`
  — squeezelite's model, with `null` / `stdout` / `-` still reserved. `-o` defaults to
  `default` wherever the backend is compiled in.
- Software volume: Q32 fixed-point sample scaling. The taper started as upstream's quadratic
  one and is now the spec's `(volume/100)^1.5` (item 7). Item 3 moved it to `src/pcm_volume.{h,cpp}`
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
- **In priority order, not probe order.** The protocol has `supported_formats` first-preferred
  and servers read it that way -- Music Assistant's `aiosendspin` takes the first entry it can
  encode and never looks further. Emitting the ladders in the ascending order they are probed
  in therefore nominated the *worst* format the device would take: FLAC at 22050 Hz on ALSA
  `default`, whose plugin accepts the whole ladder. `supported_formats()` ranks instead, on
  three named ladders -- codec `FLAC, OPUS, PCM`; rate `48000, 44100, 96000, 88200, 192000,
  176400, 32000, 22050`; depth `16, 24, 32, 8` -- so a permissive device leads with FLAC 2 ch
  16-bit @ 48 kHz, and one that takes neither 48 nor 44.1 kHz falls to its best remaining rate
  rather than its lowest. A permutation and never a filter: the same list is the menu a user
  picks a format from by hand, so ranking it must not take entries away.
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

**Userinfo in a `-s` URL is masked wherever this player names a server.** A `-s` value may
carry userinfo — `ws://user:token@host:8927/sendspin` — and `parse_server_url()` passes a
full URL through verbatim, so the whole of it reached `Connecting to %s` at `info` on every
dial, repeating under the retry pacer's backoff. `redact_url_userinfo()` in
`src/cli.{h,cpp}` is what names a server now: it masks the secret half of the userinfo and
keeps the rest, so the line still says which endpoint was dialled —
`ws://user:***@host:8927/sendspin`, or `***@host` where a single userinfo field could be a
bearer token and there is nothing to tell it from one. A fixed `***` rather than one `*` per
character, a secret's length being worth nothing to a reader. The dial itself is handed the
real URL, and nothing credential-bearing reaches disk: `last-server` holds the server *id*,
not a URL. Every rejection `parse_server_url()` writes goes through the same helper, a URL
that does not parse being the one most likely to have been mistyped around a password. The
helper is unit-tested on its own; that the dial sink actually calls it is a
`scripts/smoke_test.sh` check, `src/main.cpp` not being linked by the test binary.

**Userinfo is the boundary, and it is the boundary because of where the spec puts
authentication.** The spec authenticates in the handshake — pairing and a PSK — so a credential
in a URL is never something this protocol asked for; it is something *in front of* the server
asking, a reverse proxy wanting HTTP Basic being the case this came from. And it does not reach
that proxy either: the 6-argument `UrlParser::parse()` IXWebSocket's transport calls copies out
scheme, host, path, query and port and **discards** the user and password it just parsed
(`IXUrlParser.cpp`), and nothing in either library builds an `Authorization` header. Confirmed
against a listening socket rather than by reading: the upgrade request carries `Host`,
`Upgrade`, `Connection`, `Sec-WebSocket-*` and `Origin`, and no credential in any form. So
userinfo here is *tolerated and dropped*, and the reason to mask it is that an operator will
still type it, not that it does anything.

**Two limits on the masking, both of them real.** A credential written into a *query* string is
not masked: `?token=` is not a shape this player can tell from any other query, and guessing at
parameter names would mask what is not secret while missing what is. And the mask reads the URL
the way a URL parser does — the authority ends at the first `/`, `?` or `#` — so a userinfo
field containing one of those *unencoded* ends the authority early, leaves no `@` inside it, and
comes back unmasked. A raw base64 secret is the case that bites: `/` is in the alphabet, so a
32-character token carries one about two times in five. RFC 3986 requires those percent-encoded
in userinfo and IXWebSocket's own parser says the same, and such a URL does not resolve to the
endpoint the operator meant in the first place — the host reads as everything up to that `/` —
so this is a malformed value being printed faithfully rather than a parse to be repaired by
guesswork. `README.md` and the wiki both say to percent-encode.

**What is owed, and it is the honest answer to all of the above: stop accepting a credential
this player cannot send.** Refusing a `-s` value that carries userinfo closes every leak by
construction — a URL that is refused reaches neither `Connecting to %s` nor `connect_to()`, so
it never reaches the `sendspin.*` lines either — and it closes the unencoded-delimiter shape
with it, since that shape cannot reach the endpoint it appears to name in any case. It also
stops the worse failure this item only documents: an operator running a player they believe is
authenticated. It is a breaking change for anyone passing such a URL today, which is why it is
owed here rather than done here. Masking is what this item ships; refusing is the fix.

**A third thing this item does not claim: the library's own dial lines.** sendspin-cpp v0.7.0
logs the URL it is dialling at `info` from `ConnectionManager::connect_to()` and again at
`error` from `SendspinClientConnection`, through the same sink-less `SS_LOG*` macros that put
timestamps and per-tag filtering out of reach above — so those lines still carry whatever the
URL carries, at the default level, and no call this layer can make will change it.
`connect_to()` takes a URL and nothing else, so the credentials cannot be moved off it either.
A pipe over fd 2 would catch them and is rejected here for the reason it is rejected above.
This one is sendspin-cpp's to fix, in the same place a log sink hook belongs; per
`AI_POLICY.md` no upstream issue was opened here. Until it is, the boundary is exactly the tag
prefix: every line tagged `sendspin.*` is the library's, and every line tagged otherwise is
this player's and is held to the paragraph above.

### 7. Local control channel — *shipped*

The player could only be driven by a remote controller, and `CMakeLists.txt` pinned
`SENDSPIN_ENABLE_CONTROLLER OFF` with a comment saying the role was "for driving *other*
clients, which this daemon does not do". That was wrong, and it was the thing blocking this
item: `controller@v1` carries the transport verbs for the group this client is *part of*, and
the `player` role has none of them — only this endpoint's own volume, mute and static delay.
So the role is now on, and the deliberate addition to the squeezelite model lands here.

**Shipped** in `src/control.h`, `src/control_common.cpp`, `src/control_socket.cpp`,
`src/control_client.cpp`, `src/cli.{h,cpp}`, `src/main.cpp`, `src/player_listener.{h,cpp}`,
`src/audio_sink.h`, `src/daemon.{h,cpp}`, `src/log.h`, `CMakeLists.txt`,
`tests/control_test.cpp`, `tests/scoped_env.h` and `scripts/smoke_test.sh`:

- **The whole of `controller@v1`, one subcommand each**: `status`, `play`, `pause`, `stop`,
  `next`, `prev`, `vol`, `mute`, `seek`, `seek-rel`, `repeat`, `shuffle`, `switch`. `repeat`
  and `shuffle` are the two that are not pass-throughs — the mode *is* the command
  (`REPEAT_OFF`/`ONE`/`ALL`, `SHUFFLE`/`UNSHUFFLE`), so `shuffle off` is its own command
  rather than a false-valued parameter. A test walks the table against the library's enum, so
  a protocol command with no subcommand behind it fails the suite.
- **`vol` is *group* volume**, and `status` prints `group volume` and `player volume` as two
  named lines rather than one `volume:`. The server spreads a group volume across the group and
  clamps per player, so a squeezelite refugee's expectation that `vol 50` moves *this* box is
  wrong, and one ambiguous line would leave them unable to see that. `switch` is documented for
  what the spec's switch cycle does — re-home this client between groups — not as the "switch
  playback source" its library comment suggests.
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
  and a command absent from the server's `supported_commands` (5). The ordering is the
  load-bearing part — `on_controller_state_clear()` *empties* `supported_commands` on a
  disconnect, so a gate that consulted it first would answer "pause is not supported" when the
  truth is that nothing is connected, sending the operator to read their server's capabilities
  instead of its connection. `send_text()` also no-ops silently with no connection, so nothing
  may report success for a command that never left the process.
- **A `0600` socket at `<runtime dir>/sendspin-cli-<port>.sock`**, the port in the leaf so two
  players on one host each get their own. The mode is set by bracketing `bind()` with a
  `umask()` rather than a later `chmod()`: `bind()` applies the umask, `daemonize()` sets
  `umask(0022)`, and a post-`bind()` `chmod()` leaves a window where any local account can
  connect.
- **Two sources for the runtime directory, and no `/tmp` among them.** `$XDG_RUNTIME_DIR`
  wherever it is set, on every platform, trusted because it is the user's own declaration — and
  checked, so a group-writable one is used but warned about. Then the platform's own
  equivalent, which on macOS is `confstr(_CS_DARWIN_USER_TEMP_DIR)`, the per-user
  `/var/folders` directory launchd already provides. That second source exists because launchd
  sets no `$XDG_RUNTIME_DIR` *at all*, not even in an interactive shell, so without it the
  default path never resolved on the one platform the PortAudio backend exists to serve.
  Deliberately **not `$TMPDIR`**, which usually names the same directory: `confstr()` reads
  nothing from the environment, so unlike `$TMPDIR` it cannot be pointed at a directory someone
  else can write — which is precisely what stops it being the `/tmp` fallback this design
  refuses. Verified rather than trusted by `is_private_runtime_dir()`: a directory, owned by the
  effective uid, with neither the group- nor the other-write bit. That check is load-bearing
  rather than decorative, since macOS and the BSDs do not enforce socket-inode permissions on
  `connect()` at all — there the directory is the only thing between another local account and
  this player's transport controls. Both sources are impermanent by design (macOS prunes
  `/var/folders`, Linux clears `$XDG_RUNTIME_DIR` at logout), and `--control-socket` is the
  answer where that matters.
- **With neither source available it is still non-fatal.** One `warn` naming the reason and
  `--control-socket`, and the player carries on serving audio — the shape a failed mDNS
  advertisement already has. In practice that is the Linux systemd *system* unit case, which
  wants `RuntimeDirectory=` paired with `--control-socket`. There is deliberately no third
  source: a world-writable directory would let any local account pause playback and `switch`
  this endpoint out of its group.
- **A sibling `<path>.lock` under `flock()`**, through the *same* helper `-P` uses. The
  open/`flock`/classify sequence and its two messages live in `daemon.h`'s `lock_file()`,
  because there are now two callers and both `README.md` and this file **assert** that the two
  "already running" refusals are worded alike — sharing the code is what makes that true rather
  than a coincidence two files apart. Held for the process's life, with `unlink()` + `bind()`
  underneath it, which is what makes "stale" and "in use" different answers rather than a
  guess: `unlink()`-then-`bind()` alone races a *live* daemon's socket away, and
  connect-to-probe is a TOCTOU. Its own lock rather than a second use of `-P`, so
  `--control-socket` does not acquire a dependency on `-P`. A lock already held is the one
  control-socket failure that is **fatal**; everything else warns and carries on. Taken above
  `make_audio_sink()`, for the reason the pidfile is: two instances racing should collide on a
  lock, not on the sound card. And probed before the fork, mirroring `probe_pidfile()`, so a
  duplicate under `-z` is refused at the terminal rather than in a log the shell has stopped
  watching — the socket itself must be bound *after* `daemonize()`, being one of the resources
  that invariant exists for.
- **`argv[1]` split off before `getopt_long()`.** Not by reading getopt's leftovers: glibc
  permutes a positional argument out of the way and the BSDs stop at it, and `seek-rel -5000`
  is indistinguishable from a flag cluster to getopt regardless — so the subcommand and its
  arguments leave argv by *count*, from the table's own arity, before the scan. A subcommand
  after the flags is told to move rather than called junk. Every argument is validated at parse
  time, so `vol 500` fails at the terminal like a bad `--buffer-ms` rather than on the wire.
- **A one-command-per-connection wire format**: a line in, `ok` or `error <kind>: <reason>` and
  any payload out, then the daemon closes. The kind is one machine token so the client can map
  a refusal onto an exit status without parsing the reason, and the line still reads as English
  to `socat`. Nothing to version and nothing parsed twice. The kernel's `listen()` backlog is
  tied to `MAX_CONTROL_CONNECTIONS` rather than chosen independently, because whichever is
  smaller is the real limit and only our own cap can explain itself in the log; a connection
  refused by the cap is *answered* in the protocol's own shape rather than hung up on, and
  `ECONNREFUSED` and `ENOENT` are reported differently — a socket with nothing behind it is a
  different problem from no socket at all.
- **`status` reports what is true locally, and marks what is not.** Four decisions, each one a
  line that used to say something it had not earned:
  - **The player's volume is the gain the sink is applying**, tracked in `PlayerListener` — the
    only caller of `set_volume()`, so the only thing that knows what the sink was told — and
    marked `(default; no server has set it)` until a server sends one. `PlayerRole::get_volume()`
    is the wrong source: it stores 0 until a server speaks, while every sink starts at
    `DEFAULT_SINK_VOLUME`, so an untouched player plays at full while the role reads zero.
  - **`stream` and the output format are two separate facts.** A stream whose format the device
    *refused* has no format and is still a stream, and that is when knowing audio is arriving
    matters most, since it is arriving and being discarded. So `PlayerListener` owns an explicit
    flag set above every guard in `on_stream_start()`, and `streaming() && !stream_format()` is
    the refused case rather than an inconsistency. It is also the only thing that tells `pause`
    from `stop` in the output: both leave `state: paused`, but `pause` keeps `stream: receiving`
    where `stop` drops it to `idle`.
  - **`repeat` and `shuffle` are reported at all**, beside the group volume they arrive with,
    because this CLI can change them and their effect would otherwise be invisible.
  - **`position` says `(estimated)` while playing**, and one `note:` line names `state`,
    `position`, `repeat` and `shuffle` as the server's word. See the staleness note below.
- **The software volume taper is the spec's**, `amplitude = (volume / 100)^1.5`, replacing the
  `^2` inherited from upstream's `PortAudioSink::update_volume_multiplier_()`. Not a taste call:
  the spec defines a volume as *perceived loudness* — "volume 50 should be perceived as half as
  loud as volume 100" — and `^1.5` is the mapping that makes the number mean that. **This is
  audible for every existing user**, and only in one direction: every volume below 100 gets
  louder, by about 3 dB at 50, 6 dB at 25 and 10 dB at 10. Computed in floating point because it
  runs once per volume change, not per sample; `apply_volume()` stays integer Q32. Pinned on the
  two volumes where the curve is exact — `(1/4)^1.5` is `1/8`, `(1/25)^1.5` is `1/125` — plus a
  test asserting the perceptual property directly, and one guarding the divergence from upstream
  so it cannot be tidied back.
- **`StreamFormat` lives in `src/audio_sink.h`**, next to the `configure()` argument list it
  mirrors, so the audio adapter does not depend on the control channel's header.
- **110 new tests** (147 to 257), none of which binds a socket: the argv split, every
  subcommand's argument parse and its protocol mapping, a request round-trip through the wire
  form, the refusal predicate against hand-built snapshots (including the empty
  `supported_commands` disconnected case), the `status` formatter, the reply status line, line
  framing — partial reads, no trailing newline, CRLF, an empty line, an over-long line, an
  embedded NUL, bytes after the first newline — and every rejection path of the directory check,
  including both symlink directions and `/tmp`. They touch the filesystem, which this suite's
  boundary allows: what it forbids is opening a device, a socket or the mDNS daemon.
  `tests/scoped_env.h` was lifted out of `last_server_test.cpp` rather than copied so both
  suites share one `$XDG` helper.

**Three things worth writing down**, each one silent if got wrong:

- **Rebuilding argv without POSIX's `argv[argc] == NULL` sentinel breaks BSD `getopt_long()`.**
  Its long-option path does `optarg = nargv[optind++]` unconditionally and *then* tests
  `optarg == NULL`, so the sentinel is the only thing that tells it a required value is missing.
  Without it, `--port` with no value read one past the end of the array and accepted whatever
  was in memory. It surfaced as a test that segfaulted in a different case on each run.
- **A player must report the volume it is actually applying**, which is why `main.cpp` calls
  `player.update_volume(DEFAULT_SINK_VOLUME)` before anything can connect. Four spec rules make
  it necessary rather than tidy: `client/state`'s player `volume` **MUST** be included when the
  `volume` command is advertised; *"group volume is the average of the volumes of players in the
  group that support the `volume` command"*, so group volume is **derived from us** and a
  misreport corrupts the group reading for every controller; setting group volume works off
  *"delta = requested_volume - current_group_volume"*, so that misreport then mis-applies every
  later group volume change by exactly the error — a player claiming 0 while playing at full
  hears a request for 30 as a cut from full rather than a rise from silence; and *"a server MUST
  NOT assume these values are unchanged after a reconnect"*. `update_volume()` rather than
  reaching for the sink, because it does not invoke `on_volume_changed()` — that fires only for
  server-initiated changes — so `status` still tells a default apart from a volume a server chose.
- **Four `status` fields are the server's word and can lag what is true**, which is what the
  `note:` line exists to say. `position` is interpolated forward from the last progress the
  server sent whenever the group is playing, so a server that does not resend progress after a
  seek leaves the anchor stale and the figure drifts by however far the seek moved; it is marked
  `(estimated)` while playing and only while playing, since paused it is the server's own
  snapshot. `state` comes from the same progress object and shares that staleness. `repeat` and
  `shuffle` are reported when the server sends them and default to `off` when it does not,
  because `ServerStateControllerObject` holds them as a plain enum and a plain bool and the
  parser assigns them only when the field is present — `seek_max_ms` in the same object is an
  `optional` and does not have the problem. The spec does not oblige a server to republish state
  after acting, so **an unchanged figure is not evidence a command did nothing**; one `note:`
  line rather than a qualifier per field, so the block stays scannable and every line stays
  `key: value`.

**Also fixed here**, because it stood between the smoke test and these checks:
`check_default_mdns_boot` chose its expected outcome from whether the *host* had an mDNS
daemon, ignoring whether the *build* had mDNS at all — so the whole script failed partway
through against a `-DSENDSPIN_CLI_WITH_MDNS=OFF` build on a host with a daemon, which is a
configuration item 12 really produces. It now reads the build's own ready log too.

**Not in this slice:**

- **The socket path in a config file** → item 8, which owns configuration. `--control-socket`
  is a flag, and `Options::was_given(Opt::ControlSocket)` is the hook a precedence layer needs.
  Note the ordering constraint: the path's length is validated against `sockaddr_un::sun_path`
  at parse time, so whatever supplies it has to be resolved before that check.
- **Two remaining spec deviations in the volume path** → item 13, which owns advertised state
  matching reality, and where both shipped. Persisting `volume` and `muted` across restarts is the
  spec's RECOMMENDED and needed item 8's store. And the spec says volume changes SHOULD be ramped
  to avoid clicks; nothing in *this* slice ramped, which item 13 closed with the shared ramp in
  `src/pcm_volume.{h,cpp}`. The third — the taper — is fixed here, see below.
- **Hardware volume** → item 15. `vol` is a group command and does not touch the sink.
- **A `player`-scoped volume subcommand.** The `player` role's own volume and mute are reachable
  in the library and have no subcommand here, deliberately: every verb this item ships moves the
  *group*, which is what a controller does. This is **not** item 15, which is about driving a
  hardware mixer rather than about which scope a command addresses — so a local "set just this
  box's level" verb would be new scope on this item rather than something tracked elsewhere.
- **An interactive TUI over this socket** → item 11. The wire format is deliberately one command
  per connection, so a long-lived subscribing client is a new shape rather than an extension.
- **`--no-control` is kept but is not load-bearing.** Unlike `--no-mdns`, which the spec requires
  of a dialling client, nothing forces it now that the socket is `0600` inside a user-private
  directory. It stays because it silences the missing-runtime-directory warning for a systemd
  system unit that is only ever driven by its server.

**What has and has not been exercised.** `ctest` and `scripts/smoke_test.sh` both pass on macOS
in the default and `-DSENDSPIN_CLI_WITH_MDNS=OFF` configurations, and the whole smoke path is
clean under `-fsanitize=thread`. The smoke test covers what needs two processes: the socket at
its default path as `0600`, a `status` round trip, `--no-control`, stale-socket takeover after a
`SIGKILL`, removal on `SIGTERM`, a second instance refused both in the foreground and — through
the pre-fork probe — at the terminal under `-z`, and the missing-runtime-directory branch on
whichever side of it the platform falls. By hand on macOS: the `0600` mode read off the inode,
the connection cap, the 5 s idle deadline, and through a raw socket an empty line, an unknown
command, an out-of-range argument, an embedded NUL, an over-long line, two lines in one write,
CRLF, extra whitespace and a request with no trailing newline.

**Driven against a live `aiosendspin`/Music Assistant server on the LAN**, dialling out with
`-s`, which is what moved the server-dependent paths from reasoned to observed. All thirteen
subcommands act. `play`, `pause` and `stop` move real playback, and `stop` is distinguishable
from `pause` only by the `stream` line. `next` and `prev` walk a real queue in both directions.
`seek` moves the position, and `seek` past the server's published `seek_max_ms` (231000, exactly
the track duration) is refused locally with its own exit status. `seek-rel`, `repeat one` and
`shuffle` were confirmed **behaviourally rather than from reported state**, since this server
acts on all three without republishing any of them: a relative jump to a track's opening was
audibly indistinguishable from the absolute seek used as a control; `shuffle off` walked an album
in order where `shuffle on` produced tracks from four different albums; and `repeat one` held a
track across its end instead of advancing. `vol` and `mute` were confirmed by ear, alternating
12/50 with a mute between. `switch` re-establishes the stream, visible as `Stream ended` followed
by `Stream started` and a fresh codec header. The `status` block fills in from real metadata, and
reads `unknown` in the window before the first arrives. Reporting the sink's real gain makes the
server report `group volume: 100` for a single player at 100 where it reported 0 before, and a
following `vol 30` lands at a real 30. The three failure statuses were exercised against real
daemons rather than only unit-tested: all twelve transport commands return **4** against a daemon
that is up and has never reached a server, each naming the connection rather than the command,
while `status` on that same daemon returns **0** and prints what it knows locally; and **3**
against a socket with no daemon behind it.

**Two limits worth knowing before reading a `status`.** The position is only meaningful behind a
sink that paces itself: under `-o null` it saturates at the track duration within seconds, because
the null sink consumes instantly and leaves `on_frames_played` unset, so nothing paces the stream
and the server sends the whole track as fast as it can — `status` then faithfully reports a server
that believes the track has finished. Behind `-o portaudio` it advances at 1x. And `switch` was
only ever exercised with a single group available, where the cycle returns to where it began; the
multi-group path is unproven.

### 8. Config file and state store — *shipped*

A daemon needed its whole flag line every time, and it forgot everything when it stopped.
Item 5's `src/last_server.{h,cpp}` held one server id in a file of its own, and the library's
`SendspinPersistenceProvider` was never implemented — so `set_static_delay_adjustable(true)`
advertised an adjustable delay with nothing behind it, leaving a spec requirement unmet:
*"Clients must persist `static_delay_ms` locally across reboots and server reconnections."*

**Shipped** in `src/key_value_file.{h,cpp}`, `src/state_store.{h,cpp}`,
`src/config_file.{h,cpp}`, `src/cli.{h,cpp}`, `src/main.cpp`, `src/player_listener.{h,cpp}`,
`src/audio_sink.h`, `src/control.h`, `src/control_common.cpp`, `CMakeLists.txt`,
`tests/state_store_test.cpp`, `tests/config_file_test.cpp`, `tests/parse_harness.h` and
`scripts/smoke_test.sh`:

- **Two files, and the split is the point.** The config file is the operator's and is only ever
  read; the state file is the daemon's and is only ever written by it. A daemon that rewrote its
  own config would destroy the comments and ordering someone put there, and a config the daemon
  could not write would have nowhere to record a volume. One flat `key = value` reader
  (`src/key_value_file.h`) serves both, so the two formats cannot drift; what differs is only
  that the config *refuses* a line it cannot read and the state store skips it. Nothing but this
  daemon writes the latter, so a bad line there means corruption rather than a typo.
- **Keys are the long flag names minus the dashes**, which is why `-o -n -s -P -f -d` gained
  `--output --name --server --pidfile --logfile --log-level`. One vocabulary, so `--help` is the
  config reference rather than a second document to keep in step.
- **Search order `--config` → `$XDG_CONFIG_HOME` → `$HOME/.config` → `/etc/sendspin-cli.conf`,
  first found used whole.** No merging across layers, and no `$XDG_CONFIG_DIRS` traversal — that
  is exactly where this item's scope would have inflated. There is deliberately no
  `--no-config`: a named file that cannot be read is fatal while a missing layer is silent, so
  `--config /dev/null` already says it. A file that exists and does not parse stops the run
  rather than falling through, because an operator's config that is quietly skipped is the
  failure mode this surface exists to avoid.
- **The merge sits inside `parse_options()`, after the getopt loop and above every resolution.**
  That position is load-bearing rather than tidy. Above it, `was_given()` still means what the
  command line said, so precedence is one test per option. Below it, the `-s` URL parse, the
  `-z`-with-`-o stdout` contradiction, `--no-control` against `--control-socket`, and the socket
  path's absolutization and `sun_path` length check all run over merged options without knowing
  a file was involved — which satisfies item 7's "resolved before the length check" constraint
  for free. Two alternatives were rejected: seeding `Options` *before* getopt destroys
  `was_given()`, the entire hook item 1 built, and a separate `load_config()` the caller composes
  puts merge order in `main.cpp` on every subcommand path and has to re-run the resolution
  `parse_options()` owns.
- **The merge marks options as supplied, not merely set.** `Options::advertises()` is
  `!no_mdns && !was_given(Opt::Server)`, so a configured `server` left unmarked would have this
  player dial *and* advertise `_sendspin._tcp` — which the spec forbids — while `server_url` was
  never filled, leaving the value inert as well as non-compliant. So the bit now means "this was
  supplied" rather than "the user typed this", and there is no second bitmask: nothing downstream
  needs to tell the two apart. The one exception is *diagnostics* — a small origin map lets the
  two refusals that happen after the merge still name the file and line.
- **One validator per option.** Every settable option goes through a single `apply_option()`,
  which both the getopt switch and the merge call, so `buffer-ms = 5` is refused with exactly the
  message `--buffer-ms 5` gets, prefixed with the file and line. An unknown key or a malformed
  line is fatal the same way. `--help`, `--version` and `-l` short-circuit above the config
  entirely: a broken config must not stop `--help` explaining how to fix it.
- **Config-settable is the `Opt` enum minus run shape** — `-l`, `-z`, `--config`, `--help`,
  `--version` and any subcommand are out, and a config naming one is refused as an unknown key.
  Excluding is reversible; debugging a `daemonize` that came out of a file under systemd is not.
- **A subcommand reads the config too**, or `sendspin-cli status` would look for a player on the
  default port rather than the configured one. It applies only `port` and `control-socket` — the
  two it actually reads — while still *validating* the rest, so the "a subcommand reads only
  --port and --control-socket" warning stays a report of what was typed instead of firing at
  every operator who has a config file at all.
- **`src/last_server.{h,cpp}` folded away** into `StateStore`, which now holds `last-server`,
  `last-server-hash`, `static-delay-ms`, `volume` and `muted`. Writes go through a temporary, an
  `fsync` and a `rename` at mode `0600`: a player whose usual way of stopping is losing power
  must not leave a half-file a later run has to reason about. `--state-dir` overrides the XDG
  search outright, because a systemd **system** unit has neither variable and gets
  `/var/lib/sendspin-cli` from `StateDirectory=` — which is the pairing item 10's unit ships.
- **`last-server` and `last-server-hash` stay two independent keys with nothing reconciling
  them,** and the constraint item 5 found is why. The provider's key is an FNV1 hash computed by
  `ConnectionManager::fnv1_hash()`, which lives in the library's uninstalled `src/` — so we store
  the number we are handed and hand it back, and never compute it. Discovery's own tie-break
  needs the raw id, which the hash cannot be turned back into.
- **`CliPersistenceProvider` is installed before `add_player()` and `start_server()`**, and
  neither is negotiable: the pointer is copied into `PlayerRole` at construction, and
  `start_server()` is what loads the remembered hash. Installed after either, it is a provider
  the library never asks.
- **Volume and mute are the CLI's own half**, since the provider has no hook for either.
  `PlayerListener` writes through on every server change, and startup seeds the sink, the
  listener's applied pair and the role together. `status` grew a third case for it: a restored
  volume is neither the sink default nor something this connection's server chose, so
  `player_volume_from_server` became `VolumeSource`.

Deliberately **not** here, and each one names its owner rather than being left implied:

- **Observing and locally setting the static delay** — item 13's, and the half this item does not
  touch. It remembers and reports the figure honestly; reading it back out in `status` and setting
  it with `sendspin-cli delay` shipped there. (This item originally recorded the remaining half as
  "applying it to the audio path". That was a misreading: the library's sync task had been
  subtracting the delay all along — see item 13, which corrects it.)
- **Ramping volume changes** — item 13's too, where it shipped.
- **Debouncing the state write.** Every distinct volume a server sends costs one whole-file
  rewrite plus an `fsync`, on the main loop. Two things blunt it: the library's command slot is
  latest-wins within a drain, and `set_all()` short-circuits a value that has not changed — so a
  slider drag costs at most one write per main-loop tick, not one per message, and audio is
  untouched either way since it runs on the sync task's own thread. What it *can* delay is the
  control socket and the mDNS poll beside it, on a slow enough card. Left as-is because writing on
  change is what makes the file true at any instant a player might lose power, which is this
  item's whole point; if the cost shows up in practice, a dirty flag flushed on a timer and at
  shutdown is the fix. Still owed: item 13 shipped its volume work without needing it.
- **Two players sharing one `$XDG_STATE_HOME`** still share one state file and overwrite each
  other's keys. `--state-dir` is the answer; deriving the filename from `--port` was left out
  rather than guessed at.

### 9. Docker

Image and compose file. ALSA with `/dev/snd` passed through for real output, and the
null sink for device-less containers and CI.

### 10. Packaging — *shipped (install rules, the unit, the CI payload, the tagged release, the macOS `.pkg`, the unit's own user and hardening; signing still owed)*

Nothing in this tree could be installed. `cmake --build` left the binary where it built it, and
`.github/workflows/ci.yml` hand-rolled a tarball around it with three `cp` paths inside a
workflow step — no `install()` rule anywhere for those paths to agree with. Item 6 had shipped
everything a unit file needs and then stopped at *documenting* it, so `README.md` told an
operator to choose between `Type=simple` and `Type=forking` and write the unit themselves.

**Shipped** in `CMakeLists.txt`, `packaging/sendspin-cli.service.in`,
`packaging/sendspin-cli.sysusers.conf`, `packaging/sendspin-cli.conf.example`,
`scripts/build_macos_pkg.sh`,
`.github/workflows/{build,ci,release}.yml` and `README.md`:

- **A staged `cmake --install` plus an explicit `tar`, and no CPack.** The per-leg archive name
  is not what decides it — `CPACK_PACKAGE_FILE_NAME` handles that. It is the payload's
  `BUILD-INFO.txt`, whose content comes from the CI *environment*: the commit, the runner's OS
  and architecture, and the leg's list of runtime packages. Generating that under CPack means
  `CPACK_PRE_BUILD_SCRIPTS` or install rules reading environment variables, which is the point
  at which the tool is being worked around rather than used. One mechanism, not two.
- **Every install rule names one component, and the payload is asked for by name:**
  `cmake --install build --component sendspin-cli`. That is load-bearing rather than tidy. The
  dependencies fetched at configure time declare install rules of their own, and a bare
  `cmake --install` into an empty prefix stages **149 files on Linux** where the component stages
  six: the other 143 are every ArduinoJson header and its three CMake export files.
  `IXWEBSOCKET_INSTALL` is now `OFF` beside the existing `INSTALL_GTEST OFF`, since otherwise
  IXWebSocket adds its archive, its headers, a `.pc` and an `install(EXPORT)` as well; but
  ArduinoJson declares its rules with no option at all, and patching a fetched subproject to get
  a clean tarball is not a trade worth making. Opus declares unguarded rules of its own and
  stays out only because micro-opus compiles its sources rather than adding that project — a
  fact a payload should not have to rest on.
  Naming a component is CMake's own answer to "which of these rules are mine", and unlike a
  list of upstream options it stays true across a `SENDSPIN_GIT_TAG` bump that adds a
  dependency.
- **The install set is deliberately six files:** `bin/sendspin-cli`,
  `lib/systemd/system/sendspin-cli.service` and `lib/sysusers.d/sendspin-cli.conf` on Linux
  only, and `share/doc/sendspin-cli/`
  holding `README.md`, `LICENSE` and `sendspin-cli.conf.example`. The count is a claim that
  every member is argued for rather than a cap — the sysusers fragment is the unit's `User=`
  made installable, and belongs to the hardening slice below — and CI's payload diff is what
  stops a seventh arriving unnoticed. `sendspin-cli-core` is absent
  on purpose — it exists so the tests can link the parser without a process entry point, and
  nothing outside this build wants a static archive of it. The doc directory is named after the
  binary rather than taken from `CMAKE_INSTALL_DOCDIR`, which is built from the *project* name:
  everything anyone types is `sendspin-cli`, and `share/doc/sendspin-cpp-cli` would have been
  the single place the other spelling surfaced. Nothing is written to `/etc`: the daemon
  *reads* `/etc/sendspin-cli.conf`, so installing one would overwrite an operator's file on the
  next upgrade, and the annotated example goes beside the README instead.
- **The whole install section is top-level-only**, on the same test the test suite already used —
  hoisted into one `SENDSPIN_CLI_IS_TOP_LEVEL` rather than written twice. A parent that vendors
  this project through `FetchContent` has no use for our binary, our README, or a unit whose
  `ExecStart` names *their* prefix; leaving the rules unconditional would have been the exact
  complaint this item makes of ArduinoJson, made of somebody else.
- **The unit is generated rather than copied**, because `ExecStart` is an absolute path:
  `configure_file()` fills in `CMAKE_INSTALL_FULL_BINDIR`, so a unit installed from one prefix
  cannot start a binary from another. The consequence is worth stating rather than discovering —
  the prefix is fixed at **configure** time, so `cmake --install --prefix` relocates the files
  around a unit that still names the old path. That is why the payload is staged with `DESTDIR`
  below, and why `README.md` says to reconfigure rather than redirect.
- **`lib/systemd/system`, not `${CMAKE_INSTALL_LIBDIR}/systemd/system`,** with the reason
  written at the `install()` call so it survives a tidy-up: a unit file is
  architecture-independent and systemd reads `<prefix>/lib/systemd/system`, never Debian's
  `lib/aarch64-linux-gnu/…` or Fedora's `lib64/…`. `/usr/local/lib/systemd/system` is on that
  search path alongside `/usr/lib/systemd/system`, which is what lets the default prefix work
  with nothing copied by hand and no `pkg-config --variable=systemdsystemunitdir`.
- **`Type=simple`, which is the shape `README.md` already documented as primary.** journald
  captures stderr, so `-z` buys nothing and `-f` would put the log somewhere `logrotate` has to
  be told about — and item 6's timestamps are `-f`-only for exactly this reason. `Type=notify`
  is not available: `sd_notify` is not wired up, and this item deliberately did not wire it.
  `Type=forking` with `PIDFile=` pointing at `-P` stays documented as the alternative for a
  supervisor with no journal, rather than shipped as a second unit.
- **`RuntimeDirectory=` and `StateDirectory=` paired with `--control-socket` and `--state-dir`,**
  written as `%t/sendspin-cli/control.sock` and `%S/sendspin-cli` so each flag names the
  directory the unit itself creates. Both flags exist for this case and items 7 and 8 said so:
  unpaired, the socket is skipped with one warning because a system unit has no
  `$XDG_RUNTIME_DIR`, and the volume, mute and static delay are forgotten on every restart
  because it has no `$XDG_STATE_HOME`. Item 7's private-directory check does not apply here —
  it guards the *derived* default path, and an explicit `--control-socket` goes to `bind()` — so
  systemd's own `0755` runtime directory is not warned about, and the `0600` socket inode inside
  it is what Linux enforces on `connect()`.
- **`After=` twice, `Wants=` never.** `network.target` rather than `network-online.target`,
  because the player retries its advertisement and an `-s` dial on a backoff, so it comes up
  fine ahead of the network while `network-online.target` would delay every boot to buy nothing.
  `avahi-daemon.service` for ordering only: a failed advertisement warns and retries rather than
  being fatal, so a host whose operator turned Avahi off should stay that way instead of having
  this unit pull it back in.
- **`Restart=on-failure` with `RestartSec=5`, and it never gives up — checked, not assumed.**
  A clean exit here means SIGTERM, which is `systemctl stop` or a shutdown, and neither wants
  the player back; a device that is not ready yet exits 1, which is what the delay covers. A
  config file that does not parse also exits 1, and one restart every 5 s never trips systemd's
  default start limit of five starts in ten seconds, so the unit retries indefinitely with the
  parse error naming its file and line in the journal each time. That is the wanted end of it:
  an operator who fixes the file gets a player back without also having to `reset-failed` a unit
  that had given up.
- **It runs as `sendspin-cli` with a hardening block, and neither shipped without the other.**
  The account is declared in `lib/sysusers.d/sendspin-cli.conf` beside the unit, carrying two
  lines that are owed together — the user, and its membership of `audio` — because a uid with
  no `audio` is a player that starts and cannot open `/dev/snd` (`root:audio` mode `0660`).
  That is also what rules out `DynamicUser=`, which hands the player a uid in no supplementary
  group at all. A tarball still has no `postinst`, so `systemd-sysusers` is a command
  `README.md` and `BUILD-INFO.txt` both tell an operator to run once; skipping it is not a
  quiet degradation but `217/USER` in `systemctl status`. Nothing has to be done to an existing
  `/var/lib/sendspin-cli` left root-owned by the version before this one: `StateDirectory=`
  chowns a directory it finds as well as one it creates, recursively, documented since systemd
  235 and checked against 255 — the state a root-run player wrote is read back by the new
  account. The whole of it, the hardening block included, is exercised on every Linux CI leg
  rather than reasoned about: the unit starts as the account, the control socket answers
  `status`, a planted root-owned state file survives the ownership change, `delay` lands 0600,
  it comes back from `SIGKILL`, and `systemctl stop` leaves `Result=success`.
- **The hardening block is what was tried, not what a list suggests.** `ProtectSystem=strict`,
  `ProtectHome=`, `PrivateTmp=`, `NoNewPrivileges=`, an empty `CapabilityBoundingSet=`,
  `RestrictSUIDSGID=`, the `Protect*=` kernel family, `ProtectProc=invisible`,
  `RestrictNamespaces=`, `LockPersonality=`, `MemoryDenyWriteExecute=`,
  `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6`, `SystemCallArchitectures=native` and
  `SystemCallFilter=@system-service`. Two of those needed more than "it booted". `AF_NETLINK`
  is left out because glibc's interface probe falls back when it cannot open one, which was
  settled by running browse, resolve, the A-record query behind a `ws://` URL and a dial by
  hostname that really connected — all under the restriction. And `@system-service` covers
  every syscall `libasound` imports, `ioctl`, `mmap`, `mlock` and the SysV IPC calls `dmix`
  uses included, read off the shipped library's own import table rather than assumed, which is
  what keeps the audio path from being the thing that directive is gambling on.
  One existing configuration stops working, and it is the whole of what an upgrade breaks:
  `ProtectSystem=strict` refuses a `logfile` or `pidfile` in `/etc/sendspin-cli.conf` that
  points outside the unit's own two directories. It fails the way this project prefers — the
  error names the path and the reason on every retry, rather than a player logging into
  nowhere — and `ReadWritePaths=` in a drop-in is the documented way back. Neither key was ever
  the shape for this unit, `Type=simple` having handed stderr to journald already.
  The block also moves the unit's *effective* floor, which is worth stating rather than
  discovering: `ProtectProc=` is from 247 and four more land between 242 and 245, against the
  236 the unit needs to start at all. Below 247 systemd logs `Unknown key name … ignoring` and
  runs it anyway — checked on 245 — so the degradation is one directive and a warning rather
  than a unit that will not load, and 247 is Debian 11.
  `systemd-analyze security` scores the result 1.8 against the root unit's 9.6 on systemd 255;
  CI prints it rather than asserting it, since a number is evidence and not a target, and the
  one remaining `✗` is `UMask=`, which buys nothing here — the player creates its socket and
  its state file `0600` itself.
- **Four directives are deliberately absent, and that is the item's own rule applied to
  itself.** `PrivateDevices=`, `DeviceAllow=`, `ProcSubset=pid` and `RestrictRealtime=` each
  gate what the ALSA backend reaches — `/dev/snd`, `/proc/asound`, `SCHED_FIFO` — and every one
  of them *passes* the checks above under `-o null`, which is exactly why passing proves
  nothing about them. An untested hardening directive is worse than none, so they wait for a
  run against a real card; see the open work below.
- **CI stages the payload with `DESTDIR`, not `--prefix`,** so every path under the archive's
  `usr/` is the path the file installs to and the whole thing goes in with
  `sudo tar -xzf … --strip-components=1 -C / <name>/usr`. The member is named rather than
  omitted, or `--strip-components` would deposit `BUILD-INFO.txt` at the filesystem root. Two
  other things fall out of `DESTDIR`: the unit's absolute `ExecStart` names a path the archive
  actually contains, which `--prefix` would have left dangling, and the `.pkg` task inherits the
  payload root `pkgbuild --root` wants. `BUILD-INFO.txt` sits at the archive root and tells an
  operator how to install it, how to run it in place, and what the unit does.
- **Assertions rather than claims**, in the shape `ci.yml` already uses for the configure output.
  Every publishing leg diffs the staged payload against the file list it is supposed to be, then
  diffs the *tarball's* own listing against the same list and checks the staged binary is
  executable — the archive being one `tar` away from the directory and the thing anybody actually
  downloads. That catches a dependency which starts declaring install rules on a tag bump and a
  file quietly dropped from our own. The Linux legs then install at the real prefix and run
  `systemd-analyze verify` on the installed unit, which resolves `ExecStart` and so needs the
  binary really there — exercising the install rules at the prefix the unit names, which the
  staged payload does not.
- **That verify step runs last, and the ordering is load-bearing.** A component install writes
  `install_manifest_sendspin-cli.txt` into the *build* directory, so a `sudo` install before the
  staging one leaves a root-owned manifest and the unprivileged `DESTDIR` install afterwards
  fails on `EACCES` — a red leg, out of a file nobody thinks about. Observed under Ubuntu
  24.04's own cmake 3.28.3, which rewrites the manifest unconditionally; cmake 4.4.1 skips a
  write whose content has not changed and gets away with it, so the break is version-dependent
  and putting the root install last is what stops it being anybody's problem. Unprivileged
  first, root over the top; `README.md` says the same to anyone doing both in one build tree,
  and `.gitignore` now covers `install_manifest*.txt` rather than only the un-suffixed name.

**The release workflow**, the second slice, in `.github/workflows/release.yml` and the
`build.yml` it shares with CI:

- **The matrix moved into `build.yml`, a reusable workflow both paths call.** A composite
  action was the other option and is the wrong one *here* specifically: what must not be
  duplicated is the matrix, not the steps. A composite action cannot own `strategy.matrix`, so
  `release.yml` would have had to restate the per-leg `publish` and `avahi` keys that the
  comment above them warns a new leg owes both of — the exact block whose duplication is
  dangerous. It takes no inputs, deliberately: a release is gated on precisely what a push is
  gated on, the non-publishing no-mDNS leg included, since a break in `src/mdns_null.cpp` is a
  real break and an input is the seam along which the two would start to differ.
- **A called workflow inherits none of the caller's `defaults`, `env` or `permissions`,** so
  `build.yml` restates all three. The one that would have bitten is `shell: bash`: left behind
  in `ci.yml`, the configure step runs under the default `bash -e` with no pipefail, its `|
  tee` returns 0 on a failed configure, and the assertion after it greps a truncated log —
  both callers green on a build that never configured. A fail-open gate is worse than no gate,
  which is the argument the shellcheck job at the top of `ci.yml` already makes.
- **`ci.yml` gained a ref filter on its `push`, and that is the whole of its *behavioural*
  change** — the rest of its diff is the matrix moving out to `build.yml`. Its `push` was
  unfiltered on purpose, and a `v*` push would otherwise have run the matrix twice over and
  reported two statuses for one tag — the `concurrency` group is keyed by workflow name, so
  nothing collapses the pair. The filter is `branches: ['**']`, which is what excludes tags: a
  `push` filtered on one kind of ref does not fire for the other kind at all. The inverse
  spelling, `tags-ignore: ['**']`, reads like the same sentence but is that rule the other way
  around, and stops branch pushes firing at all.
- **Fail closed, in two places, because `needs:` only covers one of them.** `needs: build`
  catches a leg that went red. What it does not catch is a leg that went green *while
  publishing nothing* — `Upload` is `if: matrix.publish`, and `if-no-files-found: error`
  catches an empty upload rather than an absent one, while `download-artifact` matching nothing
  is an empty directory rather than an error. So the release job names the assets it expects and
  diffs the set, in the shape the payload assertions already use — four tarballs and, since the
  third slice below, the macOS `.pkg`. Only the set: each
  archive's own file list was diffed twice inside the build, and re-opening them here would be
  the duplication this split exists to avoid.
- **Created as a draft, published only once the API says every asset arrived.** `gh release
  create` does draft-upload-publish internally already, but it publishes on "no upload returned
  an error" rather than on "every asset is present", and that is an implementation detail of
  `gh` rather than a promise. Splitting it makes the check ours: the asset names are read back
  with `gh release view --json assets` and diffed before `gh release edit --draft=false`. A
  partial upload therefore leaves an invisible draft, never a published half-release. A draft
  left by a failed run makes the next attempt fail rather than overwrite — `--clobber` on a
  step that may be facing an already-published release is the opposite of the point.
- **`--verify-tag`, which is what makes "the workflow never creates tags" a gate rather than a
  claim:** without it `gh release create` creates a missing tag from the default branch. The
  human tags; this publishes what that tag builds.
- **The version is checked, not derived.** `project(VERSION)` stays a hand-edited line bumped
  in a PR before the tag is pushed. Deriving it from `git describe` would tie the version to
  the presence of a `.git` directory — breaking a build from a source tarball — and put the tag
  into `CMakeLists.txt`'s hash, which is the deps cache key. A preflight job instead refuses a
  tag that is not `vMAJOR.MINOR.PATCH` (the `v*` trigger also matches `v0.2.0-rc1`, which would
  become `latest` with nobody having decided that) and refuses a tag that disagrees with
  `CMakeLists.txt`, counting the matched `VERSION` lines rather than taking the first: no match
  yields an empty string that would fail only by luck, two would silently pick one. It gates
  the matrix, so a mistyped tag costs seconds rather than three runners for half an hour.
  It duplicates part of the archive-name check that follows, and earns it the same way the
  tarball listing does: the later check is authoritative because it reads names the *binary*
  produced, and this one is an early mirror of the likeliest human error.
- **No third-party action, and no checkout, in the job that can write to the repository.**
  `permissions: contents: write` is scoped to the release job alone with the workflow default
  left at `read`. Actions in this repo are pinned to a SHA because a tag can be repointed and
  they run with write access to the *workspace*; this job runs with write access to the
  *repository*, so it is the worst place in the tree to add one. `gh` ships on the runner,
  `GH_REPO` replaces the checkout since nothing here reads the tree, and the only pinned action
  left is `download-artifact`. "Pin exact tool versions" cannot honestly be done for a
  runner-provided `gh`; what is pinned is the image label — `ubuntu-24.04`, never
  `ubuntu-latest` — and `gh --version` is printed into the log so a future break is
  diagnosable, in the same print-then-assert habit as the rest of the matrix.
- **`SHA256SUMS` is generated from inside the directory** so its entries are bare filenames:
  `sha256sum -c` resolves paths relative to the working directory, and a file naming `dist/…`
  is one a downloader cannot use without knowing that. The notes give the macOS spelling
  (`shasum -a 256 -c`) beside the Linux one, two of the five assets being macOS-only, and
  say out loud that the checksums do not cover the `Source code` archives GitHub attaches on
  its own. Both are given with `--ignore-missing`, which is not a detail: `SHA256SUMS` lists
  all five and a reader has almost certainly taken one, so the bare form reports the
  rest as failures and exits non-zero on a file that is perfectly good. Supported by GNU
  coreutils and by macOS's Perl `shasum` alike, both checked rather than assumed.
- **The notes are written by hand, not `--generate-notes`.** On a first tag that emits every
  merged PR since the initial commit as a flat list, there is no `.github/release.yml` taxonomy
  to shape it, and the narrative of what shipped already exists here and is better. They state
  plainly that the macOS binary is ad-hoc signed and what Gatekeeper will do about it — the
  third rendering of that paragraph after `README.md` and `BUILD-INFO.txt`, kept short with the
  README linked for the full account, because a reader deciding whether to download is a third
  audience and the alternative is presenting an unsigned binary as something else. Links are
  pinned to the tag rather than relative: a relative link in a release body resolves against
  the default branch, where the text it points at is free to move after the release is cut.
- **No `concurrency` block, where `ci.yml` has one.** `cancel-in-progress: true` is free to
  cancel a run midway through uploading assets, which is the exact partial state the draft
  gate above exists to prevent. A tag is pushed once; there is no superseded run to collapse.

**The macOS installer `.pkg`**, the third slice, in `scripts/build_macos_pkg.sh`, the
`build.yml` both callers share, and `release.yml`:

- **The macOS `.pkg` wraps that payload rather than restating it**, in
  `scripts/build_macos_pkg.sh`. The staged `DESTDIR` tree is the input, so the installer ships
  exactly the files the `install()` rules do and cannot drift from the tarball published beside
  it; nothing is compiled, fetched or installed, and given a staged tree it runs offline in a
  second. A script and not lines in `build.yml` for two reasons: an installer has to be
  buildable and *installable* by a developer with no runner, and `scripts/*.sh` is already
  gated by `ci.yml`'s `shellcheck` job, so the file arrives linted with no workflow change.
- **It lives in `build.yml`, the shared build, not in `ci.yml`.** That is the whole of the
  re-homing this slice owes the second one, and it falls out of what the reusable workflow is
  for: `build.yml` takes no inputs precisely so a release is gated on what a push is gated on,
  and an installer that only a push produced would be the seam that claim warns about. One
  definition therefore builds it, installs it and asserts it on both paths, and `release.yml`
  attaches what that build hands it.
- **`pkgbuild --root <payload>/usr/local --install-location /usr/local`,** not a root one level
  up paired with `/usr`. The staged tree carries `BUILD-INFO.txt` beside `usr/` for the
  tarball's readers, and the wider root would install that file at `/` — excluded structurally,
  by where the root points, rather than by a filter a later tidy-up can drop. The tighter pair
  also declares the one directory this package writes, instead of nominally claiming a
  SIP-protected `/usr`. `--ownership recommended` is pkgbuild's default and is named anyway,
  because it is what makes the installed files `root:wheel`: the staged payload is owned by
  whoever ran `cmake --install`, and `preserve` would file a build user's uid into the receipt.
  The destination is `/usr/local` and not `/opt` because it is not a free choice — the prefix is
  baked in at configure time, and the install rules above already chose it.
- **`productbuild` over the component package, for two things a component package cannot do.**
  It has no architecture gate, so an arm64-only installer reports success on an Intel Mac and
  leaves the user a binary that answers `Bad CPU type in executable`; `<options
  hostArchitectures>` is the declarative gate, and the value is read off the binary with
  `lipo -archs` rather than passed in — so a universal build widens the installer by itself and
  a per-arch one stays refused where it could only fail. And distribution panes are a
  product-archive feature, so a component package has nowhere to say the installer is unsigned
  at the moment somebody is deciding whether to run it. Two independent reasons is what earns
  the extra layer. The distribution XML is generated rather than checked into `packaging/`,
  because three of its fields — version, the component's filename, the architectures — are
  computed by the script, and a template would be a second place to look for values it already
  holds. `<product>` carries that same identifier and version rather than a second one nobody
  types, which is what makes Installer refuse to put this over an install that is already newer.
  No `<license>` pane: `LICENSE` ships inside the payload either way, so the only difference
  would be a click-through Agree gate the tarball does not have.
- **The version is an argument, and an empty one is refused.** It is read off the binary once,
  in `build.yml`'s `Package` step, and carried to the installer step through `$GITHUB_OUTPUT`
  rather than derived a second time — the same "one mechanism, not two" the CPack decision
  above turns on.
  `build_macos_pkg.sh` rejects an empty version rather than letting `pkgbuild` file a receipt
  with nothing in it, because the assertion downstream compares the receipt against the value
  that was passed: without the guard, a dropped output would leave both sides equal and empty
  and the check would pass on an installer that had lost its version.
- **The receipt is `io.github.chrisuthe.sendspin-cli`.** A namespace the author holds, rather
  than an `io.sendspin.*` or Open Home Foundation one that was not this project's to take when
  the identifier was minted — the same manners the component argument above makes of
  ArduinoJson. `sendspin-cli` and not `sendspin-cpp-cli` for the reason the doc directory is
  named that way: everything anyone types is the binary's name, and this string is typed, by an
  operator running `pkgutil --forget`. The repository has since moved under the Sendspin org,
  which makes `io.sendspin.*` available to it for the first time, and the identifier
  deliberately does not follow: changing it orphans the receipt of every install before the
  change rather than upgrading it.
- **The `.pkg` is shipped honestly, and the installer says so itself.** It is unsigned and
  unnotarized; `spctl -a -t install` rejects it exactly as `spctl -a -t exec` rejects the
  ad-hoc-signed binary inside. The welcome pane carries that, what gets installed, and how to
  undo it. `README.md` separates the four ways a reader can come by the file, because they
  genuinely differ: taken from a release page the `.pkg` *is* the download, so it is quarantined
  and Gatekeeper refuses it outright; taken from the Actions tab it arrives inside a *zip*, so
  the flag lands on the zip and the `.pkg` inherits it only from Finder's Archive Utility and
  not from `unzip` — the same distinction the tarball paragraph above it already draws; a
  locally built one is never quarantined at all; and `installer -pkg` makes no Gatekeeper
  assessment whatever the file is flagged with, which is what lets CI install its own artifact.
  The release notes say the same in short, which is where a reader deciding whether to download
  actually is. What the `.pkg` *does* fix is narrow and is named as such: `installer` does not
  quarantine what it writes, so the installed binary needs no `xattr -d` where a Finder-unpacked
  tarball's does. That is convenience, not identity.
- **The build installs the `.pkg` at `/` and checks what came out**, on a push and on a tag
  alike, because item 10's first slice named this the untested half of the payload and an
  installer nobody installed is not shipped. The
  receipt's own file list is diffed against the four paths it should hold, its `version:` and
  `location:` are asserted so the version threaded through rather than defaulted, and then the
  installed binary answers `--version` and runs the whole of `scripts/smoke_test.sh` — the same
  suite the build tree's binary runs, pointed at `/usr/local/bin/sendspin-cli` instead. The two
  runs are sequential rather than parallel: the suite binds fixed ports 39281-39288.
  `pkgutil --files` prints paths relative to the receipt's install location, so that expected
  list is `bin/sendspin-cli` and friends rather than a copy of the tarball's absolute one.
  `._`-prefixed siblings are filtered even though that listing turns out not to need it, and the
  asymmetry is worth recording: macOS stamps every payload file with a `com.apple.provenance`
  extended attribute that cannot be removed — `xattr -c` is undone before the next `stat` — and
  `pkgbuild` carries extended attributes as AppleDouble members, so the `.pkg`'s **own** BOM does
  list them, as `lsbom` over the expanded archive shows. The receipt's does not, because
  `installer` folds those members back into attributes rather than writing files. Filtering rather
  than naming them is what keeps the assertion right in both directions.
- **The installer is a second artifact, not a second path on the tarball's upload.**
  `if-no-files-found: error` is wanted on both, and a `.pkg` path on the existing step would
  fail every Linux leg; `upload-artifact` also refuses two artifacts under one name, so the
  macOS leg publishes `…-macos-arm64` and `…-macos-arm64-installer`.
- **`release.yml` names it in both asset sets, which is the third thing a new build output
  owes.** The matrix comment in `build.yml` says so already: the release job diffs what
  `download-artifact` handed it against a set spelled out by hand, and diffs the draft's assets
  read back from the API against the same set before publishing. An output nobody added there
  fails a release with a diff rather than a reason, and this one is not per-leg — three
  tarballs and one `.pkg` — so the sets are no longer a loop over the legs. `SHA256SUMS` covers
  it too: a release asset a downloader cannot verify is worse than one that is not there, and
  the notes' `--ignore-missing` advice was already the reason a reader can check just the one
  file they took.

**Not in this slice**, each with its owner named rather than implied:

- **Developer ID signing and notarization** → a task of their own, **gated on an enrolment that
  has not happened**, and none of the analysis has changed — only the artifact it applies to now
  exists. Everything shipped is ad-hoc signed: `codesign` reports `adhoc, linker-signed`, the
  minimum an arm64 Mach-O needs to execute at all and no identity, so `spctl -a -t exec` rejects
  the binary and `spctl -a -t install` rejects the `.pkg` around it. Clearing that needs a
  Developer ID Application certificate and a Developer ID *Installer* certificate — a paid
  enrolment — and notarization; `README.md` still answers with `xattr -d com.apple.quarantine`
  and the Privacy & Security override, which are workarounds rather than fixes. The part that
  decided the `.pkg` belongs here rather than there, and is now discharged: `xcrun stapler`
  takes `.app`, `.dmg` and `.pkg` and **refuses a bare Mach-O**, so notarizing the loose binary
  alone would still leave Gatekeeper asking Apple on first run — no good for an offline Pi or
  Mac. The `.pkg` this slice produces is the artifact that ticket will staple to. Nothing was
  stubbed for it: `scripts/build_macos_pkg.sh` grows a `--sign` on its two calls, or a
  `productsign` over the finished archive, and no placeholder branch waits in the tree for it.
  A public repo also gets no secrets on a pull request from a fork, so that step has to be
  conditional rather than assumed — which is why it is not written until there is an identity
  to write it against.
- **A minimum-macOS gate on the `.pkg`** → owed with the signing task, **no task open for it
  yet**. The architectures are derived from the binary and declared, and the deployment target
  is the asymmetry: nothing sets `CMAKE_OSX_DEPLOYMENT_TARGET`, so the binary's `minos` is
  whatever the runner's SDK defaulted to, and `<options>` carries no OS floor to match the
  `hostArchitectures` one. Same class of failure as the arch gate — an install that reports
  success and then cannot run — and the same declarative remedy, but it wants the deployment
  target pinned first, which is a build decision rather than a packaging one.
- **The four `/dev/snd`-gating directives**, with **no task open for them yet**: `PrivateDevices=`,
  `DeviceAllow=`, `ProcSubset=pid` and `RestrictRealtime=`, all named in the unit where a reader
  meets them. What is missing is not analysis but a machine — every one passes CI under `-o null`
  and would keep passing while deafening a real card, so the check that settles them is a player
  on hardware with a device configured, listened to. Worth doing as one sitting: the four share a
  fixture, and `DeviceAllow=char-alsa rw` with `DevicePolicy=closed` is what the first three are
  really reaching for.
- **A drift guard on the version strings in `README.md`,** with **no task open for it yet**, and
  worth more now than when it was first named. Several places spell an artifact name out in
  full — `sendspin-cli-0.1.0-linux-x86_64`, its macOS twin, and the `.pkg` and
  `build_macos_pkg.sh` lines the third slice below added — and nothing checks any of them
  against `project(VERSION)`. They are correct for the first release and rot at 0.2.0. The
  release notes avoid the same trap by interpolating the version they were built from, and the
  same fix would suit here: generate the examples, or assert them. Named rather than left to be
  noticed, since the version is now a thing a tag agrees with.
- **A drift guard on `packaging/sendspin-cli.conf.example`.** Item 8's "one vocabulary" claim
  means every key in that file is a long flag name, and nothing enforces it — a renamed flag
  would leave a shipped example that stops a player with an unknown-key error. It was checked by
  hand this time (see below). The cheap fix is a test that uncomments the file and feeds it
  through `config_file`, and it needs the source path handing to the suite.
- **`.deb`, Homebrew and AUR** are not planned at all. The sanctioned set stays the tarball
  payload, the unit, item 9's Docker image and this item's macOS `.pkg`.
- **Docker** is item 9's, and is unaffected: it builds the binary rather than consuming a
  payload.

**What has and has not been exercised.** On macOS, against this branch's own build: configure
and build clean under `-DSENDSPIN_CLI_WERROR=ON`, 346 of 347 tests passing — the exception is
`ConfigMerge.AConfiguredControlSocketIsAbsolutizedUnderDaemonize`, which fails only because this
worktree's path is long enough that the absolutized socket path exceeds `sockaddr_un`'s 104 bytes,
and it passes from a shorter directory — `scripts/smoke_test.sh` green, and the payload staged three
ways and read back: `--component sendspin-cli` giving four files with no unit (correct: a systemd
unit on a Mac is a file that can never run), `DESTDIR` giving the same four under `usr/local/`,
and a bare `cmake --install` giving 147 — the 143 extra files being the number the component
exists for. CI's own packaging step and its payload assertion were run against that build too,
including with a stray file planted to check the assertion fails rather than merely reports.

**The unit itself was proven on Linux, in a container, rather than reasoned about.** On
`ubuntu:24.04` (aarch64, systemd 255) the tree configures and builds `-Werror`-clean with the
tests off — CI is what builds and runs those there —
`cmake --install --component sendspin-cli` puts the unit at
`/usr/local/lib/systemd/system/sendspin-cli.service`, and `systemd-analyze verify` accepts it
with nothing to say — including with no `avahi-daemon.service` on the host, which is what the
`After=` line names. Under a real `systemd` as PID 1, with the `DESTDIR` payload untarred into
`/` exactly as `BUILD-INFO.txt` says: `systemctl enable --now` starts it, the journal carries the
startup lines with no `-f` and no timestamps of ours, `RuntimeDirectory=` yields
`/run/sendspin-cli/control.sock` as a `srw-------` socket beside its `.lock`, `StateDirectory=`
yields `/var/lib/sendspin-cli`, `sendspin-cli delay 250` round-trips over that socket and lands
in a `0600` state file, a `systemctl restart` comes back reporting the remembered 250 ms, and
`systemctl stop` leaves `Result=success`, `ExecMainStatus=0` and `NRestarts=0` — with systemd
removing the whole runtime directory, so the unit never meets the stale-socket case at all. A
deliberately broken `/etc/sendspin-cli.conf` was left in place for a minute and reached
`NRestarts=18`, still `activating`, which is where the never-gives-up note above comes from. The
shipped example config was checked the same way, every commented line uncommented: all eight keys
and values are accepted, the run gets as far as the sound card the container does not have.

**The hardening slice was proven the same way, one directive at a time.** On the same
`ubuntu:24.04` (aarch64, systemd 255) with `avahi-daemon` running, every candidate was applied on
its own and put through the whole round — unit active, control socket answering `status`,
WebSocket port accepting a connection, the `_sendspin._tcp` advertisement reaching Avahi, `delay`
landing in a `0600` state file, that value surviving a restart, a `SIGKILL` recovered from, and
`systemctl stop` leaving `Result=success` — then the shipped block was put through it whole, and
then the real installed unit with no drop-in at all. `systemd-sysusers` creating the account from
`/usr/local/lib/sysusers.d` was run from a host that had no such user, `217/USER` was seen before
it and the account with its `audio` membership after, and a root-owned `/var/lib/sendspin-cli`
holding a `175 ms` delay was planted and read back through the new uid. `PrivateDevices=` and
`ProcSubset=pid` were run too, and passed — which is the evidence for excluding them rather than
against it.

**One thing that was not exercised and is worth knowing:** no audio came out of a unit. The
container has no `/dev/snd`, so `Type=simple` under systemd is proven to *start*, log, serve its
socket and stop cleanly, but the ALSA path under it is inference from a foreground run — which is
also why the four device-gating directives above are named rather than shipped.

**A third, for the release slice: `release.yml` has never run.** Everything asserted about it
above is reasoning plus a local dry run, not a release. What *was* exercised, on macOS against
this branch: `actionlint` over all three workflows with `shellcheck` present, so the `run:`
blocks are linted; the preflight's tag and version checks driven through their accepting and
every rejecting case; the completeness assertion driven through a skipped leg, an empty
download, a version mismatch and an unexpected extra file; the publish gate driven through a
complete asset set and two incomplete ones against a stubbed `gh`; and the whole version chain
end to end — `CMakeLists.txt` 0.1.0, `--version` 0.1.0, and a real `cmake --install` payload
tarred as `sendspin-cli-0.1.0-macos-arm64.tar.gz`, which is the name the release job expects.
What that leaves unproven is everything only GitHub can answer: that `gh release view` finds a
*draft* by tag name (the REST `releases/tags/{tag}` endpoint does not return drafts, so this
rests on `gh`'s list-and-match fallback), that `download-artifact`'s `merge-multiple` lays the
files out flat as read here, and that the release job's narrowed `permissions` still let it
reach the artifact service. A throwaway tag on a fork is what would settle all three, and is
worth doing before `v0.1.0` rather than after.

**The `.pkg` was closed out the same way, and it is no longer the untested half of the payload.**
Locally on macOS 26.6 (arm64), against this branch's own build: the payload staged with `DESTDIR`,
`scripts/build_macos_pkg.sh` writing an installer whose BOM holds exactly the four expected files
— `com.apple.provenance` proving unstrippable on the way, which is where the `._` filter and its
comment come from — whose `Distribution` carries `hostArchitectures="arm64"` read off the binary,
and whose welcome pane reads back as written, its install list read out of the payload rather
than kept by hand. `shellcheck` and `actionlint` clean. The guard
paths were tried rather than assumed: a wrong argument count, an empty version and a payload root
with no binary each fail with the message that names the fix. The universal case was tried rather
than reasoned about too — a real fat binary (`clang -arch arm64 -arch x86_64`) staged as the
payload widens the declaration to `hostArchitectures="x86_64,arm64"` by itself, which is the
claim the `lipo` line is there to make. `spctl -a -t install` was run rather than predicted, and
rejects the archive with `no usable signature`, as `codesign -dv` reports `adhoc, linker-signed`
on the binary inside — the two sentences the honesty paragraphs rest on. The smoke suite passes
against a staged-payload binary outside the build tree, which is the shape the installed run
takes. What a developer's Mac cannot prove without `sudo` is the install itself, and that is
precisely what the shared build now does on every push **and every tag** —
`installer -pkg` at `/`, the receipt's file list, version and location asserted, then `--version`
and the full smoke suite against `/usr/local/bin/sendspin-cli`. The one thing an arm64 runner
cannot demonstrate is the *refusal* on an Intel Mac, so the architecture **declaration** is what
is asserted instead. That pass has happened, against these step bodies before the move that gave
them their present home: `installer` reports success, the receipt reads `location: usr/local` with
no leading slash and the version it was given, `pkgutil --files` lists the four paths relative to
that location, and the full smoke suite passes against `/usr/local/bin/sendspin-cli` rather than a
build-tree binary. It is also where the `._` comment above comes from — the receipt lists no
AppleDouble members even though the `.pkg`'s own BOM does, which `lsbom` over an expanded archive
confirms locally.

The `release.yml` half was driven the way the release slice drove its own, against the real `.pkg`
and a stubbed `gh`: the completeness assertion passes on the four-file set and fails on a macOS
leg that published only its tarball and on an unexpected extra file; the checksum step writes four
bare names and, with no `.pkg` staged, fails on the literal glob rather than quietly checksumming
three; `shasum -a 256 --ignore-missing -c` verifies a directory holding only the two macOS assets,
which is the case the notes tell a reader to expect; and the publish gate publishes on the full
asset set while refusing a draft whose `.pkg` is absent or whose name arrived sanitized. What stays
unproven is what the paragraph above already says only a real tag can answer.

### 11. Interactive TUI mode

Optional, later. Upstream's `examples/tui_client` shows the shape.

### 12. CI and tests — *shipped (matrix and smoke test; the sink contract still owed)*

`.github/workflows/ci.yml` builds and tests every branch push and pull request on
`ubuntu-24.04`, `ubuntu-24.04-arm` and `macos-14`, a fourth cross-compiled for ARMv7 on
`ubuntu-24.04`, and a fifth configured `-DSENDSPIN_CLI_WITH_MDNS=OFF` — which compiles
`src/mdns_null.cpp` instead of `src/mdns_dnssd.cpp`, so that translation unit is built rather
than assumed. Every leg configures `-DSENDSPIN_CLI_WERROR=ON` and runs the CTest suite, the
no-mDNS leg included:
`discovery_test.cpp` links whichever `MdnsService` went in, and that configuration has no
other coverage. Each leg also asserts against its own configure output that it found the
backends it expects — a missing `-dev` package does not fail a configure, since every
backend is optional and auto-detected, so without that assertion the matrix would go green
on a null-sink-only, mDNS-less binary. The four platform legs additionally run the smoke
test and upload the binary they built, kept 14 days.

The matrix itself lives in `.github/workflows/build.yml`, called by `ci.yml` and by item 10's
`release.yml` alike, so one definition answers for both paths; `ci.yml` filters its `push` to
`branches: ['**']`, which excludes tags, so a tag push builds once rather than twice. See item
10 for why that split is a reusable workflow rather than a composite action.

**The sixth archive is not in that matrix, and its absence is deliberate.** `linux-armv6` builds
natively inside an emulated Raspbian container, which pays for every compile in the FetchContent
tree under `qemu-user`: 23 minutes measured, against the two or three every leg above takes. As a
matrix leg it ran whenever the matrix ran — every branch push, and again on every pull request —
so every push waited half an hour on the one leg that almost never had anything new to say.

It is now `.github/workflows/build-armv6.yml`, one job carrying its own triggers: `workflow_call`,
so `release.yml` calls it beside `build.yml`; `push: branches: [main]`, so a merge is covered
within half an hour of landing; a `pull_request` filtered to that workflow,
`scripts/build_armv6_container.sh` and `CMakeLists.txt`; and `workflow_dispatch`. Deliberately no
unfiltered branch `push` — that is the cost being removed. The alternative considered and rejected
was leaving the leg in the matrix behind `continue-on-error`, which would still hold a runner for
25 minutes twice per pull request and would turn a red leg into one that gates nothing.

What that buys is a seam, and it is worth naming: **a tag now builds one thing a push does not.**
The `pull_request` path filter is honest about its own reach — it catches infrastructure changes,
not the ARMv6-only source breaks this project has actually met, which are the gcc 12 `-Wrestrict`
false positive and the `-latomic` class described below. Those are caught after the merge instead.
Two things bound the cost. `release.yml` needs both build jobs, so a red ARMv6 build fails the
release rather than publishing an incomplete one, and the release's expected asset set still names
`sendspin-cli-<version>-linux-armv6.tar.gz` — so an archive nobody built is a diff rather than a
quietly smaller release. And `push: branches: [main]` means a break can delay a tag but has at most
one merge in which to go unnoticed. Raising the ARMv6 leg's coverage back towards per-push without
paying for it per push — a nightly, or a merge-queue check — is a separate piece of work and is not
owed by anything today.

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

**The 32-bit Pi leg is there.** `linux-armv7` cross-compiles on `ubuntu-24.04` against the
armhf multiarch tree — `scripts/build_arm32.sh` owns the configure — and runs the whole unit
suite and `scripts/smoke_test.sh` under `qemu-user` rather than skipping them, so it holds the
same rule every other leg does. Nothing builds it natively: GitHub has no armv7 runner, and its
arm64 runners are Neoverse N1 with no AArch32 at EL0, so no `runs-on` value reaches the target
at all — and a cross build is what buys back everything the ARMv6 build below has to emulate.
What keeps the leg's name honest is a `readelf` of the linked binary: the linker merges build
attributes across every object in the link and reports the highest, so one read covers the
FetchContent tree as well as our own sources, and a dependency compiled for the wrong
architecture fails the leg instead of shipping.

**The ARMv6 archive is there as well, and it is not the same job.** A Pi Zero, a Pi Zero W and an
original Pi are ARM1176 cores, and Debian and Ubuntu armhf are an ARMv7-A port — which is where
a cross toolchain's own `crt1.o`, `crtbegin.o` and `libgcc.a` come from. Compiling our sources
`-march=armv6` therefore does not produce an ARMv6 archive: the startup and helper objects
linked in beside them are ARMv7, and the merged `Tag_CPU_arch` says so. What an ARMv6 build
needs is an ARMv6 `libgcc` and ARMv6 startup objects, which is a *distribution* rather than a
flag — and Raspbian is one. Its gcc is configured `--with-arch=armv6 --with-float=hard`, and
every one of `libgcc.a`'s members reads `Tag_CPU_arch: v6`, so the build passes no `-march` at
all.

So `linux-armv6` is not a cross build at all: it is an ordinary native build run inside a
digest-pinned Raspbian bookworm container under `qemu-user`, which is why it is the one archive
`scripts/build_arm32.sh` has nothing to do with. That script still refuses `armv6`, and its
reasoning is untouched — it cross-compiles against the *host* distribution's armhf tree, which
is still ARMv7. `scripts/get_started_linux.sh` now maps an `armv6l` host onto the new archive
instead of refusing it; ARMv5 and a bare `arm` are still refused, the ARMv6 archive being the
oldest one built.

Three things about that build are load-bearing and none of them are about ARMv6 the instruction
set. Everything it runs in the container runs under `QEMU_CPU=arm1176`, because `qemu-arm`
defaults to a Cortex-A15-class core and would happily execute the ARMv7 instructions the
hardware cannot — an emulator more permissive than the target proves nothing. That covers the
configure as well as the suite, a CMake `try_run` probe asking what the CPU can do being
answerable for the wrong CPU otherwise. The container runs `--init` and builds as a non-root
user, because without a reaping PID 1 the smoke test reads an exited daemon's zombie as a player
that outlived `SIGTERM`, and because three `StateStore` cases assert that an unwritable
directory is refused, which root is refused nothing by. And the link needs `-latomic`: ARMv6 has
no `LDREXD`, so the 64-bit atomics in `src/pulse_sink.cpp`, `src/player_listener.cpp` and
sendspin-cpp's own `connection_manager.cpp` become libatomic calls. It goes in
`CMAKE_CXX_STANDARD_LIBRARIES` rather than `CMAKE_EXE_LINKER_FLAGS`, which places it ahead of
the objects that need it and leaves the linker to discard it.

That leg holds the warning line like the rest, with one diagnostic exempted: Raspbian's gcc
12.2.0 has a `-Wrestrict` false positive on `line += " " + std::to_string(...)`, at
`src/control_common.cpp:391` and `:401` and at `tests/cli_test.cpp:1133`. It passes
`-Wno-error=restrict`, which is not interchangeable with `-Wno-restrict`: `CMAKE_CXX_FLAGS`
lands before the `-Wall` `target_compile_options` adds, and a later `-Wall` turns a *disabled*
warning back on — where a later blanket `-Werror` does not re-promote a diagnostic an earlier
`-Wno-error=` has already exempted. So an ARMv6-only warning nobody has met yet still fails the
leg.

One thing improves for free. The published archives need `GLIBC_2.38`, which Raspberry Pi OS
bookworm's 2.36 refuses at load; the ARMv6 binary references nothing newer than bookworm's own
2.36, so it loads on bookworm and trixie alike. That is asserted off the finished binary rather
than assumed, and its `BUILD-INFO.txt` names the version it actually came out at. The bookworm caveat the
ARMv7 archive's `BUILD-INFO.txt` carries therefore does not apply to this one, and it does not
say it. Raising the floor for `linux-x86_64`, `linux-arm64` and `linux-armv7` is a separate
piece of work and still owed.

There is no macOS x86_64 leg. Artifacts are per-commit workflow artifacts only; the tagged
release that does not expire is item 10's, and shipped. The hand-rolled tar this entry used to
describe is gone: item 10 shipped the `install()` rules, and the archive is now a
`DESTDIR`-staged `cmake --install` payload whose file list this workflow asserts — plus, on the
legs that run on the architecture they built for, a `systemd-analyze verify` of the unit it
installs.

### 13. `PlayerRoleConfig` wiring — *shipped*

Every `PlayerRoleConfig` field is now set deliberately, the static delay is observable and
locally settable, and volume changes are ramped. Split out of item 4.

**This item's opening premise was wrong, and correcting it is part of what shipped.** It claimed
`set_static_delay_adjustable(true)` was advertised with no `on_static_delay_changed()` override,
so "a controller can offer the user a static delay this player then applies to nothing". Against
sendspin-cpp v0.7.0 that is not true, and it never was: `SyncTask::decode_chunk()` subtracts
`get_effective_static_delay_ms()` from every chunk's client timestamp
(`_deps/sendspin-src/src/sync_task.cpp:593`), and that value becomes `decoded_timestamp`, which
is what `raw_error` is measured against — the drift correction itself. `get_effective_static_delay_ms()`
returns the stored delay precisely *because* adjustability is on. So the delay was already being
obeyed; the override was never the thing standing between the value and the audio path. A future
reader tempted to look for a playout shift in `PlayerListener` should stop here: there is none,
and there should not be.

What was genuinely missing was that nothing in this repo *observed or set* it. That is what
landed:

- **`PlayerListener::on_static_delay_changed()`** logs a server-set delay at INFO, and its doc
  comment says outright that both applying and persisting it are already the library's — so the
  override is observability and nothing else.
- **`status` reports it**, as `static delay: <n> ms`, read from `PlayerRole::get_static_delay_ms()`
  rather than from a listener-held shadow. That is not a style choice: `update_static_delay()` does
  not invoke the listener (only a server's `set_static_delay` does,
  `_deps/sendspin-src/src/player_role.cpp:396-400`), so a shadow would be stale the moment the
  local knob below was used.
- **`sendspin-cli delay <0-5000>`** sets it locally. The first *mutating* request answered without
  a server — `status` was previously the only locally answered one at all — which the spec
  explicitly provides for: the delay compensates for hardware past the audio port, the client is
  required to persist it, and "clients may update `static_delay_ms` ... when audio output changes".
  It is spec-clean because `PlayerRole::update_static_delay()` calls `publish_state()` itself, so
  the server still learns the value without a controller command carrying it.
- **`--static-delay <ms>` / `static-delay =`** seeds `initial_static_delay_ms`. A *first-run
  default only*, and documented as one everywhere it appears: `load_static_delay()` prefers a
  persisted value and reads the config field only when there is none
  (`player_role.cpp:565-590`), exactly as a restored volume beats `DEFAULT_SINK_VOLUME`.
- **The bound is this repo's own**, `MAX_STATIC_DELAY_MS` in `src/control.h`, and it tracks
  `Sendspin/spec` `roles/player/v1.md` rather than the library — the same relationship
  `q32_gain_for()`'s `^1.5` has to upstream's `^2`. The library's own limit is a file-local
  constant and `update_static_delay()` **silently clamps** to it, so `delay 5001` would have been
  reported as a success that applied 5000. It is refused at parse time instead.

**The volume ramp shipped**, closing the last of the two deviations item 7 left here. (The other,
`volume`/`muted` persistence, was item 8's; the `^2` taper was fixed in item 7 itself.) The
arithmetic is in `src/pcm_volume.{h,cpp}` — `volume_ramp_step()`, `ramped_gain()` and
`apply_volume_ramp()` — so it is shared by both scaling backends and testable with no device, the
same reason `apply_volume()` lives there. Four decisions in it are load-bearing:

- **A fixed slew rate, not a fixed duration.** `VOLUME_RAMP_MS` (20 ms) is what a *full-scale*
  change takes, and a smaller change is proportionally quicker. A constant-duration ramp would
  have to divide by the distance still to travel, which means reading the current gain from the
  main loop — and only the sinks' own audio paths may advance the current gain. The step is derived
  from the stream's rate alone, so each sink's setter writes only its atomic target and takes no
  lock. (ALSA's snap points do write the current gain from the main loop, under the `device_mutex_`
  that already serialises everything it touches; PortAudio has no such lock and so snaps only where
  its callback provably cannot run.)
- **Per frame, not per sample**, or the ramp would scale a frame's channels by different gains:
  an inter-channel amplitude skew, which is worse than the click it removes.
- **The advance is committed differently in each sink, and both are right.**
  `AlsaAudioSink::write()` commits by the frames it *wrote*, because its loop can break out early
  and the sync task re-presents the unconsumed tail — advancing by the frames scaled would leave a
  gain step across that seam. `PortAudioSink::pa_callback()` advances by every frame it scales,
  because it consumes every frame it scales, including the zero-filled tail of a short ring read:
  that silence is played, so it spends ramp time as legitimately as audio. Both sites carry the
  comment, so neither gets "fixed" to match the other.
- **A stream starts at its gain rather than fading into it.** ALSA snaps in `configure()` and
  `clear()`, both under `device_mutex_`. PortAudio snaps in `open_stream_()` and
  `restart_stream_()` — before `Pa_StartStream()`, which is the only place its ordering invariant
  allows the main loop to touch a field the callback reads — and deliberately *not* in `clear()`,
  where the callback keeps running and where a mid-stream flush is followed by more audio through
  the same stream, so a ramp in progress should carry on rather than jump. The audible result is
  the same either way: snapped before the next stream's first sample.

**`NullAudioSink` is excluded deliberately.** It records volume without applying it
(`src/null_sink.cpp`), so there is no sample scaling to ramp; its mute-to-silence path for the
stdout sink stays a jump. Not an oversight.

**The `DEFAULT_SINK_VOLUME` mismatch was already closed, so it was re-documented rather than
fixed.** Item 8 made startup call `player.update_volume()`/`update_muted()` unconditionally, with
the volume falling back to `DEFAULT_SINK_VOLUME`, before `start_server()` — so the role and the
sink agree from before the first `client/state`, and the disagreement four comments still
described was no longer observable. Those four (`src/audio_sink.h`, `src/player_listener.h`,
`src/control.h`, `src/main.cpp`'s `status()`) now describe the current arrangement, keeping the
reasons those members exist: `PlayerListener` is still the only thing that knows what the sink was
*told*, and `VolumeSource` still has no equivalent in the role.

**The two remaining `PlayerRoleConfig` fields, and why they are where they are:**

- **`fixed_delay_us` is now set explicitly to 0, and must stay there.** Both sinks already report
  *future* finish timestamps that include their own buffering — `snd_pcm_delay()` on ALSA,
  `outputBufferDacTime` on PortAudio — so folding device latency in here would count it twice. The
  value matches the library's default; writing it out puts the constraint beside the field instead
  of only in this document.
- **`extra_startup_silence_ms` stays at the library's 50 ms default.** Item 4's ALSA
  start-threshold fix has shipped, so the original blocker is gone — but choosing a different
  figure needs underflow measurements on real hardware across both backends, which is not
  something to guess at. A comment at the config site says so.

**One gap found while scoping this belongs upstream, not here.** `required_lead_time_ms` and
`min_buffer_ms` are **REQUIRED** in `client/state` per `roles/player/v1.md` — the server uses them
to decide how far ahead to send audio, scheduling the first chunk at least
`min_buffer_ms + static_delay_ms` out — and sendspin-cpp v0.7.0 implements neither anywhere. A
grep of the whole library, headers and sources, matches neither name. Nothing in this repo can
supply them: `ClientPlayerStateObject`, which `PlayerRole::Impl::build_state_fields()` fills, has
no field for either. It needs a library change, not a change here.

### 14. PortAudio in-place device recovery — *shipped (hardware pass still owed)*

`PortAudioSink` no longer latches into discarding for the rest of a track when its device goes
away: it gets the device back mid-stream, or exhausts a bounded set of attempts trying. ALSA
already recovered `-EPIPE`/`-ESTRPIPE` inside its own `write()`; this is the same idea for the
other backend. Split out of item 4.

**The gap was narrower and deeper than the item claimed.** Narrower, because `configure()`
already re-resolves the device at every stream, so only the *mid-stream* case was broken.
Deeper, because reopening in place is not enough on its own: **PortAudio enumerates devices at
`Pa_Initialize()` and never revisits that list**, and `resolve_pa_device()` walks that cached
list. A DAC pulled out and pushed back in usually comes back as a device the list has never
seen, with the old index left dead — so no amount of reopening reaches it, and a
`Pa_Terminate()`/`Pa_Initialize()` cycle is the only thing that can. That single fact is what
made this two tiers across two threads rather than one function.

**Tier 1 — one in-place reopen, inline in `write()`, on the sync task's thread.** Where `write()`
finds that a stream it *had* has stopped being driven, `reopen_in_place_()` re-resolves the device
and reopens at the remembered format before the discard path is reached; on success the same call
carries on filling the ring, so playback resumes without waiting for a track boundary. This is all
a host default-output switch needs. It is safe from that thread for a reason now written into the
header, because the class previously read as though only the main loop ever opened a stream:

- **The ordering invariant is about *when*, not which thread.** `Pa_AbortStream()`/
  `Pa_CloseStream()` do not return while the callback is running, and `open_stream_()` writes
  every callback-read field before `Pa_StartStream()` lets it run again. Neither depends on who
  is calling.
- **`mutex_` already serialises it.** `write()`, `configure()`, `clear()` and `stop()` all hold
  it, so no two `Pa_OpenStream()`/`Pa_CloseStream()` calls can overlap; the only other `Pa_*`
  callers — `probe()`, `list_devices()`, `capabilities()` — run on the main loop before the
  server starts.
- **A parked `write()` was already handled.** `close_stream_()` bumps `stream_generation_` and
  notifies `space_available_`, which is the existing mechanism for exactly this.

**"A stream it had" is `stream_ != nullptr`, and that test is load-bearing rather than a
formality.** PortAudio does not null the handle when a device goes away, but `open_stream_()` does
when it fails — so the handle is exactly what separates *this stream died* from *we never got
one*. Without it, a device that merely **refused** a format would be chased: `configure()`'s failed
open leaves a remembered format behind, the very next `write()` would repeat the identical failing
open on the sync thread, and its failure would then escalate to a full device-list rescan for a
stream no rescan can help — the device is present and simply will not take that format. That is a
blocking open plus a main-loop stall plus duplicate errors, per stream, bought for nothing.
`PlayerListener` keeps forwarding audio after a refusal by design, so this is the ordinary path
for an unsupported format rather than a corner.

Two smaller things fell out of it. `stopping_` is re-checked *after* a successful open, because
`stop()` deliberately latches before it takes the mutex and so can arrive while `Pa_OpenStream()`
is running — leaving a live stream for the destructor is the hazard `configure()` already refuses
to create. And the discard return stays frame-aligned after a failed reopen has zeroed
`bytes_per_frame_`, by falling back to the remembered format.

The reopen holds `mutex_` across `Pa_OpenStream()`, so a `configure()`, `clear()` or `stop()` on
the main loop can block behind it for a few hundred milliseconds. Bounded by the same budget as
everything else here, and against the one such open `configure()` already performs per stream.

**Tier 2 — the device-list rescan, on the main loop.** `AudioSink` gains
`virtual void poll(int64_t now_ms) {}`, a no-op by default, called from `main.cpp`'s loop beside
`mdns.poll()` and `control_socket.poll()`. `PortAudioSink::poll()` closes the stream, cycles
`PortAudioGuard::reinitialize()`, re-resolves against the fresh list and reopens. It cannot be
inlined into `write()`: it invalidates every `PaDeviceIndex` in the process, and neither
`Pa_Initialize()` nor `Pa_Terminate()` is thread-safe.

The costs are real and are documented at the call sites rather than only here. Re-enumerating
every host API blocks the main loop — and so `SendspinClient::loop()` beside it — for hundreds of
milliseconds to seconds, depending on the host; the figure is not pinned down, which is why the
hardware pass below is asked to measure it. That is affordable because it happens at most once
per stream and only on a stream that has already died (nothing but `reopen_in_place_()` can ask
for a rescan, and it only runs on a dead stream). The transport itself runs on its own thread, so
what is delayed is the dispatch of messages already received; the tightest deadline on that path
is the time-sync burst response, which the library gives ten seconds. It also only *really*
terminates because the sink's guard is the sole live one at runtime, PortAudio's init pair being
reference-counted;
anything that later reached `probe()` from the running loop would turn the rescan into a silent
no-op, which is now noted on `reinitialize()`.

**Both attempts are spent per configured stream, not per outage — and that bound is the design,
not a detail.** A successful reopen does not refill the budget. Without that, a half-present
device (a dock mid-handshake, a hub browning out) that opens and dies again would have `write()`
calling `Pa_OpenStream()` on the sync task's thread fifty times a second, logging an INFO line
each time, forever — the failure mode is worse than the silence being fixed. So a stream that
dies a second time goes straight to the rescan, and once both are gone the sink discards until
the next `configure()`, which re-resolves anyway and now does so against a list the rescan has
already refreshed.

The two-second gap before the rescan does two jobs, and the second is easy to miss. The obvious
one: the rescan is the last attempt there is, and a replugged DAC takes the host a moment to
enumerate, so spending it the instant the reopen failed would usually spend it before the device
is back. The load-bearing one: **it is the gap, not the budget, that bounds the rescan across
streams.** `reset()` refills the budget at every stream and how often streams start is the
server's choice — so the budget alone would permit one teardown per stream, however fast they
came. Because each cycle needs a fresh escalation and each escalation must then wait out the gap,
cycles stay floored that far apart whatever the server does.

**The decision lives in `src/sink_recovery.{h,cpp}` and is unit-tested there**, for the reason
`mdns_common.cpp` and `pcm_volume.cpp` exist: `src/portaudio_sink.cpp` is compiled only under
`SENDSPIN_CLI_PORTAUDIO_ENABLED` where `sendspin-cli-tests` is built unconditionally, so a policy
living in the sink would go untested precisely on the hosts that have no PortAudio. `SinkRecovery`
takes `now_ms` and never reads a clock, holds no PortAudio type and logs nothing, which is what
lets its tests be a plain table. The `Pa_*` calls themselves stay untested; the hardware pass
below is what covers them.

**`last_format_` is set from `configure()`'s arguments, not from a stream that opened.** It has to
outlive `close_stream_()`, which zeroes `rate_`/`channels_`/`bits_` — but taking it from a
successful open would be wrong in a way that matters: a `configure()` whose open failed is still
the format the player is about to send audio in, and recovering to the *previous* one would play
that audio at the wrong frame size. `stop()` zeroes it, which is what makes a write after shutdown
attempt nothing.

**Deliberately left out.** No timer thread, and — as this item shipped — no retry loop or backoff
curve either: the whole shape was two attempts and a stop. Item 20 later gave the *second* attempt
a backoff and a bounded retry, for the backends whose version of it is a server reconnect rather
than a device-list rebuild; PortAudio's remains the one attempt described here.

No re-advertising of formats mid-session: `capabilities()` is answered once
before `start_server()`, so a rescan does not change what the server was told, and the refusal
path that already names the device and the format it would not take stays the mitigation, as the
comment on `capabilities()` has said since item 3. `NullAudioSink` is untouched, having no
device to lose.

**What remains after this.** A device unplugged and replugged **between** tracks is still not
found, and that is the price of the `stream_ != nullptr` gate above. The next `configure()`
resolves against the stale cached list, gets the dead index, fails to open, and ends with no
stream — which from the sink's side is indistinguishable from the device refusing the format, the
case the gate exists to stop chasing. Telling them apart means reading *why* `Pa_OpenStream()`
failed and deciding which `PaError`s mean "gone" on each host API, which is not worth guessing at
without hardware to check it against. The mid-stream case, which is what this item was for, does
reach the rescan.

Once both attempts are spent, recovery waits for the next
`configure()` — which is a *second* disappearance if the first reopen failed, and a third only in
the luckier case where it worked. A rescan renumbers PortAudio's device list, so a numeric
`-o portaudio:2` re-resolved after one may name a different card than it did before; the recovery
log line names the device it actually landed on rather than only the spec, but the spec's meaning
genuinely moved, and any later `configure()` inherits the same shift. `capabilities()` still
describes whichever device was default when the player started. And the rescan's main-loop stall
is a real, if bounded, pause in protocol handling.

**And a correction to what this item left out.** `AlsaAudioSink` was excluded above on the
reading that ALSA recovers inside its own `write()` and so has nothing to do with a main-loop
tick. **That reading was wrong, and issue #45 is what it cost.** `recover_()` handled the three
transients in place and treated everything else — `-ENODEV` among them — as a failed write:
the handle stayed open on hardware that was gone, and nothing *before the next stream* retired
it. A device pulled out mid-track therefore moved no audio at all — `write()` broke out of its
loop and returned 0, the sync task re-presented the same buffer against the dead handle, and the
unthrottled `ERROR` repeated once per retry until the process restarted. Discarding is what the
sink does *now*; what it did then was spin. (The next `configure()` did retire the handle, by
failing its `prepare()` and reopening — which is why a fresh stream recovered by accident, and
why the outage lasted exactly as long as the track did.)

It now closes the device where the loss is found and escalates to `poll()` like the rest, with
one difference from the sound-server sinks: **the inline attempt is spent rather than made.**
`snd_pcm_open()` cannot be bounded the way `PULSE_RECOVERY_TIMEOUT_MS` bounds a Pulse reconnect,
and on a plugin PCM — `default` on a PipeWire host, or the `alsa:pulse` route — it parses config
and waits on a daemon socket with no timeout at all. Making that call on the sync task's thread,
under `device_mutex_`, would break the rule the sink's threading model rests on: that `write()`
never blocks unboundedly while holding it. `SND_PCM_NONBLOCK` is not the way out either — it
changes the opened stream's semantics, which `write()`'s `snd_pcm_wait()` model depends on, and
it bounds neither the config parse nor a plugin's connect. So `poll()` pays the open instead,
which is also the attempt a replug actually wants: `SINK_RESCAN_DELAY_MS` is roughly how long a
USB device takes to come back, and an immediate retry would be made before it had.

**What that costs, said out loud.** `poll()` holds `device_mutex_` across that open, so the stall
is the same real pause in protocol handling PortAudio's rescan is, and a concurrent `write()`
blocks on the mutex rather than honouring its `timeout_ms`. Neither is new: `configure()` already
makes an identical unbounded `snd_pcm_open()` on the main loop under the same mutex, on every
stream rather than only on a lost device. What bounds it here is the budget — at most
`SINK_RESCAN_ATTEMPTS` opens per configured stream, spaced by a doubling delay — and the case
that actually stalls is narrow: an absent `hw:` PCM fails fast, so paying real time needs a
plugin PCM waiting on a daemon socket, or an exclusive device another process now holds. Moving
the open off the loop would mean a recovery thread and a handle published under the lock, which
is a background thread this sink is deliberately built without. The measurement below is what
would justify revisiting that.

**Reasoned, not measured.** Unlike item 20, this was derived from the code and from the reporter's
logs; nobody has yet pulled a DAC out of a running player and watched it come back. See the
hardware pass below.

**Hardware verification is still owed** and is what the *shipped* qualifier above refers to. The
five cases to run, and to record here once run: a USB DAC unplugged and left out mid-track (logs
once, does not spin or wedge); the same DAC replugged mid-track (playback resumes without a track
boundary); the host default output switched mid-track under a bare `-o portaudio`; normal
playback, track changes and `stop()` unaffected when nothing goes wrong; and a measurement of how
long the rescan really stalls the loop, so the estimate above can be replaced with a figure.

The ALSA correction owes its own pass, and it is a differently shaped one — `-o hw:CARD=<name>`
rather than `-o portaudio`, and a `snd_pcm_open()` on the main loop rather than a device-list
rebuild. Four cases: a USB DAC powered off mid-track (two `ERROR`s, one naming the device and one
saying it is discarding, and then quiet — not one line per buffer, which is the symptom #45 was
reported for); the same DAC powered back on inside the
budget (playback resumes with no track boundary and no restart); one powered back on *after* the
budget is spent, which should recover on the next `configure()` and not before; and a measurement
of how long `snd_pcm_open()` really stalls the loop on an absent `hw:` device, which is the one
figure the "cannot be bounded" argument above leaves as an estimate.

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

### 18. Native PulseAudio backend — *shipped (audible slice)*

`-o pulse[:<sink>]` on libpulse. **Not required to reach a PulseAudio server** — `-o
pulse` fell through `resolve_device_spec()`'s rule 3 to ALSA's plugin PCM and played
long before this existed, and still does as `-o alsa:pulse`. This item is a
deliberate step past that minimum, and what it buys is on the far side of the socket:

- **Sink enumeration in `-l`**, so `-o` can name one. The plugin exposes a single
  ALSA hint, so through it a sink is chosen with `PULSE_SINK` or `~/.asoundrc`.
- **Honest sync feedback.** `pa_stream_get_latency()` is the *server's* answer for
  when queued audio plays out. Through the plugin the same question reaches
  `snd_pcm_delay()`, which reports the plugin's own buffering — and this is a
  synchronised multi-room player, so that is not cosmetic.
- **A named stream**, so the host's mixer shows `sendspin-cli` and can route it
  per-application rather than showing another "ALSA plug-in".

**Shipped** in `src/pulse_sink.{h,cpp}`, built when `pkg_check_modules(libpulse)`
succeeds (`-DSENDSPIN_CLI_WITH_PULSE=OFF` forces it out):

- **No ring buffer, deliberately** — the one structural difference from item 3.
  libpulse lets any thread write to a stream under the mainloop lock, so **the
  server's own queue is the ring**, sized by `--buffer-ms` through
  `pa_buffer_attr::tlength` and with `PA_STREAM_ADJUST_LATENCY` sizing the rest
  around it. A ring here would be latency `pa_stream_get_latency()` could not see,
  which is the very thing the backend exists to report honestly.
- Threading follows `AlsaAudioSink` rather than `PortAudioSink`: one mutex
  serialises everything, and volume is scaled on the way in with the ramp committed
  by the frames really written. Lock order is the sink's mutex then the mainloop
  lock, and no libpulse callback takes the sink's mutex.
- **Every wait has a deadline.** `pa_threaded_mainloop_wait()` has none — it returns
  when a callback signals it and never otherwise — so a socket that accepts and then
  says nothing would hang the player at startup. Every wait is a condition variable
  of our own instead, and a query that times out is `pa_operation_cancel()`ed so no
  late callback can write through a dead pointer.
- 8/16/24/32-bit. 8-bit is the awkward one: the player emits **signed** 8-bit and
  PulseAudio has no signed 8-bit format at all, so it goes out as `PA_SAMPLE_U8` with
  the sign bit flipped in the same pass that scales the volume. Refusing it would
  leave a stream the other backends play and this one does not.
- Recovery reuses `SinkRecovery` rather than a policy of its own, with its two attempts
  mapped onto what a sound server actually needs: the inline one reopens the stream when the context is
  still up, and the delayed one on the main loop reconnects the context — which is
  what a *restarted* daemon needs, and why `SINK_RESCAN_DELAY_MS` is the right gap
  rather than an arbitrary one.

  Its one poor fit — found by killing `pulseaudio` mid-stream rather than by reasoning
  — was that the second attempt was retired as it was handed out, so a daemon slower
  than `SINK_RESCAN_DELAY_MS` to come back was never asked again. Item 20 fixed that in
  the same change, and is where the whole of it is written down.

**The sharp edge, accepted knowingly.** `-o pulse` used to mean the ALSA plugin PCM
and now means this backend: a live command line changing what it does on upgrade.
The `null` precedent is *not* the justification — `null` is device-less, so shadowing
it cost nothing, where `pulse` is a real device on exactly these hosts. What pays for
it instead: `-o alsa:pulse` is the documented way back and is named in the README, in
`-l`, and in the message a build without the backend gives; and `-l`'s shadow filter
is derived from the backend table through `alsa_pcm_is_reachable()` rather than from
a second hardcoded name list, so the PCM list cannot drift into naming a device `-o`
can no longer reach.

Volume stays on `src/pcm_volume.{h,cpp}` and is **not** pushed to the server as a
sink-input volume — the same one-path-or-the-other call item 15 makes for ALSA's
hardware mixer, for three reasons that agree: the spec's `(volume/100)^1.5` is not
PulseAudio's cubic taper, stacking the two would square it, and a gain `pavucontrol`
can move behind our back makes the guarantee in `src/audio_sink.h` — never report a
volume the speaker is not at — unenforceable, with group volume derived from what
players report.

### 19. Native PipeWire backend — *shipped (audible slice)*

`-o pipewire[:<node>]` on libpipewire. **This item does not clear the bar item 3
set**, and says so rather than leaning on it: item 3's justification was that
PortAudio is *the only way this player makes noise on macOS*, and libpipewire reaches
no host libpulse cannot — `pipewire-pulse` is on every PipeWire desktop, and item 18
therefore already plays there. It is here for what is on the far side of the socket:

- **Node selection.** `pipewire-pulse` presents *sinks*, which is a compatibility
  view of the graph rather than the graph. Only a native client can name a node, and
  only a native client has no compatibility layer in the audio path at all.
- The same enumeration and mixer-visibility arguments item 18 makes, one layer
  further in.

**Shipped** in `src/pipewire_sink.{h,cpp}`, built when
`pkg_check_modules(libpipewire-0.3 >= 0.3.64)` succeeds
(`-DSENDSPIN_CLI_WITH_PIPEWIRE=OFF` forces it out). The version floor is real:
`PW_KEY_TARGET_OBJECT`, which is how `-o pipewire:<node>` names a node, arrived in
0.3.64 and an older libpipewire would build and then quietly ignore the node.

- **A ring buffer, unlike item 18** — and the difference is not a preference. PipeWire
  *pulls*: the graph runs `process()` on its own realtime data thread and wants a
  buffer filled there and then, with no way to block. That is PortAudio's shape, so it
  buffers PortAudio's way, through the same SPSC ring — which moved to
  `src/pcm_ring.{h,cpp}` rather than being copied, since a second copy of a lock-free
  ring is the kind of duplication that drifts silently. It is device-free and
  clock-free there, so it compiles and can be tested on a host with no audio backend,
  the split `src/sink_recovery.{h,cpp}` and `src/pcm_volume.{h,cpp}` already make.
- Sync feedback from `pw_stream_get_time_n()`: `pw_time::delay` is when the next
  sample this stream hands over is presented, plus whatever `queued` and `buffered`
  say is already ahead of it, plus the buffer's own duration for its last frame.
  `pw_buffer::size` is set in **frames**, because that is what `pw_time::queued` sums.
- Volume is scaled on the way into the ring rather than in `process()`, so no gain
  arithmetic runs on the realtime thread at all — the opposite of item 3's choice, and
  for the reason item 2 makes it: the realtime thread should carry as little as
  possible. Not that it carries *nothing*: `process()` still queries `pw_time`, notifies
  the producer and calls `on_frames_played`. That is the same pragmatic trade
  `src/pcm_ring.h` names, and the one item 3 already made.
- Node enumeration for `-l` and `probe()` is a registry walk with its own thread loop,
  context and core, ended by a `pw_core_sync()` round trip so one pass is *complete*
  rather than merely likely, and bounded by `pw_thread_loop_timed_wait()`. Only
  `Audio/Sink` nodes are listed, since `-o` cannot play through the others.
- No node is marked as the default in `-l`, deliberately: where a playback stream
  lands with no target named is the graph's own routing decision, taken per stream and
  changeable while one is running, so marking one would be a claim `-l` cannot make.

Item 18's note on shadowing, on volume, and on `SinkRecovery` applies here unchanged —
`-o pipewire` was an ALSA plugin PCM too, and `-o alsa:pipewire` is the way back.

**Cross-repo consequence, flagged rather than fixed here.** Both backends are
auto-detected and default `ON`, and
[`local-audio-addon`](https://github.com/music-assistant/local-audio-addon)'s
Dockerfile asserts the configure summary line exactly
(`grep -qE '^-- sendspin-cli audio backends: null, stdout, alsa$'`). If that build
image ever gains `libpulse-dev` or `libpipewire-0.3-dev`, its own assertion fails. It
needs `-DSENDSPIN_CLI_WITH_PULSE=OFF -DSENDSPIN_CLI_WITH_PIPEWIRE=OFF` alongside the
`-DSENDSPIN_CLI_WITH_PORTAUDIO=OFF` it already passes, or a widened grep — in that
repo, not this one.

### 20. `SinkRecovery`'s second attempt reports back — *shipped*

Found while building items 18 and 19, by killing `pulseaudio` mid-stream rather than by
reasoning about it: the sink went silent for the rest of the track. Not a defect in
either sound-server backend — they reuse `SinkRecovery` exactly as items 3 and 14 wrote
it — but a place where that policy had been shaped by the one backend that then existed.

`rescan_due()` used to mark the second attempt spent **as it handed it out**, and
`src/sink_recovery.h` said why: a rescan that failed and one that worked leave nothing
further to try either way. True of a PortAudio device-list rebuild, which is what it was
written for. Not true of a *reconnect*: a daemon still down two seconds after the outage
may well be up two seconds later, and nothing has been learned that makes asking again
pointless.

**Shipped** in `src/sink_recovery.{h,cpp}`, its callers (`src/portaudio_sink.cpp`,
`src/pulse_sink.cpp`, `src/pipewire_sink.cpp`, and `src/alsa_sink.cpp` since item 14's
correction) and `tests/sink_recovery_test.cpp`:

- **`rescan_done(bool)`, symmetric with `reopen_done(bool)`.** A failure re-arms the
  attempt; a success retires it. Which of the two an attempt is worth is now the
  backend's to say rather than a property baked into the helper.
- **PortAudio's behaviour is bit-identical.** `PortAudioSink::poll()` reports
  `rescan_done(true)` whatever the outcome — its `Pa_Terminate()`/`Pa_Initialize()`
  cycle really is one-shot — and reports it *before* the work, so none of that
  function's early returns can drop it. A caller that reports nothing at all still gets
  exactly one attempt, so the change could not silently turn some future backend into a
  retry loop; `ACallerThatReportsNothingGetsExactlyOneRescan` covers that arm on its own.
- **Bounded twice over, in the code rather than in a promise.** `SINK_RESCAN_ATTEMPTS`
  (5) caps the count, and the delay doubles from `SINK_RESCAN_DELAY_MS` to a ceiling of
  `SINK_RESCAN_MAX_DELAY_MS` — 2 + 4 + 8 + 16 + 30 seconds, a minute of cover. The
  doubling is the half that matters to the main loop: a reconnect that waits on an
  unresponsive socket costs the same every time, so the only way to stop paying it at a
  fixed duty cycle is to ask less often.
- **One loud line at the end, and quiet ones before it.** Each failed attempt is a
  normal step of a restart and logs at `debug`; the last one, where the sink has really
  given up until the next stream, logs at `warn`. `pending()` is what tells the two
  apart, so the sinks need no counter of their own.

Measured, not reasoned: killing the sound server mid-stream and restarting it a few
seconds later now gets audio back without waiting for the next track.

Item 14's hardware pass is still the place to exercise the PortAudio half — a USB DAC
replug and a `systemctl --user restart pipewire` are the two ways to produce one of
these on purpose, and only the second has been run.


### 21. PipeWire's ring is sized before the graph says what it wants — *open*

Found in review of items 18 and 19, not by playing anything: `RING_QUANTUM_MULTIPLE`
was documented as a floor on the ring and never actually applied to one. The ring was
sized from `--buffer-ms` alone, the quantum is only knowable once the graph has run a
cycle, and the constant was reached for exactly once — to decide the wording of a
`debug` line. A floor that only ever describes is not a floor.

That matters because `process()` asks for a whole quantum. A ring that cannot hold one
short-reads and zero-fills the remainder **on every cycle, for the life of the stream**,
however promptly `write()` refills it. It is not a starvation that recovers.

**Half of it is fixed** in `src/pipewire_sink.{h,cpp}`, because half of it is cheap:

- **The stream now asks.** `PW_KEY_NODE_LATENCY` is set to a third of the ring at the
  stream's rate, so the graph is told what this client can absorb. Previously it stated
  no latency at all and took whatever the graph was running.
- **The arithmetic moved out and grew tests.** `pipewire_ring_frames()` and
  `pipewire_quantum_fit()` are pure and live beside the sink rather than inside it,
  covered by `tests/pipewire_sink_test.cpp` — including the case that cannot be produced
  on demand from a daemon, a quantum larger than the whole ring. The same split, for the
  same reason, as `src/pcm_ring.{h,cpp}` and `src/sink_recovery.{h,cpp}`.
- **A ring that cannot hold a quantum is now a `warn`, not a `debug`**, and it names the
  `--buffer-ms` that would clear the floor — a figure rounded up, so passing it back in
  actually satisfies the check it was printed for.

**What is left, and why it is left.** `PW_KEY_NODE_LATENCY` is a request. A graph with
`default.clock.force-quantum` set overrides it, and then the warning is all there is:
the player names the fault and keeps zero-filling. Fixing that properly means observing
the quantum and resizing the ring to suit — which cannot be done in place, because the
realtime data thread is reading that ring with no way to be told to wait. It means
tearing the stream down and reopening it at a new size, on the first `process()` of a
stream that has just started, which is a reconnect in the middle of the audible path and
wants the same care item 20's recovery got. Worth doing deliberately rather than as a
review fix.

**Flagged rather than fixed here.** The new `pipewire-minimum` CI job — Debian 12, and
so libpipewire 0.3.65, the oldest release the README claims — is the only build in
`.github/workflows/build.yml` that does not pass `-DSENDSPIN_CLI_WERROR=ON`. gcc 12 has
a `-Wrestrict` false positive on `line += " " + std::to_string(...)` in
`src/control_common.cpp`, and it is the *only* thing standing between that leg and the
warning line every other leg holds. One site, one rewrite; it belongs to whoever next
touches the control protocol rather than to a PipeWire change.


### 22. Stream hooks (`--hook-start` / `--hook-stop`) — *shipped*

The Python `sendspin-cli` grew hooks because headless installs asked for them: the box a
player runs on usually has an amplifier relay, a light, or a `curl`-able switch next to
it, and "a stream started" is the moment to flip it. The C++ CLI had no equivalent, and
no workaround either — the control socket answers questions, it does not announce events.

**Shipped** in `src/hooks.{h,cpp}`, `src/cli.{h,cpp}`, `src/player_listener.{h,cpp}`,
`src/main.cpp` and `tests/hooks_test.cpp`:

- **Two long-only flags, and their config keys.** `--hook-start` / `--hook-stop` run a
  shell command (`/bin/sh -c`, so pipes and `&&` are one hook) on stream start and stop.
  `hook-start` / `hook-stop` in the config file, through the same `apply_option()` door
  as every other key.
- **The Python CLI's vocabulary, verbatim.** `SENDSPIN_EVENT` (`start`|`stop`)
  always; `SENDSPIN_SERVER_ID`, `SENDSPIN_SERVER_NAME`, `SENDSPIN_SERVER_URL` (the URL
  this run dialled — outbound only, as in Python) and `SENDSPIN_CLIENT_NAME` where
  known. A hook script written against one player runs unchanged against the other.
  `SENDSPIN_CLIENT_ID` carries the `--id` value item 23 chooses: the library's
  MAC-derived default is not exposed, so without the flag it stays unset.
  `SENDSPIN_SERVER_URL` is what this run *dialled*, which is not the same
  claim as which server answered: `-s` leaves the inbound listener up, and the library
  reports that a connection is up without saying where it came from — no connect callback,
  and nothing exposing a connection's URL or direction — so one that dialled in while an
  outbound attempt was outstanding or had failed carries the attempt's URL. Closing that
  needs sendspin-cpp to name the live connection's origin; until it does, the docs point a
  hook that must be certain at `SENDSPIN_SERVER_ID`, which is read from the connection the
  stream arrived on. An unknown is left *unset* rather than exported empty, and inherited
  `SENDSPIN_*` variables are cleared so a wrapper script's stale export cannot describe
  some other run. Both events of a stream are told what was gathered when it started: by
  the time one ends its connection is usually already gone, and asking again would hand
  the stop hook a server it can no longer name.
- **Nothing waits on a hook.** The spawn is a `fork()`/`execve()` with the environment
  built before the fork — this process has the library's background threads, so the
  child may only touch async-signal-safe calls — and the reap is a `WNOHANG` `waitpid()`
  polled from the main loop, next to mDNS and the control socket. A non-zero exit or a
  signal death is one `W hook:` line; a hook still running at shutdown is left to
  finish, because an amplifier half-switched is worse than an orphan.
- **The stream's end is waited for on the way out.** `disconnect()` only enqueues it, and
  it is `client.loop()` that delivers it, so without this a player killed mid-stream reaches
  `return 0` with its stop hook unrun and the amplifier still on. The loop is pumped after
  the disconnect until the listener reports the stream over, bounded by
  `SHUTDOWN_DRAIN_MS` against a wait of about fifty, and says so and goes if that passes.
  The stop hook it spawns is by definition the orphan case above: nothing waits on it.
- **Fired on the stream lifecycle, not on the format being accepted**, through a
  `PlayerListener::on_stream_event` seam shaped like `AudioSink::on_frames_played`. A
  stream the device refused is audio arriving and being discarded — the case where the
  amplifier being on matters most — so the hook window is exactly `status`'s
  `stream: receiving`.
- **The child's stdout goes to stderr**, because with `-o stdout` the parent's stdout is
  carrying PCM and a hook's `echo` would land in the middle of the audio. Under `-f`
  both end up in the logfile, which is where a hook's output belongs anyway.
- **And nothing else of the player's crosses into it.** Every descriptor above stderr is
  closed in the child, because an exec carries them all: a hook that kept the audio port's
  listening socket holds it for as long as it runs, and a restart in that window fails to
  bind while still logging that it is listening. Closed there rather than opened
  close-on-exec at each site, since the library's and IXWebSocket's sockets are not ours to
  reach into. SIGPIPE goes back to `SIG_DFL` for the same reason: the player ignores it, and
  an ignored disposition survives `execve()` where a caught one does not, so `… | head -1`
  inside a hook would otherwise report a failed write rather than ending.


### 23. Identity flags (`--id`, `--manufacturer`, `--product-name`) — *shipped*

The client id was the library's MAC-derived default with no way to choose one, and the
`client/hello` device info was hardcoded. The id is the half that bites: it is the
*stable* identity a server files volume, group membership and pairing under — `-n` is
only what it displays — and two players on one host derive the same MAC, so each
server-side setting lands on whichever connected last. The dual-mono
two-daemons-one-host pattern needs a flag; the Python CLI has all three.

**Shipped** in `src/cli.{h,cpp}`, `src/main.cpp`, `tests/cli_test.cpp` and
`tests/config_file_test.cpp`:

- **`--id <id>`** sets `SendspinClientConfig::client_id`, and its config key is `id`.
  The default stays empty on purpose — the library derives the MAC-based id only when
  nothing is set, and that remains the right identity for one fixed endpoint per host.
  The flag also feeds item 22's `SENDSPIN_CLIENT_ID`, which until now was reserved but
  never set: the derived id is not exposed by the library, so the flag's value is the
  first honest one a hook can be handed.
- **`--manufacturer` / `--product-name`** override the hello device info, for the
  integrator whose product embeds this player and should be listed as itself. Defaults
  stay `sendspin-cpp-cli` / `sendspin-cli`, declaring what this really is.
- All three are refused empty, warned about on a subcommand run like every other
  daemon-only flag, and settable from the config file through the same
  `apply_option()` door.

Verified against a real `aiosendspin` server: the connected client reports the `--id`
value as its `client_id`, and the start hook exports it as `SENDSPIN_CLIENT_ID`.


### 24. `--audio-format` pin — *shipped*

Item 4 made the advertisement device-derived and ranked, and that is the right default —
but it decides *for* the operator. The case it cannot cover is the fussy DAC: a device
that opens at many formats and is only actually happy in one, where the fix is to hold
the player at that shape and refuse to run any other way. The Python CLI's
`--audio-format` is exactly that, validated against the device at startup with a hard
exit on failure.

**Shipped** in `src/supported_formats.{h,cpp}`, `src/cli.{h,cpp}`, `src/main.cpp`,
`tests/supported_formats_test.cpp` and `tests/cli_test.cpp`:

- **`--audio-format <codec:rate:depth:channels>`** (config key `audio-format`), e.g.
  `flac:48000:24:2`. The grammar is the Python CLI's, extended with `opus` because this
  player decodes it.
- **A reorder, not a narrowing.** `pin_preferred_format()` moves the pinned entry to the
  front of the derived advertisement — the protocol has `supported_formats` in priority
  order, so the front is the whole of what "preferred" means on the wire — and
  everything else the device takes is still offered behind it, so a server that cannot
  encode the pin has the rest of the list to fall back on.
- **A pin the advertisement does not carry refuses to start**, naming the format, the
  device, and `-l` as the way to see what the device itself reports — not the same set as
  what is advertised, which carries a single channel count. Checked against the *derived*
  list — or the permissive fallback when the device reported nothing, since that is what
  actually goes out — so the refusal describes the real advertisement.
- **Parse-time shape validation, startup-time advertisement validation.** The grammar, the
  codec names, the four emittable bit depths and the one shape Opus is ever advertised in
  are settled in `parse_format_spec()` when the flag is read — so a config file is
  validated without opening a device — and whether the advertisement carries the format is
  answered where the sink is real.

Verified against a real `aiosendspin` server: with `pcm:44100:16:2` pinned on a device
whose ranked head is FLAC 48 kHz, the server encoded and streamed PCM at 44100 Hz.


### 25. `--interface` — *open*

Bind the inbound listener — and scope the advertisement — to one interface, instead of
taking every address the host holds. **Nothing here is buildable against the pinned
library**, so this item exists to settle the design and the evidence rather than to leave
them to be re-derived.

**The gap, in the real code.** The listener is constructed with a literal at
sendspin-cpp's `src/host/ws_server.cpp:53` — `ix::WebSocketServer(server_port_,
"0.0.0.0", ...)` — and its `SendspinClientConfig` (`include/sendspin/config.h`) carries
`server_port` and `server_max_connections` and nothing else about the socket. This repo's
advertisement registers on `kDNSServiceInterfaceIndexAny` at `src/mdns_dnssd.cpp:200`.
Those two are *consistent* today: both unrestricted. The flaw is not a mismatch between
them — it is that neither can be narrowed, and only the listener would matter if it could.

**The one knob that looks like it and is not.** `SendspinClientConfig` does carry a
`mac_address` override, documented upstream as "recommended on multi-homed hosts where
detection may pick the wrong interface" — the same scenario, so it is worth saying that it
is not the same lever. That value goes into `client/hello`'s `device_info`: it is identity,
and it moves no socket.

**Why this is more than "it listens broadly".** Pinned v0.7.2 has **no inbound
authentication of any kind**: no PSK, no pairing gate on the inbound path, and — per item
17, whose text still cites v0.7.0, re-checked here at v0.7.2 — no `activities` /
`server/activate` either, since it still runs the older `connection_reason` handoff.
`server_max_connections` is the only inbound limit, and it counts sockets rather than
judging them. Reachability is therefore authorization: whatever reaches the listen port
completes the handshake and drives the player, and a bind address would be the only such
control the player itself offers.

**That is a statement about the pinned library, not about Sendspin.** The spec
authenticates in the handshake — pairing and a PSK, as item 6 records — and v0.7.2 does
not implement that half yet. This item's premise expires when it does.

**In proportion.** Most installs sit behind NAT, where no interface holds a routable
address and the exposure is the LAN the player is meant to serve anyway. The real cases
are multi-homed hosts, VPSes, tunnel interfaces and untrusted VLANs. **A host firewall is
the working mitigation today**, and it is the honest answer for an operator who needs one
now.

**Why the mDNS half is not shippable on its own.** An advertisement scoped to one
interface while the socket still listens on `0.0.0.0` hides the player without protecting
it: anything that already knows the address and port still connects. It does buy something
real — an unscoped register publishes address records on every interface, and a server can
take one that is unreachable from where it sits — but that is a *discovery-correctness*
fix, not access control. Shipping it under this flag's name would sell it as the latter.
Both halves land together, or the mDNS half lands under its own name.

**The parity record, corrected.** The Python CLI is the usual reference for a flag like
this, and two things commonly assumed about it are wrong — the flag checked in
`sendspin-python-cli`, the behaviour in `aiosendspin` `6.0.1`, the version that CLI pins:

- **Its `--interface` takes an IP address, not an interface name.** `sendspin/cli.py:475`
  documents it as "IP address of the network interface to bind to", and
  `sendspin/daemon/daemon.py:212` passes it through unexamined as
  `host=self._args.interface if ... else "0.0.0.0"`. There is no `if_nametoindex()`
  anywhere in it.
- **It does not scope the advertisement to that address.** The bind is honoured —
  `ClientListener` does `web.TCPSite(self._runner, self._host, self._port)` — but
  `_start_mdns()` builds a bare `AsyncZeroconf()` and takes the address it publishes from
  `get_local_ip()` in `aiosendspin/util.py`, a UDP route lookup to `8.8.8.8`.
  `self._host` never reaches it. Unchanged at `9.1.0`.

  What Python *does* scope locally is the **browse**: `ServiceDiscovery(interfaces=[...])`
  hands the value to `AsyncZeroconf(interfaces=...)` in `sendspin/discovery.py:150`. That
  is the outbound discovery path, not the inbound listener.

**The flag, settled.** `--interface <name-or-address>` (config key `interface`), long-only
like the other non-squeezelite flags. **One value has to yield both halves** — the bind
needs an *address*, `DNSServiceRegister` needs an *index* — and a name is what an operator
on a Pi actually knows, while the address is the thing a DHCP lease changes underneath
them. That makes this a deliberate superset of Python's address-only flag rather than a
divergence from it.

**Both directions have to be resolved, and neither is a lookup.** A name yields an index
from `if_nametoindex()` but must go through `getifaddrs()` for an address — and an
interface holds several: a v4, a link-local v6, often a global v6, sometimes aliases.
`ix::WebSocketServer` takes one host string, so **which one is a decision to settle when
the flag is built**, not a detail: prefer the first global v4, fall back to a global v6,
and refuse a link-local v6 rather than guess at a `%scope` suffix the transport may not
accept. In reverse, a literal address is what the bind wants directly but yields an index
only by scanning `getifaddrs()` for the interface holding it — which can match none, and
with aliases can match more than one. Both misses are refusals, not fallbacks.

**Validated in two stages, the split item 24 established.** The shape is checked when the
flag is read — a name within `IFNAMSIZ`, or an address `inet_pton()` accepts — so a config
file is validated without touching the host, and a typo still hard-fails at parse time with
the single `error:` line of item 1. **Resolution happens at startup**, where the interfaces
are real, because it depends on live host state: a Pi booting with `interface = eth0` in
its config before DHCP has brought the link up must fail where a device failure is reported,
not from inside the parser. That is the same seam item 24 drew between `parse_format_spec()`
and the advertisement check, for the same reason.

**Scope when it lands.** The inbound listener, which is the whole point, and the
advertisement's interface index. The outbound `-s` dial stays out: it is kernel-routed,
as it is in Python. Scoping the browse at `src/mdns_dnssd.cpp:217` is the one thing Python
does locally and can come along as an extension — but it is discovery convenience, not
security, and is not what the flag exists for.

**The upstream gate.** A bind address on `SendspinClientConfig`, threaded through
`ConnectionManager` into `SendspinWsServer` to replace the literal. Host-only and small,
but `SendspinWsServer` exposes only connection callbacks — there is no transport seam the
CLI can reach around it — so the flag cannot be expressed by any other route. Gated on a
`SENDSPIN_GIT_TAG` bump the same way item 17 is, and likely to arrive with one rather than
on its own. **Not expected to move soon.**
