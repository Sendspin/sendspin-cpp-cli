# sendspin-cli

[![GitHub Release](https://img.shields.io/github/v/release/Sendspin/sendspin-cpp-cli)](https://github.com/Sendspin/sendspin-cpp-cli/releases)

A headless [Sendspin](https://www.sendspin-audio.com) player that turns this machine into a
synchronized multi-room audio target. It speaks the [Sendspin protocol](https://github.com/Sendspin/spec):
it appears on your network, a Sendspin server finds it, and it plays in sync with the rest of your group.

It runs on Linux (including Raspberry Pi), Apple-silicon macOS and Windows 10 1809 or later.

[![A project from the Open Home Foundation](https://www.openhomefoundation.org/badges/ohf-project.png)](https://www.openhomefoundation.org/)

## Download

**[Download the latest release](https://github.com/Sendspin/sendspin-cpp-cli/releases/latest)**
and pick the file for your platform. On Linux and Windows, the [guided installer](#installation)
is easier: it picks the right file, verifies it and starts the player for you.

## Installation

**Linux and Raspberry Pi (guided):**

```bash
curl -fsSL https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_linux.sh | bash
```

It downloads the release for your machine, verifies its checksum, asks for a player name
and an output device, and starts the player as a systemd service. Every command that needs
root is printed first and waits for your yes.

Prefer to read it first?

```bash
curl -fLO https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_linux.sh
less get_started_linux.sh
bash get_started_linux.sh
```

`--help` lists the flags, among them `--yes --name <name> --output <device>` for an
unattended install and `--user-service` to run as you and follow your desktop's PipeWire or
PulseAudio. The [Linux guide](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Getting-Started-on-Linux)
and the [Raspberry Pi guide](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Getting-Started-on-a-Raspberry-Pi)
have the full walkthrough.

**macOS:**

Download the Apple-silicon `.pkg` installer or tarball from
[Releases](https://github.com/Sendspin/sendspin-cpp-cli/releases), then follow the
[installation guide](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Installation#macos)
and start a player from a terminal with `sendspin-cli -n living-room`.

**Windows (guided):**

The guided Windows installer needs no administrator rights:

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_windows.ps1 -OutFile get_started_windows.ps1
notepad get_started_windows.ps1
powershell -ExecutionPolicy Bypass -File .\get_started_windows.ps1
```

It verifies the release, installs it for your user and starts it whenever you log on. See
[Getting Started on Windows](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Getting-Started-on-Windows)
for the walkthrough, including the firewall rule a server needs to reach it.

## Updating

**Linux and Windows:** re-run the guided installer from [Installation](#installation). It stops
the running player, replaces the binary and starts it again. Your config, the player's
identity and its pairings are kept.

**macOS:** install the newer `.pkg` or tarball from
[Releases](https://github.com/Sendspin/sendspin-cpp-cli/releases).

> **Note:** You don't need to uninstall first.

## Pairing

The player advertises itself on the local network and a Sendspin server finds it.
Connections are encrypted, and a server must pair with the player before it can play.
Pair it from the server, and the player shows a six-digit code to type in. Read it from
`status`:

```bash
# Linux service, as installed above
sudo sendspin-cli status --control-socket /run/sendspin-cli/control.sock

# --user-service, macOS, Windows, or a player started from your own shell
sendspin-cli status
```

`--pairing-code <8 digits>` sets a fixed code instead, confirmed with
`sendspin-cli pair confirm`. `--allow-unpaired` lets any server on the network play
without pairing. See [Pairing a Player](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Pairing-a-Player).

## Configuration

The service reads its options from a config file: `/etc/sendspin-cli.conf` for the system
service, `~/.config/sendspin-cli/config` for a user service or your own shell, and
`%APPDATA%\sendspin-cli\config` on Windows. Keys are the long flag names without their dashes:

```ini
name = kitchen
output = hw:1,0
```

Every key is also a flag for a player you run yourself, and a flag wins over the file:

```bash
sendspin-cli -n living-room
```

Edit the file, then restart the player. See
[Configuration](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Configuration) for every key
and where the file is looked for.

### Server connection

By default the player advertises itself over mDNS and waits for a server to connect. To
connect out to a known server instead, which turns the advertisement off:

```bash
sendspin-cli -n living-room -s music.local          # port defaults to 8927
sendspin-cli -n living-room -s music.local:9000
sendspin-cli -n living-room -s "mdns:Music Assistant"  # discover one by name
```

See [Connection modes](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Advanced-Usage#connection-modes).

### Audio output device

```bash
sendspin-cli -l                          # list devices and what they accept
sendspin-cli -n living-room -o hw:1,0    # play through one of them
```

`--release-device` closes the device when a stream ends and reopens it for the next one, so
other players can share an exclusive device such as an ALSA `hw:` DAC. ALSA and PortAudio only.
See [Choosing an output](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Advanced-Usage#choosing-an-output).

### Preferred audio format

```bash
sendspin-cli -n living-room --audio-format flac:48000:24:2,pcm:48000:24:2
```

Each entry is `codec:rate:depth:channels`:

- `codec`: `flac`, `pcm` or `opus` (`opus` only at `48000:16`, at most 2 channels)
- `rate`: sample rate in Hz, e.g. `48000`
- `depth`: bits per sample, e.g. `16` or `24`
- `channels`: channel count, e.g. `2`

Entries go first, in order; the rest of the player's formats still follow, so this is a
preference, not a restriction. See
[Buffering and audio format](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Advanced-Usage#buffering-and-audio-format).

### Volume

```bash
sendspin-cli -n living-room --fixed-volume
```

Plays at full volume, unmuted, and lets the server set neither: for a line-out into an
amplifier whose level is set there. See
[Configuration](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Configuration#every-key).

### Streaming input

```bash
sendspin-cli -n den --input default      # a microphone or line-in
sendspin-cli -n den --input loopback     # what this host plays
```

Off unless given. `-l` lists capture devices too. See
[Stream a microphone or line-in](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Advanced-Usage#stream-a-microphone-or-line-in)
and [Stream what this host plays](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Advanced-Usage#stream-what-this-host-plays).

### Hooks

Run a shell command when a stream starts or stops, e.g. to switch an amplifier:

```bash
sendspin-cli -o hw:1,0 \
  --hook-start 'amixer -c 1 set Master unmute' \
  --hook-stop  'amixer -c 1 set Master mute'
```

The hook gets `SENDSPIN_EVENT` (`start` or `stop`) and, where known, `SENDSPIN_SERVER_ID`,
`SENDSPIN_SERVER_NAME`, `SENDSPIN_SERVER_URL`, `SENDSPIN_CLIENT_ID` and `SENDSPIN_CLIENT_NAME`.
Hooks never block playback. See
[Stream hooks](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Controlling-the-Player#the-player-driving-your-hardware-stream-hooks).

### Debugging & troubleshooting

```bash
sendspin-cli -n living-room -d debug     # none, error, warn, info, debug, verbose
```

Or set `log-level = debug` in the config. See
[Troubleshooting](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Troubleshooting).

## Controlling the player

The same binary drives a running player on its own host:

```bash
sendspin-cli status
sendspin-cli pause
sendspin-cli vol 40
```

Against the Linux system service, add `sudo` and `--control-socket` as in [Pairing](#pairing).
See [Controlling the Player](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Controlling-the-Player)
for every subcommand.

## Running as a service (systemd, Linux)

The guided installer sets this up. To manage it:

```bash
sudo systemctl start sendspin-cli
sudo systemctl stop sendspin-cli
systemctl status sendspin-cli
journalctl -u sendspin-cli -f
```

For a `--user-service` install, use `systemctl --user` and `journalctl --user -u sendspin-cli`.
See [Running as a Service](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Running-as-a-Service).

## Uninstall

```bash
curl -fsSL https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/uninstall_linux.sh | bash
```

It removes the service and the installed files, and keeps your config and the player's
remembered state unless you pass `--purge`. For Windows, see
[Uninstalling](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Getting-Started-on-Windows#uninstalling).

## Need more help?

The [wiki](https://github.com/Sendspin/sendspin-cpp-cli/wiki) is the complete end-user
reference. It covers installation, configuration, service management, local controls,
troubleshooting, and [advanced usage](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Advanced-Usage).
Run `sendspin-cli --help` for the command-line reference.

## How can I contribute?

Contributions, bug reports, and documentation improvements are welcome. Read
[contributors.md](contributors.md) for the development setup, test commands, project
layout, CI, and release process. Wiki pages are authored in this repository under
[`docs/wiki/`](docs/wiki), so documentation changes can be reviewed in a pull request.

## License

[Apache 2.0](LICENSE)
