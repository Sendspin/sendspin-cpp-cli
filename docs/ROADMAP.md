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
  server, optionally dials a server with `-s`, pumps `client.loop()`, and shuts down
  cleanly on `SIGINT`/`SIGTERM`.
- Defines the `AudioSink` seam (`src/audio_sink.h`) and plays real audio through it: an
  auto-detected ALSA backend (item 2 below), with the device-less null/stdout sink as the
  fallback, so the binary still runs where there is no sound card.
- Parses the squeezelite-style flag surface: `-o -l -n -s -z -P -d -f --port --help
  --version`, validating every value at parse time and refusing to start on a bad one
  (item 1).

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
  upstream's `PortAudioSink::apply_volume_()`.
- libasound's own stderr diagnostics routed through the CLI logger at `debug`.

**Not in this slice.** Three things were deliberately left out; each is now tracked on
the item that owns it, rather than as a loose follow-up here:

- The **ALSA hardware mixer** (`snd_mixer_*`, squeezelite's `-V`) → item 4. Software
  scaling was chosen first because the usual `default` device here is PipeWire's ALSA
  plugin, where a hardware mixer element either does not exist or moves something other
  than this stream. A hardware path is worth having for `hw:` output, where it is the
  real volume control.
- **Buffer/period tuning** (squeezelite's `-a`) → item 4. The ring is currently a fixed
  100 ms with 20 ms periods.
- **Richer per-card enumeration** → item 1, where it shipped. `-l` now prints each PCM's
  rates, formats, and channel counts alongside its hint.

One known rough edge: because the start threshold is a full ring, `snd_pcm_delay()`
under-reports the finish time until playback actually begins, so the first ~100 ms of
timestamps on a track are optimistic. It self-corrects once running. Lowering the start
threshold is the fix if it ever shows as drift at track boundaries — see item 4.

### 3. PortAudio backend

The cross-platform development backend (macOS and Linux), and what the sendspin-cpp
examples themselves use. `examples/common/portaudio_sink.cpp` upstream is a working
reference, including its lock-free ring buffer bridging push writes to PortAudio's pull
callback.

### 4. Player role wiring and output tuning

Take the stub past "it compiles": `PlayerRoleConfig` tuning (`fixed_delay_us`,
`extra_startup_silence_ms`), correct handling of mid-stream format changes, and underrun
behaviour.

Also the two knobs the ALSA backend (item 2) deliberately left fixed:

- **Buffer and period sizing**, exposed the way squeezelite's `-a` does it. ALSA is
  currently pinned to a 100 ms ring with 20 ms periods, chosen to keep `snd_pcm_delay()`
  a useful sync signal without inviting underruns. Tuning it also covers the start
  threshold, which is what makes early-track timestamps optimistic today.
- **Volume routing**: software scaling where the backend has no mixer (what ALSA does
  now, and the right answer through PipeWire), and the **ALSA hardware mixer**
  (`snd_mixer_*`, squeezelite's `-V`) where there is a real one to drive — chiefly `hw:`
  output straight to a card.

### 5. mDNS advertise/discovery + outbound mode

Advertise `_sendspin._tcp` so servers discover the player without being told about it.
Upstream's example uses `dns_sd.h` (Bonjour on macOS, `libavahi-compat-libdnssd` on
Linux); an Avahi-native path avoids the compat shim. Also make `-s` a first-class
outbound mode: retry, reconnect, and discovery of a named server.

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
parser suite lives in `tests/`; **the sink contract is still untested** — that is what a
`NullAudioSink`/`AlsaAudioSink` suite would add, alongside the matrix and the smoke test.
