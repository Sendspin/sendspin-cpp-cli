# sendspin-cli

## What is it?

`sendspin-cli` is a headless audio player for the
[Sendspin](https://github.com/Sendspin/spec) protocol. It appears on your network, a
Sendspin server finds it, and it plays synchronized audio with the rest of your group.

It runs on Linux and Apple-silicon macOS. On Linux, it can also run as a system service.

## How can I run it?

### Linux and Raspberry Pi

The quickest way to install a released build is the guided Linux installer:

```bash
curl -fLO https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_linux.sh
less get_started_linux.sh
chmod +x get_started_linux.sh
./get_started_linux.sh
```

The script downloads the right release for your machine, verifies it, and guides you
through choosing an audio device. See the
[Linux quick start](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Getting-Started-on-Linux)
or [Raspberry Pi guide](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Getting-Started-on-a-Raspberry-Pi)
for the complete walkthrough.

### macOS

Download the Apple-silicon `.pkg` installer or tarball from
[Releases](https://github.com/Sendspin/sendspin-cpp-cli/releases), then follow the
[installation guide](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Installation#macos).

### Start playing

Once installed and configured, start a player from a terminal:

```bash
sendspin-cli -n living-room
```

It advertises itself on the local network and waits for a Sendspin server to connect.
For a Linux system-service installation, the quick-start guide explains how to enable
and check the service.

The options most people need beyond a name:

```bash
# Pick a sound card -- run `sendspin-cli -l` to list what this host has
sendspin-cli -n living-room -o hw:1,0

# Pin a format, for a DAC that is only happy in one shape
sendspin-cli -n living-room --audio-format flac:48000:24:2

# Connect out to a specific server, instead of waiting to be found
sendspin-cli -n living-room -s music.local
```

| Option | What it does |
|---|---|
| `-n, --name <name>` | The friendly name a server displays. Defaults to this host's name. |
| `-o, --output <device>` | Which sound card to play through. `-l` lists this host's devices and what they accept. |
| `--audio-format <codec:rate:depth:channels>` | Offers this format first, e.g. `flac:48000:24:2`. Everything else the device takes is still offered behind it. |
| `-s, --server <host[:port]>` | Connect out to a server rather than waiting to be discovered. Turns off the mDNS advertisement. |

Any of these can go in a config file instead of on the command line — see
[Configuration](https://github.com/Sendspin/sendspin-cpp-cli/wiki/Configuration).

Common local controls are available from the same host:

```bash
sendspin-cli status
sendspin-cli pause
sendspin-cli vol 40
```

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
