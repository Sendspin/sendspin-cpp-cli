# sendspin-cli

## What is it?

`sendspin-cli` is a headless audio player for the
[Sendspin](https://github.com/Sendspin/spec) protocol. It appears on your network, a
Sendspin server finds it, and it plays synchronized audio with the rest of your group.

It runs on Linux and Apple-silicon macOS. On Linux, it can also run as a system service.

> **Status: early scaffold.** The player works, but not every planned feature is
> available yet. See the [roadmap](docs/ROADMAP.md) for the current status.

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

[MIT](LICENSE)
