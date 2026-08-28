# sendspin-cpp-cli

A headless **CLI / daemon audio player** built on the
[sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) synchronized
audio-streaming library, taking its command-line and control ergonomics from
[squeezelite](https://github.com/ralph-irving/squeezelite).

> **Status: early scaffold.** This repository is being stood up as an *epic*.
> The initial task brings up the build and boots a sendspin client; feature work
> is tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md).

**New here?** The [wiki](https://github.com/Sendspin/sendspin-cpp-cli/wiki) is the
task-shaped version of this file — installing, a Raspberry Pi walkthrough, troubleshooting —
and on Linux [`scripts/get_started_linux.sh`](scripts/get_started_linux.sh) does the install
in one command. Those pages are authored in [`docs/wiki/`](docs/wiki) and mirrored to the
wiki tab on every push to `main`.

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
libasound, libportaudio, libpulse and libpipewire is present gets compiled in;
where none is, the build falls back to the device-less sinks, so a
sound-card-less container still builds and runs. mDNS comes from `dns_sd.h` —
Bonjour on macOS, where it needs nothing installed, and
`libavahi-compat-libdnssd` on Linux. The configure output says what you got:

```
-- sendspin-cli audio backends: null, stdout, alsa, portaudio, pulse, pipewire
-- sendspin-cli mDNS: dns_sd (/usr/lib/x86_64-linux-gnu/libdns_sd.so)
```

```bash
sudo dnf install pkgconf alsa-lib-devel portaudio-devel pulseaudio-libs-devel pipewire-devel avahi-compat-libdns_sd-devel  # Fedora / RHEL
sudo apt install pkg-config libasound2-dev portaudio19-dev libpulse-dev libpipewire-0.3-dev libavahi-compat-libdnssd-dev  # Debian / Ubuntu
brew install portaudio pkgconf                    # macOS (no ALSA, and Bonjour is built in)
```

ALSA is found with CMake's own `find_package(ALSA)`; the other three ship no CMake
config module, so they are found with `pkg-config` (`portaudio-2.0`, `libpulse`,
`libpipewire-0.3` ≥ 0.3.64) — on macOS that is also the only thing that knows the
CoreAudio frameworks have to be linked too. A host without `pkg-config` gets its
own configure message saying so.

The PipeWire minimum is a real one rather than caution: `PW_KEY_TARGET_OBJECT`,
which is how `-o pipewire:<node>` names a node, arrived in 0.3.64, and an older
libpipewire would build and then quietly ignore the node.

Pass `-DSENDSPIN_CLI_WITH_ALSA=OFF`, `-DSENDSPIN_CLI_WITH_PORTAUDIO=OFF`,
`-DSENDSPIN_CLI_WITH_PULSE=OFF`, `-DSENDSPIN_CLI_WITH_PIPEWIRE=OFF` or
`-DSENDSPIN_CLI_WITH_MDNS=OFF` to leave one out even where its library is available.
On a build that also has ALSA, dropping the two sound-server backends costs
nothing but the extras below: a PulseAudio or PipeWire host stays reachable
through ALSA's plugin PCMs as `-o alsa:pulse` and `-o alsa:pipewire`. Where the
ALSA backend is absent too — it is Linux-only, and `-DSENDSPIN_CLI_WITH_ALSA=OFF`
turns it off anywhere — those two *are* the only route to a sound server, and
dropping them leaves the build with no audio path but `null`. That is why the
configure summary only offers the `alsa:` way back where there is an ALSA backend
to serve it.

`-DSENDSPIN_CLI_WERROR=ON` makes warnings fatal, for sendspin-cli's own three
targets and nothing else — the `sendspin` and GoogleTest trees fetched at
configure time are not ours to keep clean. It is off by default so that a fresh
diagnostic from a newer compiler cannot block a contributor who did not cause it;
CI turns it on, which is where the line is actually held.

> C++20 rather than C++17: sendspin-cpp's host build declares
> `target_compile_features(sendspin PUBLIC cxx_std_20)`, so the requirement
> propagates to anything that links it.

```bash
git clone https://github.com/Sendspin/sendspin-cpp-cli.git
cd sendspin-cpp-cli
cmake -B build
cmake --build build
./build/sendspin-cli --help
```

To build against a different version of the library:

```bash
cmake -B build -DSENDSPIN_GIT_TAG=v0.7.0
```

## Install

```bash
cmake -B build                                        # the prefix is chosen here
cmake --build build
sudo cmake --install build --component sendspin-cli
```

```
/usr/local/bin/sendspin-cli
/usr/local/lib/systemd/system/sendspin-cli.service          # Linux only
/usr/local/lib/sysusers.d/sendspin-cli.conf                 # Linux only
/usr/local/share/doc/sendspin-cli/README.md
/usr/local/share/doc/sendspin-cli/LICENSE
/usr/local/share/doc/sendspin-cli/sendspin-cli.conf.example
```

**`--component sendspin-cli` is not garnish.** ArduinoJson, fetched by sendspin-cpp in
turn, installs its headers and CMake export files unconditionally — no option to turn
that off the way IXWebSocket's and GoogleTest's have — so a plain `cmake --install`
stages 143 files more than the payload's own, none of which anything here links or
builds against. Naming the component is how you say which install rules are this
project's.

**The prefix is baked in at *configure* time**, because the unit's `ExecStart` is an
absolute path: `cmake -B build -DCMAKE_INSTALL_PREFIX=/usr` moves both the binary and
the path the unit names, while `cmake --install --prefix` relocates the files around a
unit that still points at the old one. Reconfigure rather than redirect the install.

The unit goes in `lib/systemd/system` and not in a multiarch `libdir`: a unit file is
architecture-independent, and systemd reads `/usr/lib/systemd/system` and
`/usr/local/lib/systemd/system` — never `lib/x86_64-linux-gnu/systemd/system`. That is
also why the default prefix needs nothing copied by hand. The account declaration
beside it is in `lib/sysusers.d` for the same two reasons: `systemd-sysusers` searches
`/usr/local/lib/sysusers.d` alongside `/usr/lib/sysusers.d`, and a list of users has no
architecture either.

To stage the same payload elsewhere — a tarball, a container image, an installer —
give `cmake --install` a `DESTDIR` instead of a different prefix, and every path
inside it is the path the file installs to:

```bash
DESTDIR=/tmp/stage cmake --install build --component sendspin-cli
```

That is exactly what CI publishes: see [CI](#ci). One ordering trap if you do both in
one build tree: a component install writes `install_manifest_sendspin-cli.txt` into
`build/`, so the `sudo` install above leaves a root-owned one and an unprivileged
`DESTDIR` install afterwards fails trying to rewrite it — on the CMake versions that
rewrite it unconditionally, which is most of them. Stage first, install second, which
is the order CI uses for exactly this reason.

### The systemd unit

```bash
sudo systemd-sysusers
sudo systemctl daemon-reload
sudo systemctl enable --now sendspin-cli
systemctl status sendspin-cli
journalctl -u sendspin-cli -f
```

One system unit, with sane defaults and nothing to fill in. **`systemd-sysusers` is not
optional** — it creates the account the unit runs as, and the unit does not start
without it; the end of this section says what that account is and what it may reach.
It runs the player in the **foreground** under `Type=simple`, so the log goes to the
journal rather than to a file something has to rotate — `-z` and `-f` would both be
working around the supervisor. `Type=forking` with `PIDFile=` pointing at `-P` is the
shape for a supervisor with no journal, and `Type=notify` is not available at all:
`sd_notify` is not wired up.

Two flags are on the `ExecStart` line, and both are there because a system unit has
neither of the environment variables a default path would come from:

| Unit directive | Flag | Without the pair |
|---|---|---|
| `RuntimeDirectory=sendspin-cli` | `--control-socket /run/sendspin-cli/control.sock` | no `$XDG_RUNTIME_DIR`, so no control socket — one warning, and the player carries on |
| `StateDirectory=sendspin-cli` | `--state-dir /var/lib/sendspin-cli` | no `$XDG_STATE_HOME`, so volume, mute and the static delay are forgotten every restart |

**Expect to set `output` before it plays anything.** A system unit has no user session,
so there is no PipeWire or PulseAudio for ALSA's `default` PCM to follow, and the
default that works from your shell usually fails under `systemctl` — a device that will
not open exits 1, and `Restart=on-failure` then retries it every five seconds. Run
`sendspin-cli -l`, pick the card, and put `output = hw:1,0` (or whatever it names) in
the config file below.

`-o pulse` and `-o pipewire` are no way round that: a per-user sound server puts its
socket in that user's `$XDG_RUNTIME_DIR`, which this unit's own account cannot reach —
`ProtectHome=yes` aside, it is a different user. They fail at startup rather than
silently, naming the server they could not reach. Point `PULSE_SERVER` at a socket the
service account can open, run the server in system mode, or name the card directly.

**Configure it in `/etc/sendspin-cli.conf`**, not by editing the unit — every config
key is a long flag name, so there is nothing the `ExecStart` line can say that the
file cannot. The installed `sendspin-cli.conf.example` is annotated for it. One
consequence to know, since the command line beats the file per option: `state-dir`
and `control-socket` in a config file are *silently ignored* under this unit, because
the unit passes both. Setting `control-socket` to the same path the unit uses is still
worth doing — it is what lets a subcommand find the socket with no flags.

**The subcommands need `sudo` here.** The control socket is mode `0600` and belongs to
the service account, so an unprivileged shell cannot connect to it — while root can,
because root is not subject to the mode:

```console
$ sudo sendspin-cli status --control-socket /run/sendspin-cli/control.sock
```

**It runs as `sendspin-cli`, an unprivileged account, and creating it is the one
step installing cannot do for you.** The unit names `User=sendspin-cli` — no home,
no shell — and a tarball has no `postinst`, so the declaration ships beside the unit as
`lib/sysusers.d/sendspin-cli.conf` and one command turns it into an account:

```console
$ sudo systemd-sysusers
Creating group 'sendspin-cli' with GID 997.
Creating user 'sendspin-cli' (Sendspin audio player) with UID 997 and GID 997.
```

It is idempotent, so running it twice is free. Skip it and the unit does not start at
all — `systemctl status` says `status=217/USER`, which names the cause:

```
sendspin-cli.service: Main process exited, code=exited, status=217/USER
```

The fragment carries two lines, and they are owed **together**: the account, and its
membership of `audio`. A `sendspin-cli` in no `audio` group is a player that starts and
cannot open a device, because `/dev/snd` is `root:audio` mode `0660`. That is the whole
reason this is a shipped declaration rather than a `useradd` line in this file — one
artifact, both halves, or neither. If you manage accounts with your own tooling, the
equivalent is `useradd --system --no-create-home -G audio sendspin-cli`.

`DynamicUser=` looks like it would avoid all of this and does not: it hands the player a
uid in no supplementary group at all, which deafens the ALSA backend.

**Upgrading from a version that ran as root needs nothing done to
`/var/lib/sendspin-cli`.** `StateDirectory=` chowns the directory it finds as well as
the one it creates, recursively, so a root-owned state file from an earlier install
becomes the new account's on the first start and the remembered volume, mute and static
delay carry over. systemd has documented that since v235 — the same release this unit
needs anyway — and CI plants a root-owned `/var/lib/sendspin-cli` on every Linux leg and
reads the value back out of it afterwards.

**What is hardened, and what is not.** The unit carries a hardening block —
`ProtectSystem=strict`, `NoNewPrivileges=`, an empty `CapabilityBoundingSet=`,
`RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6`, `SystemCallFilter=@system-service`
and the `Protect*=` family — and every directive in it was run rather than copied from a
list. Read the unit: each is commented where it sits. On systemd 255 `systemd-analyze
security` puts the result at **1.8 OK**, against **9.6 UNSAFE** for the root unit this
replaces; the Linux CI legs print the score on every run rather than asserting it, since
pinning a number would make an unrelated systemd release fail a build for rewording.

**One configuration stops working, and it is worth checking before you upgrade.**
Under `ProtectSystem=strict` a `logfile` or `pidfile` in `/etc/sendspin-cli.conf`
pointing outside `/run/sendspin-cli` and `/var/lib/sendspin-cli` is refused — `cannot
open logfile /var/log/sendspin-cli.log: Read-only file system`, on every restart,
rather than a player that logs nowhere in silence. Neither key is the shape for this
unit anyway, since journald already has stderr and `-z`/`-f` are for a supervisor
without one. If you want a logfile regardless, a drop-in is the way back:

```ini
[Service]
ReadWritePaths=/var/log
```

The block wants systemd **247** in full — `ProtectProc=` is its newest directive —
while the unit itself still starts on 236. Below 247 the shortfall is one line and a
log message: systemd warns `Unknown key name 'ProtectProc' … ignoring` and runs the
unit with the rest, which was checked on 245.

Four directives are deliberately *absent*, because they gate what the ALSA backend
reaches and a machine with no sound card cannot tell you whether they break it:
`PrivateDevices=`, `DeviceAllow=`, `ProcSubset=pid` and `RestrictRealtime=`. Each of
them passes every check CI makes, which is exactly why passing proves nothing about
them. They are tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md) item 10.

**To change any of it, use a drop-in** rather than editing the installed unit, which an
upgrade overwrites:

```bash
sudo systemctl edit sendspin-cli
```

An `ExecStart=` in a drop-in has to be cleared first (`ExecStart=` on its own line,
then the replacement), which is systemd's rule for every list-valued directive rather
than anything about this unit.

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

**A `-s` URL may carry userinfo, and this player does not send it.** `-s
ws://user:token@host:8927/sendspin` is accepted and dials `host:8927/sendspin` — the
credentials are dropped before the handshake, because the spec authenticates in the
handshake (pairing and a PSK) and neither sendspin-cpp nor the IXWebSocket transport under
it turns URL userinfo into an `Authorization` header. Verified on the wire: the upgrade
request carries `Host`, `Upgrade`, `Sec-WebSocket-*` and `Origin`, and nothing else. So if
something in front of your server wants HTTP Basic, a `-s` URL is not how to give it to it —
and accepting a credential it cannot send is a wrong the player owes a fix, tracked in
[docs/ROADMAP.md](docs/ROADMAP.md#6-daemonization-and-logging--shipped).

What userinfo does do is get written down, so **the log masks it**: `Connecting to
ws://user:***@host:8927/sendspin`, and the same in any complaint about a URL that did not
parse. Two limits on that are worth knowing. Lines tagged `sendspin.*` are the library's own
and print the URL in full — it logs what it dials through macros with no sink hook, so
nothing here can reach them. And masking reads the URL the way a URL parser does, so a
userinfo field containing an unencoded `/`, `?` or `#` — a raw base64 secret, say — ends the
authority early and is *not* masked; percent-encode those (`%2F`, `%3F`, `%23`). Both are in
[docs/ROADMAP.md](docs/ROADMAP.md#6-daemonization-and-logging--shipped).

Either way `argv` is not something the player controls: `ps` shows a running process's
command line to every local user on the box, so a URL you would rather not publish belongs
in the config file, where the file's own `0600` is the protection.

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
   interleaved PCM to standard output, `portaudio` follows whatever this host's
   default output currently is, `pulse` the PulseAudio server's own default sink,
   and `pipewire` wherever the PipeWire graph routes a playback stream. These mean
   the same thing on every build that has them, even where ALSA ships a PCM of the
   same name;
2. `<backend>:<device>`, split on the **first** colon, where the backend is one of
   the names the build reports (`null, stdout, alsa, portaudio, pulse, pipewire`).
   The split is on the first colon because ALSA device names carry their own, so
   `-o alsa:hw:2,0` is the `alsa` backend playing `hw:2,0`;
3. anything else is an ALSA PCM name, which is squeezelite's model — so `-o hw:2,0`
   and `-o default` keep working with no prefix at all. This step is deliberately
   ALSA-only: the other backends *do* enumerate their devices, so letting a bare
   name reach one would make the same command line mean different things per host.

```bash
./build/sendspin-cli -l                 # what this host can play through
./build/sendspin-cli -o default         # follow the system config (PipeWire/Pulse)
./build/sendspin-cli -o hw:2,0          # a card directly, bypassing the sound server
./build/sendspin-cli -o plughw:2,0      # same, letting ALSA convert rate/format
./build/sendspin-cli -o alsa:hw:2,0     # the same card, naming the backend explicitly
./build/sendspin-cli -o portaudio       # this host's default output (what macOS wants)
./build/sendspin-cli -o portaudio:2     # a PortAudio device by index, as -l prints it
./build/sendspin-cli -o "portaudio:MacBook Pro Speakers"   # ...or by name
./build/sendspin-cli -o pulse           # the PulseAudio server's default sink
./build/sendspin-cli -o pulse:alsa_output.pci-0000_00_1f.3.analog-stereo   # a named sink
./build/sendspin-cli -o pipewire        # let the PipeWire graph route it
./build/sendspin-cli -o pipewire:alsa_output.usb-Topping_D10s              # a named node
./build/sendspin-cli -o alsa:pulse      # the same server through ALSA's plugin PCM
./build/sendspin-cli -o null            # no sound card needed at all
```

A PortAudio device name is matched in full and case-insensitively. The **name is
the form worth writing down**: PortAudio numbers devices as it walks each host
API, so an index shifts as devices come and go. A name matching more than one
device is refused, naming the candidates, rather than guessed at — two host APIs
can offer the same card under the same name.

A PulseAudio sink or a PipeWire node is named exactly as `-l` prints it, and the
name is passed to the server unchanged — so it is the *server* that resolves it, at
every stream, and a sink that appears after startup is picked up by the next track
without a restart.

#### `-o pulse` and `-o pipewire` changed meaning in this release

Both are also the names of real ALSA plugin PCMs, on exactly the hosts these
backends target. Before the native backends existed, `-o pulse` fell through step 3
and opened `libasound2-plugins`' PulseAudio PCM; now step 1 claims the name and it
reaches the native backend instead.

That is deliberate — on those hosts the native backend is the better answer, for
the reasons below — but it is a working command line changing what it does on
upgrade, so:

- **`-o alsa:pulse` and `-o alsa:pipewire` are the way back**, unambiguously, and
  they work on every build with the ALSA backend, native backends or not;
- `-l` leaves `pulse` and `pipewire` out of its ALSA PCM list on a build that
  shadows them, because listing them would name devices `-o` can no longer reach —
  and says so, with the `alsa:` form, above the list;
- a build *without* the native backends answers `-o pulse:<sink>` by naming both
  `-DSENDSPIN_CLI_WITH_PULSE` and the `alsa:pulse` route, rather than sending you
  off to rebuild for a path that already works.

What the native backends buy over the plugin PCM, which is why the trade is worth
making:

- **The sinks and nodes are enumerable.** Through the plugin there is one ALSA
  hint and no more, so a sink is chosen with `PULSE_SINK` or `~/.asoundrc` rather
  than with `-o`.
- **Honest sync feedback.** The ALSA backend derives its playout timing from
  `snd_pcm_delay()`, which through the plugin reports the plugin's own buffering
  rather than the server's. `pa_stream_get_latency()` and PipeWire's `pw_time`
  answer for the real path — and this is a *synchronised* multi-room player, so
  that is not cosmetic.
- **A named stream.** The host's mixer shows `sendspin-cli` and can route it
  per-application, where every plugin stream is just "ALSA plug-in".

`-o pipewire` is a native backend rather than a second way to spell `-o pulse`
because only a native client can name a *node*: `pipewire-pulse` presents sinks,
which is a compatibility view of the graph rather than the graph. It reaches no
host libpulse cannot — what it buys is on the far side of the socket.

The default `-o` is unchanged on every host that had one: `default` where the ALSA
backend is present, then `portaudio`, then `pulse`, then `pipewire`, and `null`
where none is. ALSA wins wherever it is built, because on Linux everything else is
a layer over it or beside it and going direct is one layer fewer — and `default`
already reaches whichever sound server the host runs.

Volume is applied in software on every backend, sharing one Q32 fixed-point
implementation, so a stream sounds the same whichever plays it — and works the same
through PipeWire's ALSA plugin as through bare hardware. It is deliberately *not*
handed to PulseAudio or PipeWire as a stream volume: the curve below is the spec's
rather than either server's, stacking a server gain on the software one would square
the taper, and a gain the host's mixer could move behind our back would leave the
player reporting a volume the speaker is not at — which group volume is derived
from. The ALSA hardware mixer is a follow-up, for the same one-path-or-the-other
reason.

The one audible difference between backends: PortAudio scales in its audio
callback, so a volume change also reaches audio already buffered, where ALSA,
PulseAudio and PipeWire scale on the way in and so only affect what has not been
handed over yet.

The curve is the one the spec names, `amplitude = (volume / 100)^1.5`, because a
Sendspin volume is **perceived loudness** rather than amplitude — volume 50 is
meant to sound half as loud as 100, and that exponent is what makes the number on
a controller's slider mean that. It is deliberately not upstream's `^2`, which is
about 3 dB quieter at volume 50 and 6 dB at 25.

A change is applied over a **20 ms ramp** rather than as a jump, which the spec asks
for: *"to avoid audible clicks, clients SHOULD apply volume changes over a short
ramp."* Jumping the gain between one sample and the next is a step in the waveform,
and a step is what a click is — most obvious on a mute, which is the largest jump
there is. The gain moves at a fixed slew rate, so a full-scale change takes the whole
20 ms and a small one is proportionally quicker, and it steps **per frame** so every
channel of a frame is scaled by the same gain. A stream *starts* at its gain rather
than fading into it, so a volume restored from the state file produces no fade at the
top of the first track. `-o null` and `-o stdout` are unaffected: they record the
volume without applying it, so there is nothing there to ramp.

### Buffering, and what gets advertised

`--buffer-ms <ms>` (10–2000, default 100) is how much audio the output backend
keeps queued — one figure for every backend rather than squeezelite's ALSA-only
`-a`, whose `<b>:<p>:<f>:<m>` grammar would mean something different per backend
here. ALSA divides it into five periods; PortAudio and PipeWire make it the ring
size, where a figure smaller than one device buffer or graph quantum is raised to
the floor and says so at `debug`; PulseAudio makes it the server's own queue
(`tlength`), which is the only buffer in that path and exactly what its latency
query answers about. A device-less sink (`null`, `stdout`) has nothing to size and
ignores it.

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
- `-o pulse` and `-o pipewire` advertise everything `sendspin-cli` can emit, and
  that is the honest answer rather than a shortcut: both servers resample and
  reformat into whatever the sink is running at, so what a sink *accepts* says
  nothing about the hardware behind it.

Either way, a format the device then refuses is reported loudly — naming the
device, the format, and the fact that the stream's audio is being discarded —
rather than leaving a player that looks healthy and plays nothing.

`--audio-format <codec:rate:depth:channels>` (e.g. `flac:48000:24:2`) pins a
preferred format on top of that derived list — the way to hold a fussy DAC at
the one shape it is happy in. It is a *reorder*, not a narrowing: the pinned
entry moves to the front, which is where a spec-following server picks, and
everything else the device takes is still offered behind it. A pin that
derived list does not carry **refuses to start** — playing something else
instead is the failure the flag exists to prevent — and `-l` shows what the
device itself reports, which is not the same set. The advertisement carries a
single channel count — stereo where the device takes it, its narrowest count
otherwise — so a mono or multichannel pin is refused on any device that also
takes stereo. Codecs are `flac`, `opus` and `pcm`; the grammar is the Python
`sendspin-cli`'s, plus `opus`. An `opus` pin is refused outright at anything
but 48 kHz / 16-bit / at most two channels, since that is the only shape it is
ever advertised in.

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
better choice under a supervisor that already captures stderr. That is what the unit
this project installs does — see [The systemd unit](#the-systemd-unit), where neither
`-z` nor `-P` appears.

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
static delay: 0 ms
output: default (48000 Hz / 2 ch / 16-bit)

$ sendspin-cli pause
$ sendspin-cli vol 40
$ sendspin-cli seek-rel -30000
$ sendspin-cli delay 250
```

Every transport verb the protocol has, one subcommand each — `play`, `pause`,
`stop`, `next`, `prev`, `vol`, `mute`, `seek`, `seek-rel`, `repeat`, `shuffle`,
`switch` — plus two that go nowhere near the server: `status`, and `delay`.
`--help` lists them all with their arguments.

**Four fields are the server's word, and can lag what is true** — which is why the
block ends with a `note:` line saying so. `state`, `position`, `repeat` and
`shuffle` all come from the server's last report, and the spec does not oblige it
to resend them after acting. Observed against a real server: `shuffle` read `off`
for minutes while it was demonstrably shuffling, and the position climbed straight
through seeks that audibly worked. If you have just changed something and the
figure has not moved, that is the likely reason — not a failed command.

`position` additionally says `(estimated)` while playing, because the library
interpolates forward from the last progress the server sent. After a seek that the
server does not re-report, the estimate drifts by however far you jumped. Paused,
it is the server's own snapshot and carries no marker.

`player volume` is the gain **this box's output** is applying, and it says
`(default; no server has set it)` until a server sends a volume command. The
qualifier is how you tell "nobody has set this" from a server that deliberately
chose full output.

That figure is also what the server is told, from the first message — which the
spec requires and which matters more than it looks: **group volume is the average
of the players' volumes**, and setting group volume applies a *delta* against it.
A player that reported a volume it was not applying would skew the group reading
for every controller, and make the next group volume change land wrong by exactly
that error.

Two `status` lines are worth reading together. `state` is the **group's** transport
state, from the metadata `playback_speed`, and reads `unknown` rather than guessing
when the server has sent no progress. `stream` is whether audio is arriving at
**this** endpoint, which is a different fact — a player dropped from the group loses
it while the group plays on. A `stream: receiving` line with no format after the
device name is the case where the device *refused* the stream's format and its audio
is being discarded; the log says so loudly at the same moment.

**`delay` is the one subcommand that changes *this* box.** It sets the player role's
`static_delay_ms`, 0–5000: **how much latency this endpoint's hardware adds after the
audio port** — an amplifier, an external speaker, a DSP.

Note the direction, because it is the opposite of what the name suggests. This is not
"play this speaker later"; it is "my gear is *already* this far behind". The sync task
**subtracts** the figure from every chunk's timestamp, so the player hands audio to the
device that much **earlier**, and the sound then lands on the timestamp the server meant.
The spec puts it as: 0 "means audio exits the device's audio port at the timestamp", and
the value "compensates for additional delay beyond the port". So if this speaker sounds
250 ms *late* against the rest of the group, `delay 250` is the fix:

```console
$ sendspin-cli delay 250      # my amp adds 250 ms, so hand audio over 250 ms early
$ sendspin-cli status | grep 'static delay'
static delay: 250 ms
$ sendspin-cli delay 0        # off again
```

Three things follow from it being local rather than a transport command. It works with
**no server connected**, exactly as `status` does — nothing about it is sent, so there
is nothing for a missing connection to stop. It is **remembered across restarts**, which
the spec requires of a client, so you set it once per speaker. And the player still tells
the server, because the library republishes `client/state` when the value changes — the
server needs it to work out how far ahead to send audio.

Out-of-range values are refused rather than clamped: `delay 5001` exits 1 and names the
bound. The library would quietly take it down to 5000 and report success, which would
mean a delay nobody asked for.

One caveat: changing it mid-stream re-times chunk scheduling, so expect a brief resync.
Set it while stopped where you can.

**Three of the others are easy to misread, so:**

- **`vol` is the *group* volume**, not this box's output level. It goes out as a
  `controller@v1` command and the server spreads it across every player in the
  group, clamping per player. That is why `status` prints `group volume` and
  `player volume` as two named lines rather than one ambiguous `volume:` — a
  squeezelite refugee will expect `vol 50` to move *this* box, and it does not.
  `delay` above is the counter-example: that one really is this endpoint's own.
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
| `0` | sent, or answered locally (`status`, `delay`) |
| `1` | the command line did not parse (`vol 500`, `delay 5001`) |
| `2` | the player refused the argument (a `seek` past `seek_max_ms`) |
| `3` | nothing is listening on that socket — no player, or the wrong `--port` |
| `4` | the player is up but has **no server connection** |
| `5` | the server does not offer that command (`supported_commands`) |
| `6` | the exchange broke down |

`4` and `5` are kept apart on purpose. A dropped connection *empties*
`supported_commands`, so the naive check answers "pause is not supported" when the
truth is that nothing is connected — sending you to read your server's
capabilities instead of its connection. The two locally answered subcommands —
`status` and `delay` — are never refused by any of them: nothing about either is
sent, so a missing connection is no obstacle. A disconnected player is exactly when
reading `status` is worth doing, and a speaker's own delay does not become unsettable
because nothing is playing through it.

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

### Stream hooks

`--hook-start` and `--hook-stop` run a shell command when a stream starts and when
it stops — the amplifier relay, the light, the notification. The command goes
through `/bin/sh -c`, so pipes and `&&` work, and the event's facts arrive in the
environment rather than as arguments:

| Variable | Carries |
|---|---|
| `SENDSPIN_EVENT` | `start` or `stop` |
| `SENDSPIN_SERVER_ID` | the connected server's id |
| `SENDSPIN_SERVER_NAME` | its friendly name |
| `SENDSPIN_SERVER_URL` | the URL this run dialled — set only on an `-s` run |
| `SENDSPIN_CLIENT_ID` | this player's id, when `--id` chose one |
| `SENDSPIN_CLIENT_NAME` | this player's friendly name (`-n`) |

The vocabulary is the Python `sendspin-cli`'s, deliberately: a hook script written
against one player runs unchanged against the other. `SENDSPIN_CLIENT_ID` carries the id
`--id` or the `id` config key chose: the library derives one from the interface
MAC when neither did, and does not expose what it derived, so a run that
configured no id leaves the variable unset. A variable whose value is
unknown for the event is left unset rather than exported empty, so `[ -n
"$SENDSPIN_SERVER_ID" ]` means what it says — and any `SENDSPIN_*` inherited from
the player's own environment is cleared first, so a wrapper script's stale export
cannot describe some other run to the hook.

`SENDSPIN_SERVER_URL` says what this run dialled, not which server answered.
Those are the same thing whenever `-s` is how the player got its connection —
but `-s` leaves the inbound listener up, and a server that dials *in* while an
outbound attempt is outstanding or has failed is a connection the player cannot
tell apart from its own: the library reports that one is up, not where it came
from. A hook that must be certain which server it is acting on should read
`SENDSPIN_SERVER_ID`, which always describes the connection the stream arrived
on.

A stop event carries the same server facts as the start it pairs with, so
`--hook-stop 'curl -X POST .../$SENDSPIN_SERVER_ID/off'` names the server the
stream was actually on. They are the values gathered when the stream started
rather than whatever is left to ask at the end: a stream usually ends *because*
its connection went, and the server is no longer there to describe itself.

The hook fires on the stream lifecycle, not on the format being accepted: a stream
the device refused is still audio arriving, so the amplifier is on for exactly as
long as `status` says `stream: receiving`. Nothing waits on it — a hook that blocks
cannot stall the audio path. It is reaped from the main loop; its output lands in
the log (stdout deliberately re-pointed at stderr, since `-o stdout` may be
carrying PCM); and a non-zero exit is a `W hook:` line, not a player failure. A
hook still running at shutdown is left to finish: an amplifier half-switched is
worse than an orphan. A player stopped while a stream is playing runs its stop
hook on the way out, so `systemctl stop` leaves the amplifier off rather than on.

The hook is handed nothing of the player's but that output stream: every other
descriptor is closed and SIGPIPE is back at its default, so `something | head -1`
behaves the way it would in any other shell and a slow hook cannot sit on the
port a restart needs.

```bash
sendspin-cli -o hw:1,0 \
  --hook-start 'amixer -c 1 set Master unmute' \
  --hook-stop  'amixer -c 1 set Master mute'
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
`-s` server, `-z` daemonize, `-P` pidfile, `-d`/`-f` logging. All but `-l` and
`-z` also have a long spelling — `--output --name --server --pidfile --logfile
--log-level` — so that every config key is a flag name. Fifteen more are long-only
because they are not squeezelite's: `--port`, the port this player serves on,
`--buffer-ms`, `--static-delay`, `--audio-format`, the two mDNS flags `--no-mdns`
and `--mdns-name`, the two control-socket flags `--control-socket` and
`--no-control`, the two stream hooks `--hook-start` and `--hook-stop`, the three
identity flags `--id`, `--manufacturer` and `--product-name`, and `--config` and
`--state-dir` for the two files above. `--id` is the one to know about: it is the *stable* id a server
files this player's volume, group and pairing under, and without it the id is
derived from the network interface MAC — so two players on one host share it, and
each server-side setting lands on whichever connected last. A dual-mono pair needs
its own `--id` per instance, and its own `--port` and `--state-dir` with it — the
state file is namespaced by neither `--id` nor `--port`, so without `--state-dir`
the two overwrite each other's volume, mute, delay and last server. Run `--help` for the current
state of each — a few still point at [`docs/ROADMAP.md`](docs/ROADMAP.md) for
behaviour that is not built yet.

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

$ sendspin-cli --static-delay 5001
error: invalid --static-delay '5001' -- expected 0-5000
```

**`--static-delay <ms>` is a first-run default, not an override.** It seeds the same
0–5000 static delay the `delay` subcommand sets — the latency this endpoint's hardware
adds *after* the audio port, which the player compensates for by handing audio over
earlier; see the control-channel section above for the direction, which is the opposite
of what the name suggests. The library prefers whatever the state store
remembers and reads this flag only when there is nothing remembered — exactly as a
restored volume beats the sink's default. So it is for declaring a speaker's physical
offset up front (an Ansible-managed fleet, a container with an ephemeral
`--state-dir`); once a server or `sendspin-cli delay` has set one, the remembered value
wins every run after and this flag is inert. The startup log says which of the two it
took.

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

### The config file, and what the player remembers

Two files, and the split is deliberate: one you write and the daemon only ever
reads, one the daemon writes and you never need to touch.

**The config file** holds anything you would otherwise type. Keys are the long
flag names without their dashes, one per line, and a value is exactly the string
the flag would have been given:

```
# /etc/sendspin-cli.conf, or ~/.config/sendspin-cli/config
name = kitchen
output = hw:1,0
buffer-ms = 250
static-delay = 40
server = mdns:Living Room
no-mdns = true
```

That is why `-o -n -s -P -f -d` grew the long aliases `--output --name --server
--pidfile --logfile --log-level`: one vocabulary, so `--help` is the config
reference rather than a second document to keep in step. `#` starts a comment
only at the **start** of a line — a name or a path is free to contain one, and a
reader that ate everything after a `#` would silently truncate it. Booleans take
`true`/`yes`/`on`/`1` or `false`/`no`/`off`/`0`. Where a key appears twice, the
last one wins.

The first of these that exists is read **whole**, and nothing below it is merged
over the top:

1. `--config <path>` — and this one is **fatal if it cannot be read**, because
   you named it. Falling back would start a player on options nobody chose.
2. `$XDG_CONFIG_HOME/sendspin-cli/config`
3. `$HOME/.config/sendspin-cli/config`
4. `/etc/sendspin-cli.conf`

Finding none is silent and normal. There is deliberately no `--no-config`: the
asymmetry above already gives you `--config /dev/null`. A half-overridden config
assembled from several layers is far harder to reason about than one file you can
read top to bottom, which is also why there is no `$XDG_CONFIG_DIRS` traversal.

**Precedence is command line > config file > built-in default**, per option
rather than per file — `-n bathroom` on a line whose config also sets
`buffer-ms` overrides only the name. Everything can come from a file except `-l`,
`-z`, `--config` itself, and `--help`/`--version`, which have no meaning in one.
Run shape stays on the command line: excluding it is
reversible, and debugging a `daemonize` that came out of a file under systemd is
not. A config that names one is refused as an unknown key.

A configured value is validated by exactly the code that validates a typed one,
with the same message and the line to go and fix:

```console
$ sendspin-cli
error: /etc/sendspin-cli.conf:4: invalid --buffer-ms '5' -- expected 10-2000
```

An unknown key or a line that is not `key = value` is refused the same way. A
silently ignored typo is the failure mode this whole surface exists to avoid — so
is a config that is quietly skipped, which is why a file that exists and does not
parse stops the run rather than falling through to `/etc`. `--help`, `--version`
and `-l` short-circuit above all of it: a broken config must not stop `--help`
from telling you how to fix it. Startup logs one line naming the file in use, or
saying there is none.

`server` from a config file behaves exactly as `-s` does, and that includes
suppressing the mDNS advertisement — the spec forbids advertising
`_sendspin._tcp` while this end is the one dialling out.

**The state file** is the other half: what the daemon remembers for itself,
across restarts.

```
# Written by sendspin-cli. Edits are overwritten.
last-server = 7f3a…
last-server-hash = 3387423128
static-delay-ms = 375
volume = 42
muted = true
```

`static-delay-ms` is there because the spec requires it — *"Clients must persist
`static_delay_ms` locally across reboots and server reconnections"* — and
`volume`/`muted` because the spec marks those RECOMMENDED. `last-server` is the
server id mDNS discovery uses to break a tie between candidates;
`last-server-hash` is the opaque `uint32_t` the library asks us to keep so *it* can
prefer the last-played server among inbound connections. They mean different things
and are deliberately not reconciled with each other.

**A remembered `static-delay-ms` is reported *and* applied.** The player hands the
figure back to the server in its first `client/state`, so the two agree across a
restart, and the library's sync task subtracts it from every chunk's timestamp before
scheduling — which is the spec's own rule for how a client obeys it. `sendspin-cli
status` prints the value in force, and `sendspin-cli delay <ms>` changes it. Three
things can set it: a server's `set_static_delay`, that subcommand, and
`--static-delay` on a first run with nothing yet remembered.

It lives at `$XDG_STATE_HOME/sendspin-cli/state`, then
`$HOME/.local/state/sendspin-cli/state`, and `--state-dir <dir>` overrides both —
a systemd **system** unit has neither variable and gets `/var/lib/sendspin-cli`
from `StateDirectory=`. With none of the three the player still runs and simply
remembers nothing. Writes go through a temporary, an `fsync` and a `rename` at
mode `0600`, so a player that loses power mid-write leaves either the old file or
the new one and never half of either.

Two players on one host share this file unless you give each its own
`--state-dir`. They already need different `--id`s and `--port`s; give them
different state directories too, or the second one to save its volume overwrites
the first's.

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
`status` round-trips, that `delay` reaches the player role and survives a restart
through the state store, that `--no-control` binds nothing, that a stale socket is
taken over after a `SIGKILL`, that it is gone after `SIGTERM`, and that a second
instance on the same socket is refused. The config file is in here for the same
reason: a player configured entirely from a file has to be findable by a
subcommand that repeats none of it, and that is two processes agreeing on one
path. CI runs it on every platform leg; run it yourself against any build.

## CI

Every branch push and pull request builds on `ubuntu-24.04`, `ubuntu-24.04-arm`
and `macos-14`, plus a fourth leg configured `-DSENDSPIN_CLI_WITH_MDNS=OFF` — which
compiles `src/mdns_null.cpp` in place of `src/mdns_dnssd.cpp`, so that
configuration is built rather than assumed. Every leg builds with
`-DSENDSPIN_CLI_WERROR=ON` and runs the unit suite, and each asserts from its own
configure output that it found the backends it expects: a missing `-dev` package
does not fail a configure, so without that check the matrix would happily go green
on a deaf, undiscoverable binary.

The matrix lives in `.github/workflows/build.yml`, which both `ci.yml` and
`release.yml` call, so a release is built and gated exactly the way a push is.
`ci.yml` ignores tags for that reason — otherwise a tag would build twice.

To try a commit without building it, open its run under the repository's Actions
tab and take `sendspin-cli-<version>-<os>-<arch>` from the run summary. Inside is a
tarball staged by the same [`install()` rules](#install) — `DESTDIR` and the
`sendspin-cli` component — so every path under its `usr/` is the path the file
installs to, plus a `BUILD-INFO.txt` at the root naming the runtime packages it needs:

```bash
sudo tar -xzf sendspin-cli-0.1.0-linux-x86_64.tar.gz --strip-components=1 -C / \
  sendspin-cli-0.1.0-linux-x86_64/usr
sudo systemctl daemon-reload
```

Naming the `usr` member is what leaves `BUILD-INFO.txt` in the archive rather than
unpacking it at `/`. Or run it where you unpacked it, at
`./<name>/usr/local/bin/sendspin-cli`. The prefix is baked in, so a binary moved out
of `/usr/local` leaves the unit naming a path with nothing at it. The macOS leg
publishes a second artifact beside that tarball,
`sendspin-cli-<version>-macos-arm64-installer`, holding the
[`.pkg`](#the-macos-installer-pkg) described below. These are per-commit builds
kept for 14 days. For something that does not expire, take a
[release](../../releases) instead.

## Releases

Pushing a `vMAJOR.MINOR.PATCH` tag builds the same matrix and publishes the three
platform archives and the macOS installer `.pkg`, plus a `SHA256SUMS` covering all
four, as a GitHub Release. The workflow triggers on `v*` but refuses anything else
that matches — a prerelease like `v0.2.0-rc1` is rejected rather than quietly
published as the latest release, until somebody decides what it should mean.
Nothing else publishes, and the workflow never creates a tag: a release exists
because a human tagged a commit
whose version `CMakeLists.txt` already agreed with. It is attached whole or not at
all — the release is drafted, its assets are checked against the set the tag is
supposed to carry, and only then is it published, so a half-finished upload leaves a
draft rather than a release missing an architecture.

The archives are the same staged payload described above, so they install the same
way, and the `.pkg` wraps the macOS one. Verify whichever you took first:

```bash
sha256sum --ignore-missing -c SHA256SUMS      # Linux
shasum -a 256 --ignore-missing -c SHA256SUMS  # macOS
```

`--ignore-missing` because `SHA256SUMS` lists all four and you have almost
certainly taken one; without it the rest are reported as failures and the command
exits non-zero on a file that is fine. Those checksums cover the four things built
here, not the `Source code` archives GitHub attaches on its own. Neither the macOS
binary nor the `.pkg` around it is signed — see below. A Developer ID signature and
notarization are still owed, tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md) item 10.

### macOS, and Gatekeeper

Unpack the tarball from a terminal rather than in Finder:

```bash
tar -xzf sendspin-cli-0.1.0-macos-arm64.tar.gz
./sendspin-cli-0.1.0-macos-arm64/usr/local/bin/sendspin-cli --version
```

That is not fussiness. These binaries are **ad-hoc signed** — the minimum an
arm64 Mach-O needs to execute at all, applied by the linker — so they carry no
developer identity and `spctl` rejects them. What decides whether you notice is
the quarantine flag, and `tar` does not propagate it where Finder's Archive
Utility does. If you did unpack in Finder, or macOS refuses it anyway:

```bash
xattr -d com.apple.quarantine ./sendspin-cli-0.1.0-macos-arm64/usr/local/bin/sendspin-cli
```

### The macOS installer `.pkg`

Every [release](#releases) attaches an installer, and the macOS CI leg publishes the
same thing per commit in its `-installer` artifact:

```bash
sudo installer -pkg sendspin-cli-0.1.0-macos-arm64.pkg -target /
sendspin-cli --version
```

It carries the same four files the tarball does, staged from the same
[`install()` rules](#install), and puts them at the `/usr/local` prefix the binary
was built for. It refuses a Mac it cannot run on: the architectures are read off the
binary with `lipo` at build time and declared in the package, so an arm64-only
installer is turned away on an Intel Mac instead of reporting success and leaving a
`Bad CPU type in executable`. Build the same `.pkg` from a payload of your own with:

```bash
DESTDIR=/tmp/stage cmake --install build --component sendspin-cli
scripts/build_macos_pkg.sh /tmp/stage 0.1.0 sendspin-cli.pkg
```

**The `.pkg` is not the Gatekeeper fix.** It is unsigned and unnotarized, exactly as
the binary inside it is ad-hoc signed, and `spctl -a -t install` rejects it. What it
does change is narrower and worth stating precisely, because the four ways you can
come by this file behave differently:

- **Taken from a release page and double-clicked.** The `.pkg` is the download, so
  the browser quarantines the `.pkg` itself and Gatekeeper refuses it outright —
  allow it once under **System Settings → Privacy & Security**, which offers an
  *Open Anyway* for the last thing it blocked, or take the terminal path below.
- **Taken from the Actions tab and double-clicked.** An artifact arrives as a *zip*,
  so it is the zip that is quarantined, and what decides whether the `.pkg` inside
  inherits the flag is the same thing it is for the tarball above: `unzip` from a
  terminal does not propagate it, Finder's Archive Utility does.
- **`sudo installer -pkg … -target /`** — not gated at all. The `installer` CLI makes
  no Gatekeeper assessment, whatever the file is flagged with, which is why CI can
  install and test its own artifact and why it is the line printed above.
- **Built locally** by the script above, or unzipped with `unzip` — never
  quarantined, so it opens in Installer.app with nothing to clear.

Files that `installer` puts on disk are not quarantined either way, so the installed
`/usr/local/bin/sendspin-cli` needs no `xattr -d` — which the tarball's does if you
unpacked it in Finder. That is convenience, not identity.

There is no uninstaller. Four files and the receipt undo it completely — the systemd
unit and the sysusers fragment in the [Install](#install) list are Linux-only and never
in this package:

```bash
sudo rm -f /usr/local/bin/sendspin-cli
sudo rm -rf /usr/local/share/doc/sendspin-cli
sudo pkgutil --forget io.github.chrisuthe.sendspin-cli
```

A Developer ID signature and notarization are still owed, and the `.pkg` is what will
carry them: `xcrun stapler` staples a ticket to a `.app`, a `.dmg` or a `.pkg` and
refuses a bare executable, so notarizing the loose binary alone would still leave an
offline Mac asking Apple. Tracked in [`docs/ROADMAP.md`](docs/ROADMAP.md) item 10.

## Roadmap

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the epic breakdown and the child
tasks that build out audio backends, discovery, daemonization, the local control
channel, a config file, Docker packaging, and more.

## Upstream

- Sendspin protocol library: https://github.com/Sendspin/sendspin-cpp
- Control-scheme inspiration: https://github.com/ralph-irving/squeezelite

## License

Licensed under the [Apache License 2.0](LICENSE), matching sendspin-cpp.
