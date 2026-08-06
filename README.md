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

`-l` lists what this host can play through, and for each ALSA PCM the rates,
formats, and channel counts it actually accepts:

```
  hdmi:CARD=NVidia,DEV=0
      HDA NVidia, HDMI 0
      rates:    32000 44100 48000 88200 96000 176400 192000
      formats:  S16_LE S32_LE
      channels: 2 4 6 8
```

Only the four formats `sendspin-cli` can emit are ever listed — `S8`, `S16_LE`,
`S24_3LE`, `S32_LE` — since anything else is a capability this player cannot
reach. A plug-style PCM (`default`, `plughw:`, most sound-server PCMs) reports
nearly everything because the plug layer converts, so its list says little about
the hardware behind it. A device another process holds exclusively is reported as
in use rather than dropping out of the listing.

`-o` reads its argument in three steps, in this order:

1. one of the reserved device-less names — `null` discards audio, `stdout` (or
   `-`) writes raw interleaved PCM to standard output. These mean the same thing
   on every build, even where ALSA ships a PCM of the same name;
2. `<backend>:<device>`, split on the **first** colon, where the backend is one of
   the names the build reports (`null, stdout, alsa`). The split is on the first
   colon because ALSA device names carry their own, so `-o alsa:hw:2,0` is the
   `alsa` backend playing `hw:2,0`;
3. anything else is an ALSA PCM name, which is squeezelite's model — so `-o hw:2,0`
   and `-o default` keep working with no prefix at all.

```bash
./build/sendspin-cli -l                 # what this host can play through
./build/sendspin-cli -o default         # follow the system config (PipeWire/Pulse)
./build/sendspin-cli -o hw:2,0          # a card directly, bypassing the sound server
./build/sendspin-cli -o plughw:2,0      # same, letting ALSA convert rate/format
./build/sendspin-cli -o alsa:hw:2,0     # the same card, naming the backend explicitly
./build/sendspin-cli -o null            # no sound card needed at all
```

`default` is the default when the ALSA backend is present, and `null` when it is
not. Volume is applied in software, so it works the same through PipeWire's ALSA
plugin as through bare hardware; the ALSA hardware mixer is a follow-up.

### Flags, and what they refuse

The flags follow squeezelite's: `-o` output device, `-l` list devices, `-n` name,
`-s` server, `-z` daemonize, `-P` pidfile, `-d`/`-f` logging. Run `--help` for the
current state of each — several are scaffolding whose real behaviour is still to come.

Everything is validated before the daemon starts, and a bad value exits `1` with a
single `error:` line naming it rather than falling back to a default:

```console
$ sendspin-cli -s music.local:abc
error: -s 'music.local:abc': 'abc' is not a port number (expected 1-65535)
```

That is a deliberate change from warn-and-continue. `-s` used to warn about a
malformed port and dial the default anyway; a player quietly talking to the wrong
endpoint is harder to diagnose than one that refuses to start. `-s` takes
`<host>[:<port>]` — filling in `8927`, the port a Sendspin *server* listens on —
or a full `ws://`/`wss://` URL. An IPv6 literal must be bracketed (`[::1]:8927`),
since an unbracketed one cannot be told from a host with a port.

## Tests

```bash
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

GoogleTest is fetched at configure time and pinned to a tag. The suite is built by
default only when this is the top-level project, so vendoring `sendspin-cli` into
another build does not pay for it; `-DSENDSPIN_CLI_BUILD_TESTS=OFF` turns it off
outright.

## Roadmap

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the epic breakdown and the child
tasks that build out audio backends, discovery, daemonization, the local control
channel, Docker packaging, and more.

## Upstream

- Sendspin protocol library: https://github.com/Sendspin/sendspin-cpp
- Control-scheme inspiration: https://github.com/ralph-irving/squeezelite

## License

Licensed under the [Apache License 2.0](LICENSE), matching sendspin-cpp.
