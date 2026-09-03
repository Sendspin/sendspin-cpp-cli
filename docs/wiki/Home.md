# sendspin-cli

A headless **Sendspin audio player** for Linux and macOS. It advertises itself over
mDNS, waits for a Sendspin server to find it, and plays what it is sent in sync with
every other player in the group.

## Start here

| | |
|---|---|
| [Getting Started on Linux](Getting-Started-on-Linux) | One script, from nothing to a player on the network |
| [Getting Started on a Raspberry Pi](Getting-Started-on-a-Raspberry-Pi) | The same script, plus what a Pi does differently |
| [Installation](Installation) | Every way in: release archive, macOS `.pkg`, source |
| [Configuration](Configuration) | The config file, and what the player remembers by itself |
| [Controlling the Player](Controlling-the-Player) | `sendspin-cli pause` and the other thirteen subcommands |
| [Running as a Service](Running-as-a-Service) | The systemd unit, the account it runs as, drop-ins, and reading the log |
| [Advanced Usage](Advanced-Usage) | Connection modes, output selection, logging, buffering, and stream hooks |
| [Troubleshooting](Troubleshooting) | It starts and makes no sound, and the rest |

## What it does, in one screen

```console
$ sendspin-cli -n living-room
I cli: sendspin-cli 0.1.0 listening on port 8928 as "living-room" (output: default, mDNS: dns_sd (avahi-compat))
I mdns: advertising _sendspin._tcp as "living-room" on port 8928 (path /sendspin)
```

That is the whole of the usual setup: nothing to configure on either end. A Sendspin
server discovers the advertisement and dials in. `-s <server>` inverts it and makes this
player the one dialling, which the protocol treats as the other of two mutually exclusive
modes — see [Connection modes](Advanced-Usage#connection-modes).

Audio goes out through ALSA (the Linux default) or PortAudio (the cross-platform one, and
the only way to make noise on macOS), with volume applied in software on a curve the spec
names. The player is also driven from its own host over a Unix socket:

```console
$ sendspin-cli status
$ sendspin-cli pause
$ sendspin-cli vol 40
```

## Supported platforms

| Platform | Architecture | How |
|---|---|---|
| Linux | `x86_64`, `arm64`, `armv7`, `armv6` | Release tarball, or `scripts/get_started_linux.sh` |
| macOS | Apple silicon (`arm64`) | Release tarball or installer `.pkg` |
| Raspberry Pi | `arm64`, or `armv7`/`armv6` on a 32-bit OS | The Linux tarball, same as any other Linux host |

The macOS builds are made on the `macos-14` CI runner and declare no minimum OS version;
what the installer `.pkg` does check is the architecture, read off the binary with `lipo` at
build time, so it turns an Intel Mac away rather than reporting success.

`armv6` covers the ARM1176 boards — a Pi Zero, a Pi Zero W, an original Pi — and is built
inside an emulated Raspbian container rather than cross-compiled, because Debian and Ubuntu
armhf are an ARMv7-A port. There is no Intel-Mac build; that one is recorded in
[`docs/ROADMAP.md`](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/docs/ROADMAP.md),
item 12.
Anything else builds from source.

## Where things live

- The [wiki](Home) is the complete end-user reference. It covers installation,
  configuration, local control, services, troubleshooting, and
  [advanced usage](Advanced-Usage).
- **[`docs/ROADMAP.md`](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/docs/ROADMAP.md)**
  is what is built, what is not, and what was actually tested rather than reasoned about.
- **`sendspin-cli --help`** is the flag reference, and the config file's reference too:
  every config key is a long flag name minus its dashes.

## Editing these pages

**This wiki is generated. Do not edit it here — the edit will be overwritten.**

The pages are authored in the repository at
[`docs/wiki/`](https://github.com/Sendspin/sendspin-cpp-cli/tree/main/docs/wiki) and
mirrored here by
[`.github/workflows/wiki.yml`](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/.github/workflows/wiki.yml)
on every push to `main`. A page removed there is removed here; a page changed here is put
back on the next push.

That is deliberate rather than awkward. A wiki edit lands with no pull request, no review
and no CI, which for a document that tells people what commands to run as root is the wrong
default. Send a documentation fix as a pull request against `docs/wiki/`, the same way a
code fix goes, and it arrives here when it merges.
