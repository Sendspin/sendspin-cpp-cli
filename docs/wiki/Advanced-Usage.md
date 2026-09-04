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
pair split on the first colon (`portaudio:2`, `pulse:<sink>`, `pipewire:<node>`), or an
ALSA PCM name such as `hw:1,0`, `plughw:1,0`, or `default`. `plughw:` lets ALSA convert
rate and format for a device that refuses the stream as it arrives.

`default` follows the host's normal audio configuration. Under a system service, name
a hardware device such as `hw:1,0` instead; the service does not have a logged-in
desktop audio session.

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

`--static-delay <0-5000>` declares how much latency this endpoint's hardware adds
*after* the audio port, so the player hands audio over that much earlier. It is a
first-run default only: once a server or [`delay`](Controlling-the-Player) has set one,
the remembered value wins.

`--audio-format <codec:rate:depth:channels>` pins a preferred format to the front of
the advertised list, for a DAC that is only happy in one shape:

```bash
sendspin-cli --audio-format flac:48000:24:2
```

The player refuses to start if the device cannot offer the pinned format. Run
`sendspin-cli -l` to see what it accepts.

## Identity

`--id` sets the stable client id a server files this player's volume, group, and
pairing under; `-n` is only the displayed name. Without it, the id is derived from the
network interface MAC, which two players on one host would share. Run two players on
one host with their own `--id`, `--port`, `--state-dir`, and control socket.

`--manufacturer` and `--product-name` set what the player reports to servers, for a
product that embeds this player and should be listed as itself.

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
