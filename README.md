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
  with `snd_pcm_delay()`-based sync feedback), PortAudio (the cross-platform one,
  and the only way to make noise on macOS), and a null/stdout sink for
  device-less containers.
- **Binary:** `sendspin-cli`.

## Build

Requires **CMake ≥ 3.16**, a **C++20** compiler, and network access on the first
configure. sendspin-cpp is pulled in with `FetchContent` at a pinned tag, and it
fetches its own dependencies (ArduinoJson, micro-flac, micro-opus, IXWebSocket) in
turn.

**The audio backends and mDNS are all optional and auto-detected.** Whichever of
libasound and libportaudio is present gets compiled in; where neither is, the build
falls back to the device-less sinks, so a sound-card-less container still builds and
runs. mDNS comes from `dns_sd.h` — Bonjour on macOS, where it needs nothing
installed, and `libavahi-compat-libdnssd` on Linux. The configure output says what
you got:

```
-- sendspin-cli audio backends: null, stdout, alsa, portaudio
-- sendspin-cli mDNS: dns_sd (/usr/lib/x86_64-linux-gnu/libdns_sd.so)
```

```bash
sudo dnf install alsa-lib-devel portaudio-devel avahi-compat-libdns_sd-devel  # Fedora / RHEL
sudo apt install libasound2-dev portaudio19-dev libavahi-compat-libdnssd-dev  # Debian / Ubuntu
brew install portaudio pkgconf                    # macOS (no ALSA, and Bonjour is built in)
```

ALSA is found with CMake's own `find_package(ALSA)`; PortAudio ships no CMake
config module, so it is found with `pkg-config` (`portaudio-2.0`) — on macOS that
is also the only thing that knows the CoreAudio frameworks have to be linked too.
A host without `pkg-config` gets its own configure message saying so.

Pass `-DSENDSPIN_CLI_WITH_ALSA=OFF`, `-DSENDSPIN_CLI_WITH_PORTAUDIO=OFF` or
`-DSENDSPIN_CLI_WITH_MDNS=OFF` to leave one out even where its library is available.

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

```bash
# Advertise over mDNS and wait to be found — the usual way to run it
./build/sendspin-cli -n living-room

# Dial a server explicitly, retrying until it answers
./build/sendspin-cli -s 192.168.1.10

# Discover a server and dial that instead
./build/sendspin-cli -s mdns:

# Debug logging, in the foreground
./build/sendspin-cli -d debug
```

### The two connection modes

The protocol has two, and they are **mutually exclusive** — the spec's rule, not a
preference here:

> Do not advertise `_sendspin._tcp` if the client plans to initiate the connection.

**Server-initiated (the default).** `sendspin-cli` advertises `_sendspin._tcp` on
its `--port`, with the required TXT `path=/sendspin` and a TXT `name`, and waits
for a server to dial it. Nothing needs configuring on either side.

```console
$ sendspin-cli -n living-room
sendspin-cli 0.1.0 listening on port 8928 as "living-room" (output: default)
mDNS: advertising _sendspin._tcp. as "living-room" on port 8928 (path /sendspin)
```

The instance name comes from `--mdns-name`, falling back to `-n`, falling back to
this host's name. The name that is *logged* is the one that actually registered —
the mDNS daemon renames on a collision, so two players called `living-room` will
not fight over it. `--no-mdns` turns the advertisement off without switching modes.

**Client-initiated (`-s`).** Any `-s` makes this player the one dialling, so the
advertisement is suppressed and the run says so:

```console
$ sendspin-cli -s 192.168.1.10
Not advertising _sendspin._tcp: -s makes this player the one initiating the connection,
and the Sendspin spec forbids advertising while it is
```

There is deliberately no flag that turns both on together. `--mdns-name` alongside
`-s` warns that it is unused rather than failing — it names an advertisement the
mode has already ruled out.

`-s` takes an address, or `mdns:` to go and find one:

```bash
./build/sendspin-cli -s 192.168.1.10          # a host, port 8927 assumed
./build/sendspin-cli -s music.local:9000      # host and port
./build/sendspin-cli -s ws://music:9000/sendspin   # a full URL
./build/sendspin-cli -s "[2001:db8::1]:8927"  # IPv6 must be bracketed
./build/sendspin-cli -s mdns:                 # discover any server
./build/sendspin-cli -s "mdns:Music Assistant"  # ...or one by its advertised name
```

`mdns:` is reserved **before the first colon only**, the same way `-o` reads
`<backend>:<device>`, so every address form still works — `hifi:8927` is a host
and a port, and a bare `-s mdns` is still a host called `mdns`. Discovery is not a
bare `-s` because `-s` takes a required argument, so a bare one would swallow the
next word.

Discovery browses `_sendspin-server._tcp`, resolves each instance to an address,
and dials `ws://<addr>:<port><path>` built from the server's own TXT `path`. An
instance advertising no `path`, or one not starting with `/`, is skipped and said
so at `debug`. A non-link-local IPv4 wins over IPv6; an IPv6-only server yields a
bracketed URL. The browse stays open, so a server that appears later is picked up
without a restart.

Among several servers, the one this player last completed a handshake with wins,
and otherwise the first to resolve. That works because the `_sendspin-server._tcp`
instance label *is* the protocol `server_id`, so the preference is decidable before
anything is dialled. It is remembered in `$XDG_STATE_HOME/sendspin-cli/last-server`
(falling back to `~/.local/state/...`); a process with neither variable set, or an
unwritable directory, simply does not remember and says so at `debug`.

```console
$ sendspin-cli -s mdns:
mDNS: found server "OraobU4l…" (name: Music Assistant) at ws://10.0.2.8:8927/sendspin
mDNS: found server "oGsvjWZw…" (name: Music Assistant) at ws://10.0.1.6:8927/sendspin
Connecting to ws://10.0.1.6:8927/sendspin (server "oGsvjWZw…") -- chosen because it is
the last server whose handshake completed
```

**Retries are part of the mode.** In this direction nothing else re-establishes the
link — per the spec, "servers cannot reclaim clients by reconnecting" — so `-s`
retries on its own: 1 s, doubling to a 30 s ceiling, reset when a handshake
completes and restarted from the floor when a connection drops. The ceiling matches
the library's own 30 s establish timeout. Redials are paced from the last dial
rather than from "not connected yet", because an attempt in flight reads as not
connected and redialling over it would cancel it.

### When the build has no mDNS

mDNS is optional and auto-detected, like the audio backends. Without it the player
still builds, starts and plays — it just has to be told where its server is:

```console
$ cmake -B build
-- sendspin-cli mDNS: none

$ ./build/sendspin-cli
This build has no mDNS support, so it cannot be discovered: point a server at
ws://<this-host>:8928/sendspin, or dial one with -s. See docs/ROADMAP.md.

$ ./build/sendspin-cli -s mdns:
error: -s 'mdns:': this build has no mDNS support, so it cannot discover a server.
Rebuild with dns_sd.h available (libavahi-compat-libdnssd-dev on Debian/Ubuntu,
avahi-compat-libdns_sd-devel on Fedora), or give -s an address.
```

Discovery is refused at *parse* time rather than starting and quietly finding
nothing, which is how `-o` already treats a backend the build lacks.

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

On a host with PortAudio, `-l` also lists its output devices — index, name, host
API, output channel count and default rate, with the system default marked and
input-only devices left out:

```
  idx  name                                   host API     out ch  default rate
    0  Odyssey G95NC                          Core Audio    2 ch   48000 Hz
      rates:    22050 32000 44100 48000 88200 96000 176400 192000
      formats:  paInt8 paInt16 paInt24 paInt32
      channels: 1 2
    2  MacBook Pro Speakers                   Core Audio    2 ch   48000 Hz  (system default)
      rates:    22050 32000 44100 48000 88200 96000 176400 192000
      formats:  paInt8 paInt16 paInt24 paInt32
      channels: 1 2
```

Both backends report the same three lines, asked the same way a stream would ask —
only the format spelling is each backend's own. The rate on a PortAudio device's
own line is its *default*; the rates under it are what it will take.

`-o` reads its argument in three steps, in this order:

1. a backend name on its own — `null` discards audio, `stdout` (or `-`) writes raw
   interleaved PCM to standard output, and `portaudio` follows whatever this host's
   default output currently is. These mean the same thing on every build, even
   where ALSA ships a PCM of the same name;
2. `<backend>:<device>`, split on the **first** colon, where the backend is one of
   the names the build reports (`null, stdout, alsa, portaudio`). The split is on
   the first colon because ALSA device names carry their own, so `-o alsa:hw:2,0`
   is the `alsa` backend playing `hw:2,0`;
3. anything else is an ALSA PCM name, which is squeezelite's model — so `-o hw:2,0`
   and `-o default` keep working with no prefix at all. This step is deliberately
   ALSA-only: PortAudio *does* enumerate its devices, so letting a bare name reach
   one would make the same command line mean different things per host.

```bash
./build/sendspin-cli -l                 # what this host can play through
./build/sendspin-cli -o default         # follow the system config (PipeWire/Pulse)
./build/sendspin-cli -o hw:2,0          # a card directly, bypassing the sound server
./build/sendspin-cli -o plughw:2,0      # same, letting ALSA convert rate/format
./build/sendspin-cli -o alsa:hw:2,0     # the same card, naming the backend explicitly
./build/sendspin-cli -o portaudio       # this host's default output (what macOS wants)
./build/sendspin-cli -o portaudio:2     # a PortAudio device by index, as -l prints it
./build/sendspin-cli -o "portaudio:MacBook Pro Speakers"   # ...or by name
./build/sendspin-cli -o null            # no sound card needed at all
```

A PortAudio device name is matched in full and case-insensitively. The **name is
the form worth writing down**: PortAudio numbers devices as it walks each host
API, so an index shifts as devices come and go. A name matching more than one
device is refused, naming the candidates, rather than guessed at — two host APIs
can offer the same card under the same name.

The default `-o` is `default` where the ALSA backend is present, `portaudio` where
only that one is, and `null` where neither is. ALSA wins over PortAudio wherever
both are built, because on Linux PortAudio is itself a layer over ALSA and going
direct is one layer fewer.

Volume is applied in software on both backends, sharing one Q32 fixed-point
implementation, so a stream sounds the same either way — and works the same
through PipeWire's ALSA plugin as through bare hardware. The ALSA hardware mixer
is a follow-up. The one audible difference: PortAudio scales in its audio
callback, so a volume change also reaches audio already buffered, where ALSA
scales on the way in and so only affects what has not been written yet.

### Buffering, and what gets advertised

`--buffer-ms <ms>` (10–2000, default 100) is how much audio the output backend
keeps queued — one figure for every backend rather than squeezelite's ALSA-only
`-a`, whose `<b>:<p>:<f>:<m>` grammar would mean something different per backend
here. ALSA divides it into five periods; PortAudio makes it the ring size, where
a figure smaller than one device buffer is raised to the floor and says so at
`debug`. A device-less sink (`null`, `stdout`) has nothing to size and ignores it.

The formats advertised to the server are **derived from the device**, not fixed:
`sendspin-cli` probes what `-o` selected — the same probe `-l` prints — and crosses
it with what each codec can carry. FLAC and PCM get every rate and depth the device
takes; OPUS gets 48 kHz / 16-bit only, because the decoder writes `int16_t`. The
result is logged at startup, and a device that cannot be probed is advertised
permissively rather than not at all.

Two limits worth knowing, since both make that list a snapshot:

- `-o portaudio` re-resolves the host's default output at every stream, so the
  advertisement describes whichever device was default when the player started.
- ALSA's `default` is usually PipeWire's plugin, so the probe describes what the
  *plugin* accepts, not the card behind it.

Either way, a format the device then refuses is reported loudly — naming the
device, the format, and the fact that the stream's audio is being discarded —
rather than leaving a player that looks healthy and plays nothing.

### Flags, and what they refuse

The flags follow squeezelite's: `-o` output device, `-l` list devices, `-n` name,
`-s` server, `-z` daemonize, `-P` pidfile, `-d`/`-f` logging. Four are long-only
because they are not squeezelite's: `--port`, the port this player serves on,
`--buffer-ms`, and the two mDNS flags `--no-mdns` and `--mdns-name`. Run `--help`
for the current state of each — several are scaffolding whose real behaviour is
still to come.

Everything is validated before the daemon starts, and a bad value exits `1` with a
single `error:` line naming it rather than falling back to a default:

```console
$ sendspin-cli -s music.local:abc
error: -s 'music.local:abc': 'abc' is not a port number (expected 1-65535)

$ sendspin-cli -o portaudio:99
error: -o portaudio:99: no device at that index -- indices run 0-2 here, and -l lists the ones -o can reach

$ sendspin-cli --buffer-ms 0
error: invalid --buffer-ms '0' -- expected 10-2000
```

That is a deliberate change from warn-and-continue. `-s` used to warn about a
malformed port and dial the default anyway; a player quietly talking to the wrong
endpoint is harder to diagnose than one that refuses to start. `-s` takes
`<host>[:<port>]` — filling in `8927`, the port a Sendspin *server* listens on —
or a full `ws://`/`wss://` URL, or `mdns:[<name>]` to discover one. An IPv6 literal
must be bracketed (`[::1]:8927`), since an unbracketed one cannot be told from a
host with a port.

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
