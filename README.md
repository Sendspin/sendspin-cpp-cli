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
configures identity, audio output, discovery, logging, and daemonization. It also
listens on a **local control socket**, so `sendspin-cli pause` on the player's own
host drives it too — the deliberate addition to the squeezelite model.

- **Library:** sendspin-cpp (`player`, `metadata` and `controller` roles; FLAC /
  Opus / PCM), pulled via CMake `FetchContent`.
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
sudo dnf install pkgconf alsa-lib-devel portaudio-devel avahi-compat-libdns_sd-devel  # Fedora / RHEL
sudo apt install pkg-config libasound2-dev portaudio19-dev libavahi-compat-libdnssd-dev  # Debian / Ubuntu
brew install portaudio pkgconf                    # macOS (no ALSA, and Bonjour is built in)
```

ALSA is found with CMake's own `find_package(ALSA)`; PortAudio ships no CMake
config module, so it is found with `pkg-config` (`portaudio-2.0`) — on macOS that
is also the only thing that knows the CoreAudio frameworks have to be linked too.
A host without `pkg-config` gets its own configure message saying so.

Pass `-DSENDSPIN_CLI_WITH_ALSA=OFF`, `-DSENDSPIN_CLI_WITH_PORTAUDIO=OFF` or
`-DSENDSPIN_CLI_WITH_MDNS=OFF` to leave one out even where its library is available.

`-DSENDSPIN_CLI_WERROR=ON` makes warnings fatal, for sendspin-cli's own three
targets and nothing else — the `sendspin` and GoogleTest trees fetched at
configure time are not ours to keep clean. It is off by default so that a fresh
diagnostic from a newer compiler cannot block a contributor who did not cause it;
CI turns it on, which is where the line is actually held.

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

# Debug logging, in the foreground. Every line is "<L> <tag>: <message>", ours
# and the library's alike, so one grep reaches either half
./build/sendspin-cli -d debug 2>&1 | grep ' mdns:'

# As a daemon: detached, with a locked pidfile and a logfile
./build/sendspin-cli -z -P /run/sendspin-cli.pid -f /var/log/sendspin-cli.log

# ...and drive that player from its own host
./build/sendspin-cli status
./build/sendspin-cli pause
./build/sendspin-cli vol 40
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

### Running as a daemon

`-z` forks once, `setsid()`s away from the controlling terminal, `chdir()`s to `/`
so it does not pin a mount point, and points stdin and stdout at `/dev/null`. The
parent exits `0` immediately, so the shell comes straight back.

```bash
sendspin-cli -z -P /run/sendspin-cli.pid -f /var/log/sendspin-cli.log
```

**Which failures reach the terminal, and which only reach the log.** Everything
cheap and fallible happens *before* the fork, so it can still be reported to the
shell that is watching: the flag parse, opening the `-f` logfile, and a probe of
the `-P` pidfile. Those exit `1` at the terminal exactly as a foreground run does.

Everything after the fork — the output device, the WebSocket server, mDNS — can
only report into the log, because the terminal has already been given a `0`. A
device that is busy or absent therefore looks like a clean start and then says why
in the logfile:

```console
$ sendspin-cli -z -f /tmp/s.log -o portaudio:999 ; echo $?
0
$ cat /tmp/s.log
2026-08-10T21:41:48Z E audio: -o portaudio:999: no device at that index -- indices run 0-2 here, and -l lists the ones -o can reach
```

A fatal error like that one is stamped and tagged like every other line, but it is **not**
gated by `-d`: `-d none` means "do not narrate", not "exit without saying why".

That boundary is why `-z` **without** `-f` warns. What goes to `/dev/null` is not
just the running commentary: it is the answer to "why did my daemon not come
up?" — and the non-zero exit is unreachable too, because the parent already
returned `0`. A supervisor sees a clean start followed by nothing at all. It
still starts, since a supervisor that captures nothing is a legitimate way to
run it, but that silence is indistinguishable from a crash, which is why the
warning exists. `-z` with `-o stdout` is refused outright rather than warned
about, since a daemon's stdout *is* `/dev/null` and the PCM would be discarded.

Under `-z`, a relative `-P` or `-f` path is resolved against the directory you ran
it from, before the `chdir()`. A foreground run leaves relative paths exactly as
typed.

**The pidfile is a lock, not just a file.** `-P` holds it under an exclusive
`flock()` for the process's whole life, so a second instance is refused by name:

```console
$ sendspin-cli -z -P /run/sendspin-cli.pid -f /var/log/sendspin-cli.log
error: another sendspin-cli is already running -- it holds the lock on /run/sendspin-cli.pid
```

That also makes stale files a non-issue. A process killed with `SIGKILL` has its
descriptor closed by the kernel, so the lock is simply gone: the next start
truncates the leftover file and writes its own pid, with no cleanup step and no
liveness probe. Nothing ever parses the old contents, which is what stops a
recycled pid being read as a live instance. The file is removed on every clean
exit path.

Two things to know about *where* the pidfile goes. Keep it on a **local
filesystem** — `flock` is emulated over NFS and is not dependable over SMB — which
in practice means `/run` (or `/var/run`). And `/run` is `root`-owned, so a daemon
started as a normal user wants a path it can write, such as
`$XDG_RUNTIME_DIR/sendspin-cli.pid`.

**systemd.** A forking daemon wants `Type=forking` with `PIDFile=` pointing at the
same path as `-P`; the foreground default suits `Type=simple`, which is usually the
better choice under a supervisor that already captures stderr. No unit file ships
yet — that is [`docs/ROADMAP.md`](docs/ROADMAP.md) item 10.

### The local control channel

The player listens on a **Unix socket**, and the same binary is its own client:

```console
$ sendspin-cli status
name: living-room
server: Music Assistant (connected)
state: playing
stream: receiving
track: Nils Frahm - Says
position: 2:05 / 9:03
group volume: 55
player volume: 80
output: default (48000 Hz / 2 ch / 16-bit)

$ sendspin-cli pause
$ sendspin-cli vol 40
$ sendspin-cli seek-rel -30000
```

Every transport verb the protocol has, one subcommand each — `status`, `play`,
`pause`, `stop`, `next`, `prev`, `vol`, `mute`, `seek`, `seek-rel`, `repeat`,
`shuffle`, `switch`. `--help` lists them with their arguments.

`player volume` is the gain **this box's output** is applying, and it says
`(default; no server has set it)` until a server sends a volume command — because
until one does, the sink runs at full while the library's own stored volume reads
0. Those disagree, and the qualifier is how you tell "nobody has set this" from a
server that deliberately chose full output.

Two `status` lines are worth reading together. `state` is the **group's** transport
state, from the metadata `playback_speed`, and reads `unknown` rather than guessing
when the server has sent no progress. `stream` is whether audio is arriving at
**this** endpoint, which is a different fact — a player dropped from the group loses
it while the group plays on. A `stream: receiving` line with no format after the
device name is the case where the device *refused* the stream's format and its audio
is being discarded; the log says so loudly at the same moment.

**Three of them are easy to misread, so:**

- **`vol` is the *group* volume**, not this box's output level. It goes out as a
  `controller@v1` command and the server spreads it across every player in the
  group, clamping per player. That is why `status` prints `group volume` and
  `player volume` as two named lines rather than one ambiguous `volume:` — a
  squeezelite refugee will expect `vol 50` to move *this* box, and it does not.
- **`switch` is not a source selector.** Per the spec's switch cycle it re-homes
  this client through the groups available to it. It sits next to `play` and
  `pause` and means something quite different.
- **`seek-rel` takes a signed offset** and is bounded only by `int32_t`. `seek`
  is absolute, non-negative, and refused past the `seek_max_ms` the server
  publishes — which is absent for a live stream, where nothing bounds it.

**Where the socket is.** `$XDG_RUNTIME_DIR/sendspin-cli-<port>.sock`, mode `0600`,
where `<port>` is `--port` — or, where that variable is unset and the platform has a
private directory of its own, there instead (see below). The port is in the name so
two players on one host each get their own — and it means a player on a non-default
`--port` has its socket somewhere else, so **a subcommand needs the same `--port`**,
or an explicit `--control-socket`:

```bash
sendspin-cli --port 9000 &            # this player's socket carries 9000
sendspin-cli status --port 9000       # ...so its subcommands need it too
sendspin-cli status --control-socket /run/user/1000/sendspin-cli-9000.sock  # or name it
```

**On macOS the default still works, because launchd sets no `$XDG_RUNTIME_DIR`.**
There the path comes from `confstr(_CS_DARWIN_USER_TEMP_DIR)` — the per-user
directory under `/var/folders` that launchd already gives every session:

```console
$ sendspin-cli -n living-room &
I control: Listening on /var/folders/y5/9jvkfq…/T/sendspin-cli-8928.sock
$ sendspin-cli status
name: living-room
…
```

That is deliberately **not** `$TMPDIR`, which usually names the same directory:
`confstr()` reads nothing from the environment, so unlike `$TMPDIR` it cannot be
pointed at a directory someone else can write. And it is verified rather than
trusted — it must be a directory, owned by this user, with no group- or other-write
bit — so a platform that answered with something unsafe gets refused, not used.

**There is no `/tmp` fallback anywhere, and that is the point.** `$XDG_RUNTIME_DIR`
and the macOS directory are both per-user and `0700`, which is what makes the socket
unreachable by other local accounts; `/tmp` is world-writable, and a socket there
would let any local user pause your music and `switch` this endpoint out of its
group. Linux enforces socket permissions on `connect()`, but macOS and the BSDs
historically do not — so the private parent directory is doing real work, not just
belt-and-braces. `$XDG_RUNTIME_DIR` wins wherever it is set, on every platform.

With neither available — a **systemd *system* unit** on Linux, which gets no
`$XDG_RUNTIME_DIR` (a user unit does) — there is no control socket. That is **not
fatal**: a player without a control channel is still a player, exactly as one
without an mDNS advertisement is, and it warns once, naming the fix:

```console
$ env -u XDG_RUNTIME_DIR sendspin-cli          # on Linux
W control: No control socket: $XDG_RUNTIME_DIR is not set, so there is no user-private
directory to put a control socket in. Give --control-socket <path> to choose one, or
--no-control to stop asking
I cli: sendspin-cli 0.1.0 listening on port 8928 as "living-room" (output: default, ...)
```

For a system unit, pair systemd's own `RuntimeDirectory=` with `--control-socket`;
`--no-control` turns the channel off and silences the warning if the player is only
ever driven by its server.

Two caveats on the macOS path. The OS prunes `/var/folders` on a schedule, so a
very long-lived player could in principle have its socket unlinked from under it,
which looks like subcommands reporting no daemon until it restarts.
`$XDG_RUNTIME_DIR` on Linux is tmpfs cleared at logout, so it is the same class of
impermanence. And the directory is **per-user**, which is the point — so a player
run as a launchd *system* daemon puts its socket in `root`'s, where your own
`sendspin-cli status` will not find it. Both want an explicit `--control-socket`,
exactly as a systemd system unit does.

**A second instance is refused**, in the same words `-P` uses, and refused before
it opens the sound card or its port:

```console
$ sendspin-cli --control-socket /run/user/1000/s.sock
E control: another sendspin-cli is already running -- it holds the lock on
/run/user/1000/s.sock.lock
```

That comes from an exclusive `flock()` on a sibling `<path>.lock`, held for the
process's life, with the socket unlinked and rebound underneath it — the same
`lock_file()` helper `-P` uses, which is what makes the two refusals identically
worded rather than coincidentally so. The lock is what makes "stale" and "in use"
different answers: a player killed with `SIGKILL` has its descriptor closed by the
kernel, so its leftover socket file has no lock and is simply taken over on the
next start — no cleanup step. `unlink()`-then-`bind()` on its own would race a
*live* player's socket away, and connecting to probe is a TOCTOU. (The lock file
itself is left behind; it holds nothing, and removing it would reintroduce a race.)

Under `-z` the refusal still reaches the **terminal**, not just the log. The socket
has to be bound after the fork — it is one of the resources that invariant exists
for — so the parent probes the lock first and exits `1` at the shell, exactly as a
locked `-P` does. The child's own acquire is still the authoritative one.

**Exit status is the interface for scripts.** The three ways a command can fail to
land are three different statuses, because they need three different actions:

| Status | Means |
|---|---|
| `0` | sent, or `status` printed |
| `1` | the command line did not parse (`vol 500`) |
| `2` | the player refused the argument (a `seek` past `seek_max_ms`) |
| `3` | nothing is listening on that socket — no player, or the wrong `--port` |
| `4` | the player is up but has **no server connection** |
| `5` | the server does not offer that command (`supported_commands`) |
| `6` | the exchange broke down |

`4` and `5` are kept apart on purpose. A dropped connection *empties*
`supported_commands`, so the naive check answers "pause is not supported" when the
truth is that nothing is connected — sending you to read your server's
capabilities instead of its connection. `status` is never refused by any of them:
it is answered out of the player's own state, which is exactly when a
disconnected player is worth reading.

**No thread, and one tick of latency.** The socket is polled from the main loop
alongside mDNS, so a request round-trips in up to `LOOP_INTERVAL_MS` (10 ms). That
is the trade, and it is the right way round: `send_command()` reaches
`ConnectionManager::current()`, which is documented main-thread-only, and reading
the controller state hands back a reference to a vector the main loop
move-assigns from inside `client.loop()`. A reader thread would be a data race in
both directions.

**The wire format**, if you want to drive it without this binary: connect, send
one line (`vol 50\n`), read until the player closes. The first line back is `ok`
or `error <kind>: <reason>`; a `status` payload follows the `ok`. One command per
connection.

```console
$ printf 'status\n' | socat - UNIX-CONNECT:/run/user/1000/sendspin-cli-8928.sock
ok
name: living-room
...
```

### Logging

Every *log* line carries a level letter and a subsystem tag:

```
I cli: sendspin-cli 0.1.0 listening on port 8928 as "living-room" (output: default, mDNS: dns_sd (Bonjour))
I mdns: advertising _sendspin._tcp as "living-room" on port 8928 (path /sendspin)
I sendspin.ws_server: Starting server on port: 8928 (max connections: 4)
```

The third line is the library's. That is the point of the format: it is the shape
sendspin-cpp's own `SS_LOG*` macros already emit, so `grep 'I mdns:'` and
`grep 'I sendspin.ws_server:'` both work on the same file. Ours are `cli`, `audio`,
`mdns`, `discovery`, `outbound`, `player`, `metadata` and `control`; the library's
are all `sendspin.<subsystem>`. `audio` lines then name their own backend, since the tag
says which subsystem but not which device is talking: `I audio: alsa: 'hw:1,0'
closed`.

Fatal startup errors are in this format too, at `E`, but they are deliberately
**not** gated by `-d` — see the `-z` example above. The one line that explains why a
daemon never came up has to be both greppable and impossible to switch off. Two
kinds of diagnostic are the exception and stay plain `error: …` lines: the flag
parser's, and the pre-fork pidfile probe's. Both answer a command line rather than
recording a run, and both are printed before there is a log to write them to —
which is why a `-P` conflict reads `error: …` under `-z` and `E cli: …` in the
foreground.

`-d` sets one level for this player and the library together, which is deliberate —
a single flag turns up everything about one run. It accepts squeezelite's
`-d <category>=<level>` shape, but **the category is ignored** and says so: the
library gates its lines on one global integer with no sink or filter hook, so there
is no honest way to raise the level for one category only. The per-line tag plus
`grep` is the filtering that does work, and it works on the library's lines too.

`-f <path>` sends the log to a file instead of stderr, appending, and stamps every
line with a UTC timestamp:

```
2026-08-10T21:37:18Z I cli: sendspin-cli 0.1.0 listening on port 18931 as "living-room" (output: null, mDNS: dns_sd (Bonjour))
```

Only a `-f` file is stamped. A foreground run under systemd or Docker already gets
a timestamp from journald or the container runtime, and a second one would only be
noise. The library's own lines are *not* stamped, for the same reason its category
cannot be filtered — nothing in this process gets to reformat them.

**Rotation is `logrotate`'s and `newsyslog`'s job, not this daemon's.** `SIGHUP`
reopens the `-f` path, which is the whole handshake those tools need:

```
/var/log/sendspin-cli.log {
    daily
    rotate 7
    compress
    postrotate
        kill -HUP $(cat /run/sendspin-cli.pid)
    endscript
}
```

The reopen happens on the main loop rather than in the signal handler, because it
flushes the old stream and then logs the result, and neither of those is
async-signal-safe. The handler is installed **only** with `-f`:
without it, `SIGHUP` keeps its default disposition and terminates the process,
which is what a foreground run whose terminal has just closed should do.

### Flags, and what they refuse

The flags follow squeezelite's: `-o` output device, `-l` list devices, `-n` name,
`-s` server, `-z` daemonize, `-P` pidfile, `-d`/`-f` logging. Six are long-only
because they are not squeezelite's: `--port`, the port this player serves on,
`--buffer-ms`, the two mDNS flags `--no-mdns` and `--mdns-name`, and the two
control-socket flags `--control-socket` and `--no-control`. Run `--help` for the
current state of each — a few still point at
[`docs/ROADMAP.md`](docs/ROADMAP.md) for behaviour that is not built yet.

A **subcommand comes first**, before any flag: `sendspin-cli vol 50 --port 9000`,
not `sendspin-cli --port 9000 vol 50`. That is not getopt permutation showing
through — argv[1] is split off *before* `getopt_long()` runs, because getopt's
handling of a positional argument differs between glibc and the BSDs, and because
`seek-rel -5000` is indistinguishable from a flag cluster to it. A subcommand in
the wrong place says so rather than being called junk.

Everything the flags can settle is validated before anything is opened, and a bad
value exits `1` with a single line naming it rather than falling back to a default:

```console
$ sendspin-cli -s music.local:abc
error: -s 'music.local:abc': 'abc' is not a port number (expected 1-65535)

$ sendspin-cli --buffer-ms 0
error: invalid --buffer-ms '0' -- expected 10-2000
```

What a flag *cannot* settle on its own is whether the thing it names will open. That
is a startup failure rather than a bad value, so it comes out in the log's format —
which is what puts it in the logfile under `-z`, where it is the only record of why
the daemon never came up:

```console
$ sendspin-cli -o portaudio:99
E audio: -o portaudio:99: no device at that index -- indices run 0-2 here, and -l lists the ones -o can reach
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

The suite is pure — nothing in `tests/` opens an audio device, a socket or the
mDNS daemon, which is what keeps `ctest` runnable anywhere at all. What that
leaves out is everything needing a real process, and that is a script instead:

```bash
scripts/smoke_test.sh build/sendspin-cli
```

It checks that the binary runs, comes up on its port, forks under `-z`, refuses a
second instance holding the same `-P`, survives an mDNS daemon it cannot reach,
and exits `0` on `SIGTERM` — plus the whole of the control socket, which needs two
processes by definition: that it appears at the default path as `0600`, that
`status` round-trips, that `--no-control` binds nothing, that a stale socket is
taken over after a `SIGKILL`, that it is gone after `SIGTERM`, and that a second
instance on the same socket is refused. CI runs it on every platform leg; run it
yourself against any build.

## CI

Every push and pull request builds on `ubuntu-24.04`, `ubuntu-24.04-arm` and
`macos-14`, plus a fourth leg configured `-DSENDSPIN_CLI_WITH_MDNS=OFF` — which
compiles `src/mdns_null.cpp` in place of `src/mdns_dnssd.cpp`, so that
configuration is built rather than assumed. Every leg builds with
`-DSENDSPIN_CLI_WERROR=ON` and runs the unit suite, and each asserts from its own
configure output that it found the backends it expects: a missing `-dev` package
does not fail a configure, so without that check the matrix would happily go green
on a deaf, undiscoverable binary.

To try a commit without building it, open its run under the repository's Actions
tab and take `sendspin-cli-<version>-<os>-<arch>` from the run summary. Inside is
a tarball holding the binary, this README, the licence, and a `BUILD-INFO.txt`
naming the runtime packages it needs. These are build outputs kept for 14 days
rather than an installation — `install()` rules, a systemd unit and distribution
packages are [`docs/ROADMAP.md`](docs/ROADMAP.md) item 10.

### macOS, and Gatekeeper

Unpack the tarball from a terminal rather than in Finder:

```bash
tar -xzf sendspin-cli-0.1.0-macos-arm64.tar.gz
./sendspin-cli-0.1.0-macos-arm64/sendspin-cli --version
```

That is not fussiness. These binaries are **ad-hoc signed** — the minimum an
arm64 Mach-O needs to execute at all, applied by the linker — so they carry no
developer identity and `spctl` rejects them. What decides whether you notice is
the quarantine flag, and `tar` does not propagate it where Finder's Archive
Utility does. If you did unpack in Finder, or macOS refuses it anyway:

```bash
xattr -d com.apple.quarantine ./sendspin-cli
```

A Developer ID signature and notarization are item 10's, together with the
`.pkg` that lets the notarization be *stapled* — `xcrun stapler` refuses a bare
executable, so signing alone would still leave an offline Mac asking Apple.

## Roadmap

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the epic breakdown and the child
tasks that build out audio backends, discovery, daemonization, the local control
channel, a config file, Docker packaging, and more.

## Upstream

- Sendspin protocol library: https://github.com/Sendspin/sendspin-cpp
- Control-scheme inspiration: https://github.com/ralph-irving/squeezelite

## License

Licensed under the [Apache License 2.0](LICENSE), matching sendspin-cpp.
