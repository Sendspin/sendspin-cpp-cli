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
- Defines the `AudioSink` seam (`src/audio_sink.h`) with a device-less null/stdout
  implementation, so the binary runs where there is no sound card.
- Parses the squeezelite-style flag surface: `-o -l -n -s -z -P -d -f --port --help
  --version`.

## Child tasks

### 1. CLI argument parser

Flesh out the flag surface: validation, `<device>` specs per backend, a real
`--list-devices` that enumerates hardware, config-file interaction, and unit tests for
the parser. `src/cli.cpp` currently covers the flags but not the depth behind them.

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

**Deliberately deferred** to follow-ups:

- The **ALSA hardware mixer** (`snd_mixer_*`, squeezelite's `-V`). Software scaling was
  chosen first because the usual `default` device here is PipeWire's ALSA plugin, where
  a hardware mixer element either does not exist or moves something other than this
  stream.
- **Buffer/period tuning flags** (squeezelite's `-a`). The ring is currently a fixed
  100 ms with 20 ms periods.
- **Richer per-card enumeration** — `-l` prints the PCM hints, not per-card capability
  detail (supported rates, formats, channel counts).

### 3. PortAudio backend

The cross-platform development backend (macOS and Linux), and what the sendspin-cpp
examples themselves use. `examples/common/portaudio_sink.cpp` upstream is a working
reference, including its lock-free ring buffer bridging push writes to PortAudio's pull
callback.

### 4. Player role wiring

Take the stub past "it compiles": buffer sizing, `PlayerRoleConfig` tuning
(`fixed_delay_us`, `extra_startup_silence_ms`), correct handling of mid-stream format
changes, underrun behaviour, and applying software volume where the backend has none.

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

### 9. Docker

Image and compose file. ALSA with `/dev/snd` passed through for real output, and the
null sink for device-less containers and CI.

### 10. Packaging

`install()` rules, a systemd unit, and distribution packaging.

### 11. Interactive TUI mode

Optional, later. Upstream's `examples/tui_client` shows the shape.

### 12. CI and tests

A build matrix (Linux and macOS), unit tests for the parser and the sink contract, and a
smoke test that the binary boots, links, and exits cleanly on a signal.
