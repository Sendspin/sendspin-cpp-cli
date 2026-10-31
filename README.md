# sendspin-cli

`sendspin-cli` is a headless audio player for the
[Sendspin](https://github.com/Sendspin/spec) protocol. It appears on your network, a
Sendspin server finds it, and it plays synchronized audio with the rest of your group.

It runs on Linux, Apple-silicon macOS and Windows 10 1809 or later.

## Quick start

### Linux and Raspberry Pi

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

### macOS

Download the Apple-silicon `.pkg` installer or tarball from
[Releases](https://github.com/Sendspin/sendspin-cpp-cli/releases), then follow the
[installation guide](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Installation#macos)
and start a player from a terminal with `sendspin-cli -n living-room`.

### Windows

The guided Windows installer needs no administrator rights:

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_windows.ps1 -OutFile get_started_windows.ps1
notepad get_started_windows.ps1
powershell -ExecutionPolicy Bypass -File .\get_started_windows.ps1
```

It verifies the release, installs it for your user and starts it whenever you log on. See
[Getting Started on Windows](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Getting-Started-on-Windows)
for the walkthrough, including the firewall rule a server needs to reach it.

### Pair it with your server

The player advertises itself on the local network and a Sendspin server finds it.
Connections are encrypted, and a server must pair with the player before it can play.
Print the token to paste into the server:

```bash
# Linux service, as installed above
sudo sendspin-cli pair-token --control-socket /run/sendspin-cli/control.sock

# --user-service, macOS, Windows, or a player started from your own shell
sendspin-cli pair-token
```

[Pairing a player](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Pairing-a-Player)
covers that and the two code-based ways. `--allow-unpaired` lets any server on the network
play without pairing.

## Everyday use

The service reads its options from a
[config file](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Configuration); the same
options are flags when you run the player yourself:

```bash
sendspin-cli -n living-room
```

The options most people need beyond a name:

```bash
# Pick a sound card -- run `sendspin-cli -l` to list what this host has
sendspin-cli -n living-room -o hw:1,0

# Prefer formats, in order -- the server uses the first one it can encode
sendspin-cli -n living-room --audio-format flac:48000:24:2,pcm:48000:24:2

# Connect out to a specific server, instead of waiting to be found
sendspin-cli -n living-room -s music.local

# Also stream this host's microphone or line-in to the server
sendspin-cli -n den --input default
```

| Option | What it does |
|---|---|
| `-n, --name <name>` | The friendly name a server displays. Defaults to this host's name. |
| `-o, --output <device>` | Which sound card to play through. `-l` lists this host's devices and what they accept. |
| `--audio-format <codec:rate:depth:channels>[,...]` | Formats to offer first, in priority order, e.g. `flac:48000:24:2,pcm:48000:24:2`. Everything else the player normally offers still follows, and a server uses the first format it can encode, so it can still fall back to a later one. This sets a preference, not a restriction. |
| `--input <device>` | Audio to stream to the server: a microphone or line-in, e.g. `default`, `hw:1,0`, `pulse`, `pipewire:<node>` or `portaudio`, or `coreaudio` on macOS; or `loopback` for what this host plays (on macOS without the player's own playback, on Linux with it). `-l` lists them. Off unless given. See [Stream a microphone or line-in](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Advanced-Usage#stream-a-microphone-or-line-in) and [Stream what this host plays](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Advanced-Usage#stream-what-this-host-plays). |
| `--allow-unpaired` | Let a server play without pairing with this player first. Off by default. |
| `--fixed-volume` | Play at full volume, unmuted, and let the server set neither — for a line-out into an amplifier whose level is set there. |
| `--release-device` | Close the output device when a stream ends and reopen it when the next one starts, so other players can take turns on an exclusive device such as an ALSA `hw:` DAC. Off by default: between streams another app can take the device, and the next stream is then discarded. ALSA and PortAudio only. |
| `--pairing-code <8 digits>` | A fixed code a server can pair with, confirmed with `sendspin-cli pair confirm`. Without it the player shows a fresh 6-digit code per attempt. |
| `-s, --server <host[:port]>` | Connect out to a server rather than waiting to be discovered, or `mdns:[<name>]` to discover one over mDNS. Turns off the mDNS advertisement. |

Common local controls are available from the same host:

```bash
sendspin-cli status
sendspin-cli pause
sendspin-cli vol 40
```

## Uninstall

```bash
curl -fsSL https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/uninstall_linux.sh | bash
```

On Windows:

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/uninstall_windows.ps1 -OutFile uninstall_windows.ps1
powershell -ExecutionPolicy Bypass -File .\uninstall_windows.ps1
```

Each removes the service and the installed files, and keeps your config and the player's
remembered state unless you pass `--purge` (`-Purge` on Windows).

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
