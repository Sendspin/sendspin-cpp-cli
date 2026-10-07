# Advanced usage

Most players need only a name and an output device. This page covers the less common
ways to run `sendspin-cli`; use `sendspin-cli --help` for the complete flag reference.

## Connection modes

By default, the player advertises `_sendspin._tcp` over mDNS and waits for a Sendspin
server to connect:

```bash
sendspin-cli -n living-room
```

To make the player connect to a known server instead, use `-s`/`--server`. This disables
mDNS advertisement because the Sendspin protocol does not allow both modes at once:

```bash
sendspin-cli --server music.local              # the server port defaults to 8927
sendspin-cli --server music.local:9000
sendspin-cli --server ws://music.local:9000/sendspin
sendspin-cli --server "[2001:db8::1]:8927"     # an IPv6 literal must be bracketed
sendspin-cli --server mdns:                    # discover any server
sendspin-cli --server "mdns:Music Assistant"   # ...or one by its advertised name
```

An outbound connection retries until it answers, and `--mdns-name` is unused in this
mode. `--no-mdns` turns the advertisement off without switching modes.

## Choosing an output

List the outputs available in this build and on this host, with the rates, formats, and
channel counts each one accepts:

```bash
sendspin-cli -l
```

Set the selected value with `-o`/`--output` or persist it in the
[configuration file](Configuration):

```bash
sendspin-cli --output hw:1,0
```

An argument is either a reserved name (`null`, `stdout`, `-`), a `<backend>:<device>`
pair split on the first colon (`coreaudio:2`, `portaudio:2`, `pulse:<sink>`,
`pipewire:<node>`), or an ALSA PCM name such as `hw:1,0`, `plughw:1,0`, or `default`. `plughw:` lets ALSA convert
rate and format for a device that refuses the stream as it arrives.

`default` follows the host's normal audio configuration. Under a system service, name
a hardware device such as `hw:1,0` instead; the service does not have a logged-in
desktop audio session.

## Stream a microphone or line-in

`--input` makes the player a Sendspin *source* as well: it captures from a sound card and
streams that audio to the server, for a turntable on a line-in or a microphone in a room.

```bash
sendspin-cli -n den --input default
sendspin-cli -n den --input hw:1,0
sendspin-cli -n den --input pipewire
sendspin-cli -n den --input pulse:alsa_input.usb-mic.analog-stereo
```

Nothing is captured until the server starts the source, and the device is released again
when the server stops it. Without `--input` the player does not offer the source role at
all.

`sendspin-cli -l` lists capture devices in their own section, under the outputs: ALSA
PCMs with the rates, formats and channel counts each takes, then the PulseAudio sources
and PipeWire source nodes. `--input` reads its argument the way `-o` does:

- `alsa:<device>`, or a bare ALSA name such as `default`, `hw:1,0` or `plughw:1,0`.
- `pulse:<source>` for a PulseAudio source, or `pulse` alone for the server's default.
- `pipewire:<node>` for a PipeWire source node, or `pipewire` alone for the graph's
  default. `pulse:default` and `pipewire:default` mean the same as the bare names.
- `tone` and `null`, which need no sound card: `tone` streams a 440 Hz test tone, which
  checks the path to the server without a microphone, and `null` streams silence.

On a desktop, prefer `pulse` or `pipewire` to a `hw:` name: the sound server shares the
card with other programs, where a `hw:` device is held exclusively.

The capture format is chosen once at startup: 48000 Hz, stereo, 16-bit if the device
takes it, otherwise the nearest it does take — a mono microphone is sent as mono. A sound
server converts, so a capture through `pulse` or `pipewire` always runs at 48000 Hz,
stereo, 16-bit. The startup log and `sendspin-cli status` both show what was chosen:

```text
input: hw:1,0 (48000 Hz / 2 ch / 16-bit), streaming
```

A device that cannot be opened — a wrong name, a card another program holds, or a sound
server that is not running — stops the player at startup with an error naming it. A
device unplugged mid-stream, or a sound server that restarts, is logged and reopened when
it comes back; playback is not affected. A named PulseAudio source or PipeWire node is
never swapped for the default while it is away.

Capture through CoreAudio or PortAudio is not available yet, so capturing from a sound
card needs a build with ALSA, PulseAudio or PipeWire; `tone` and `null` work in any
build. A build without the PulseAudio or PipeWire backend can still reach that server
through ALSA's own PCMs: `--input alsa:pulse`, `--input alsa:pipewire`.

## Logging and background operation

Run in the foreground with verbose diagnostics while investigating a problem:

```bash
sendspin-cli -d debug
```

Levels are `none`, `error`, `warn`, `info` (the default), `debug`, and `verbose`. One
level covers this player and the sendspin library together, and every line is
`<L> <tag>: <message>`, so filter after the fact:

```bash
sendspin-cli -d debug 2>&1 | grep ' mdns:'
```

For service management on Linux, prefer the supplied
[systemd service](Running-as-a-Service). For a supervisor without a journal, `-z`
detaches the process, `-f` writes the log to a file, and `-P` holds a locked pidfile:

```bash
sendspin-cli -z -P /run/sendspin-cli.pid -f /var/log/sendspin-cli.log
```

`-z` refuses `-o stdout` and warns without `-f`, which is where the log would
otherwise be lost. `SIGHUP` reopens the `-f` path, so `logrotate` can rotate it. These
three, along with `-l`, `--config`, `--help`, and `--version`, cannot come from a
config file.

## Buffering and audio format

`--buffer-ms` controls how much audio the output backend keeps queued, from 10 to 2000
(default 100). Raise it if a busy host produces clicks or dropouts.

`--output-delay <0-5000>` declares how much latency this endpoint's hardware adds
*after* the audio port, so the player hands audio over that much earlier. It is a
first-run default only: once a server or [`delay`](Controlling-the-Player) has set one,
the remembered value wins. `--static-delay` is still accepted as the old name.

`--audio-format <codec:rate:depth:channels>[,...]` lists preferred formats, comma-separated
in priority order. They go to the front of the advertised list in that order:

```bash
sendspin-cli --audio-format flac:48000:24:2,pcm:48000:24:2
```

This is a preference, not a restriction. Every other format the player normally
advertises still follows the listed ones, and a server uses the first format it can
encode, so it may still choose a later one. A single format works the same way.

The player refuses to start if any listed format is not among those it advertises for
the device, and the error names every one that is missing. Run `sendspin-cli -l` to see
what the device accepts.

## Identity

The client id a server files this player's volume, group, and pairing under is the public
half of a keypair the player generates on its first run and keeps in the
[state file](Configuration#the-state-file); `-n` is only the displayed name.
`sendspin-cli status` prints it. It cannot be chosen: `--id` is refused. Run two players on
one host with their own `--port`, `--state-dir`, and control socket — the state directory is
what gives each its own identity.

`--manufacturer` and `--product-name` set what the player reports to servers, for a
product that embeds this player and should be listed as itself.

## Pairing and unpaired access

Every connection is encrypted, and a server has to **pair** with the player before it may
play on it. A server that has not paired can still connect, but stays idle; the `trust` line
of `sendspin-cli status` reads `paired` or `unpaired` for the server connected now.
[Pairing a Player](Pairing-a-Player) walks through the three ways to pair.

`--allow-unpaired` (or `allow-unpaired = true`) lets any server on the network play without
pairing. It is off by default, and the player says so once at startup:

```
I cli: A server must pair with this player before it can play -- pass --allow-unpaired to let any server play
```

## Stream hooks

`--hook-start` and `--hook-stop` run a shell command when a stream starts or stops,
which is useful for switching an amplifier or an indicator. The event's facts arrive in
the environment as `SENDSPIN_EVENT` and, where known, `SENDSPIN_SERVER_ID`,
`SENDSPIN_SERVER_NAME`, `SENDSPIN_SERVER_URL`, `SENDSPIN_CLIENT_ID`, and
`SENDSPIN_CLIENT_NAME`. Hooks never block playback, and a non-zero exit is logged as a
warning rather than failing the player. See
[Controlling the Player](Controlling-the-Player#the-player-driving-your-hardware-stream-hooks)
for the full behavior.

Treat hook commands as local configuration: they run with the permissions of the player
process.

## All options

Every option and config key is listed by:

```bash
sendspin-cli --help
```

Config keys are the long flag names without their dashes; see
[Configuration](Configuration#every-key).
