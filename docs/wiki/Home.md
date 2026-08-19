# sendspin-cli

A headless **Sendspin audio player** for Linux and macOS — what squeezelite is to
Lyrion/Logitech Media Server, `sendspin-cli` is to the
[Sendspin](https://github.com/Sendspin/spec) protocol. It advertises itself over mDNS,
waits for a Sendspin server to find it, plays what it is sent in sync with every other
player in the group, and takes its flags and its ergonomics from squeezelite so that
muscle memory carries over.

> **Status: early scaffold.** The player works; not everything on the roadmap is built.
> [`docs/ROADMAP.md`](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/docs/ROADMAP.md)
> is the honest list of what is and is not done.

## Start here

| | |
|---|---|
| [Getting Started on Linux](Getting-Started-on-Linux) | One script, from nothing to a player on the network |
| [Getting Started on a Raspberry Pi](Getting-Started-on-a-Raspberry-Pi) | The same script, plus what a Pi does differently |
| [Installation](Installation) | Every way in: release archive, macOS `.pkg`, source |
| [Configuration](Configuration) | The config file, and what the player remembers by itself |
| [Controlling the Player](Controlling-the-Player) | `sendspin-cli pause` and the other thirteen subcommands |
| [Running as a Service](Running-as-a-Service) | The systemd unit, drop-ins, and reading the log |
| [Troubleshooting](Troubleshooting) | It starts and makes no sound, and the rest |

## What it does, in one screen

```console
$ sendspin-cli -n living-room
I cli: sendspin-cli 0.1.0 listening on port 8928 as "living-room" (output: default)
I mdns: advertising _sendspin._tcp as "living-room" on port 8928 (path /sendspin)
```

That is the whole of the usual setup: nothing to configure on either end. A Sendspin
server discovers the advertisement and dials in. `-s <server>` inverts it and makes this
player the one dialling, which the protocol treats as the other of two mutually exclusive
modes — see
[The two connection modes](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#the-two-connection-modes).

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
| Linux | `x86_64`, `arm64` | Release tarball, or `scripts/get_started_linux.sh` |
| macOS 12+ | Apple silicon (`arm64`) | Release tarball or installer `.pkg` |
| Raspberry Pi | `arm64` only — **a 64-bit OS is required** | The Linux tarball, same as any arm64 host |

There is no 32-bit ARM build and no Intel-Mac build. The CI matrix has no armv7, 32-bit Pi
or macOS `x86_64` leg, which is recorded in
[`docs/ROADMAP.md`](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/docs/ROADMAP.md),
item 12.
Anything else builds from source.

## Where things live

- **[`README.md`](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md)** is
  the reference, and ships inside every archive at
  `usr/local/share/doc/sendspin-cli/README.md`. It explains *why* the player behaves as it
  does — the two connection modes, how `-o` resolves its argument, why `vol` is the group's
  volume and not this box's. These wiki pages link into it rather than restating it, so
  there is one copy of each argument and it is the copy an offline tarball holder also has.
- **[`docs/ROADMAP.md`](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/docs/ROADMAP.md)**
  is what is built, what is not, and what was actually tested rather than reasoned about.
- **`sendspin-cli --help`** is the flag reference, and the config file's reference too:
  every config key is a long flag name minus its dashes.

## Editing these pages

**This wiki is generated. Do not edit it here — the edit will be overwritten.**

The pages are authored in the repository at
[`docs/wiki/`](https://github.com/chrisuthe/sendspin-cpp-cli/tree/main/docs/wiki) and
mirrored here by
[`.github/workflows/wiki.yml`](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/.github/workflows/wiki.yml)
on every push to `main`. A page removed there is removed here; a page changed here is put
back on the next push.

That is deliberate rather than awkward. A wiki edit lands with no pull request, no review
and no CI, which for a document that tells people what commands to run as root is the wrong
default. Send a documentation fix as a pull request against `docs/wiki/`, the same way a
code fix goes, and it arrives here when it merges.
