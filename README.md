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
- **Audio out:** an `AudioSink` seam — ALSA as the default Linux/Docker backend,
  a null/stdout sink for device-less containers, and PortAudio for cross-platform
  development.
- **Binary:** `sendspin-cli`.

## Build

Requires a C++17 compiler and CMake. Dependencies (sendspin-cpp and its
transitive deps) are fetched automatically.

```bash
cmake -B build
cmake --build build
./build/sendspin-cli --help
```

## Roadmap

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the epic breakdown and the child
tasks that build out audio backends, discovery, daemonization, the local control
channel, Docker packaging, and more.

## Upstream

- Sendspin protocol library: https://github.com/Sendspin/sendspin-cpp
- Control-scheme inspiration: https://github.com/ralph-irving/squeezelite

## License

Licensed under the [Apache License 2.0](LICENSE), matching sendspin-cpp.
