# sendspin-cpp-cli

A headless **CLI / daemon audio player** built on the
[sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) synchronized
audio-streaming library, taking its command-line and control ergonomics from
[squeezelite](https://github.com/ralph-irving/squeezelite).

> **Status: early scaffold.** This repository is being stood up as an *epic*.
> The initial task brings up the build and boots a sendspin client; feature work
> is tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## What it is

Like squeezelite is a headless endpoint for Lyrion/Logitech Media Server,
`sendspin-cli` is a headless endpoint for the **Sendspin** protocol: it
advertises itself over mDNS (`_sendspin._tcp`), is discovered, and is driven by
a remote sendspin *controller* — while a small, squeezelite-style flag set
configures identity, audio output, discovery, logging, and daemonization. A
local control channel (socket / subcommands) is planned so you can also drive it
from the same host.

- **Library:** sendspin-cpp (`player` role; FLAC / Opus / PCM), pulled via CMake
  `FetchContent`.
- **Audio out:** an `AudioSink` seam — ALSA (the default Linux/Docker backend,
  with `snd_pcm_delay()`-based sync feedback), a null/stdout sink for device-less
  containers, and PortAudio for cross-platform development still to come.
- **Binary:** `sendspin-cli`.

## Build

Requires **CMake ≥ 3.16**, a **C++20** compiler, and network access on the first
configure. sendspin-cpp is pulled in with `FetchContent` at a pinned tag, and it
fetches its own dependencies (ArduinoJson, micro-flac, micro-opus, IXWebSocket) in
turn.

**libasound (ALSA) is optional and auto-detected.** Where the development headers
are present, the ALSA output backend is compiled in and becomes the default `-o`;
where they are not, the build silently falls back to the device-less sinks, so
macOS and sound-card-less containers still build and run. The configure output
says which backends you got:

```
-- sendspin-cli audio backends: null, stdout, alsa
```

```bash
sudo dnf install alsa-lib-devel      # Fedora / RHEL
sudo apt install libasound2-dev      # Debian / Ubuntu
```

Pass `-DSENDSPIN_CLI_WITH_ALSA=OFF` to leave ALSA out even where it is available.

> C++20 rather than C++17: sendspin-cpp's host build declares
> `target_compile_features(sendspin PUBLIC cxx_std_20)`, so the requirement
> propagates to anything that links it.

```bash
git clone https://github.com/chrisuthe/sendspin-cpp-cli.git
cd sendspin-cpp-cli
cmake -B build
cmake --build build
./build/sendspin-cli --help
```

To build against a different version of the library:

```bash
cmake -B build -DSENDSPIN_GIT_TAG=v0.7.0
```

## Run

`sendspin-cli` is a listener: it starts a WebSocket server and waits for a Sendspin
server to drive it. Until mDNS advertisement lands, either point your server at
`ws://<this-host>:8928/sendspin` or dial it from here with `-s`.

```bash
# Listen on the default port and play through the system's default sound card
./build/sendspin-cli -n living-room

# Dial a server explicitly
./build/sendspin-cli -s 192.168.1.10

# Debug logging, in the foreground
./build/sendspin-cli -d debug
```

### Choosing an output

With the ALSA backend compiled in, `-o` takes any PCM name `aplay -L` prints, and
`-l` lists them with their descriptions. Three names stay reserved for the
device-less sinks on every build: `null` discards audio, and `stdout` (or `-`)
writes raw interleaved PCM to standard output.

```bash
./build/sendspin-cli -l                 # what this host can play through
./build/sendspin-cli -o default         # follow the system config (PipeWire/Pulse)
./build/sendspin-cli -o hw:2,0          # a card directly, bypassing the sound server
./build/sendspin-cli -o plughw:2,0      # same, letting ALSA convert rate/format
./build/sendspin-cli -o null            # no sound card needed at all
```

`default` is the default when the ALSA backend is present, and `null` when it is
not. Volume is applied in software, so it works the same through PipeWire's ALSA
plugin as through bare hardware; the ALSA hardware mixer is a follow-up.

The flags follow squeezelite's: `-o` output device, `-l` list devices, `-n` name,
`-s` server, `-z` daemonize, `-P` pidfile, `-d`/`-f` logging. Run `--help` for the
current state of each — several are scaffolding whose real behaviour is still to come.

## Roadmap

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the epic breakdown and the child
tasks that build out audio backends, discovery, daemonization, the local control
channel, Docker packaging, and more.

## Upstream

- Sendspin protocol library: https://github.com/Sendspin/sendspin-cpp
- Control-scheme inspiration: https://github.com/ralph-irving/squeezelite

## License

Licensed under the [Apache License 2.0](LICENSE), matching sendspin-cpp.
