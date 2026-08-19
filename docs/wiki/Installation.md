# Installation

Four ways in, depending on what you have. If you are on Linux and want the short version,
[Getting Started on Linux](Getting-Started-on-Linux) does all of this in one command.

| You have | Take |
|---|---|
| A Linux box or a Raspberry Pi | The `linux-x86_64` or `linux-arm64` tarball |
| An Apple-silicon Mac | The `macos-arm64` installer `.pkg`, or the tarball |
| Anything else | [Build from source](#build-from-source) |

Everything published is on the
[Releases page](https://github.com/chrisuthe/sendspin-cpp-cli/releases). Per-commit builds
of unreleased work are under the repository's Actions tab and expire after 14 days — see
[CI](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#ci).

> **No release exists yet.** `v0.1.0` has not been tagged at the time of writing, so until
> it is, the only routes are building from source or staging a payload yourself. The
> getting-started script says the same thing rather than failing obscurely.

## What is in the archive

Every tarball is a staged `cmake --install` payload, which means **every path under `usr/`
is the path the file installs to**:

```
sendspin-cli-0.1.0-linux-arm64/
├── BUILD-INFO.txt                                          # what this build is, and what it needs
└── usr/local/
    ├── bin/sendspin-cli
    ├── lib/systemd/system/sendspin-cli.service             # Linux only
    └── share/doc/sendspin-cli/
        ├── README.md
        ├── LICENSE
        └── sendspin-cli.conf.example
```

The prefix is baked in at build time — the unit's `ExecStart` is an absolute
`/usr/local/bin/sendspin-cli` — so a binary moved somewhere else leaves the unit pointing at
nothing. Read `BUILD-INFO.txt` first; it names the runtime packages that build needs.

## Linux

```bash
# 1. Take the archive for this machine's architecture, and the checksums
VERSION=0.1.0
ARCH=$(uname -m); [ "$ARCH" = aarch64 ] && LEG=linux-arm64 || LEG=linux-x86_64
BASE=https://github.com/chrisuthe/sendspin-cpp-cli/releases/download/v$VERSION
curl -fLO "$BASE/sendspin-cli-$VERSION-$LEG.tar.gz"
curl -fLO "$BASE/SHA256SUMS"

# 2. Verify before unpacking anything
sha256sum --ignore-missing -c SHA256SUMS

# 3. Install
sudo tar -xzf "sendspin-cli-$VERSION-$LEG.tar.gz" --strip-components=1 -C / \
  "sendspin-cli-$VERSION-$LEG/usr"
sudo systemctl daemon-reload
```

`--ignore-missing` because `SHA256SUMS` covers every archive the release carries and you
have taken one of them; without it the other three are reported as failures. It is not a
way of passing with nothing checked — `sha256sum` still exits non-zero if the flag leaves
it with no file to verify.

**Naming `<name>/usr` as the member to extract is what leaves `BUILD-INFO.txt` in the
archive** instead of writing it to `/`. `--strip-components=1` drops the archive's own top
level so the rest lands where it belongs.

Runtime packages, if the binary will not start:

```bash
sudo apt install libasound2t64 libportaudio2 libavahi-compat-libdnssd1   # Debian / Ubuntu
sudo dnf install alsa-lib portaudio avahi-compat-libdns_sd               # Fedora / RHEL
```

To run it without installing anywhere, unpack it and use it in place:

```bash
tar -xzf sendspin-cli-0.1.0-linux-arm64.tar.gz
./sendspin-cli-0.1.0-linux-arm64/usr/local/bin/sendspin-cli --help
```

Then [Running as a Service](Running-as-a-Service) for the systemd half.

## macOS

The installer is the easier of the two:

```bash
sudo installer -pkg sendspin-cli-0.1.0-macos-arm64.pkg -target /
sendspin-cli --version
```

It refuses a Mac it cannot run on — the architectures are read off the binary at build time
and declared in the package — so an Intel Mac is turned away rather than told the install
worked. To undo it: remove the four files and
`sudo pkgutil --forget io.github.chrisuthe.sendspin-cli`.

Or take the tarball, and **unpack it from a terminal, not in Finder**:

```bash
tar -xzf sendspin-cli-0.1.0-macos-arm64.tar.gz
./sendspin-cli-0.1.0-macos-arm64/usr/local/bin/sendspin-cli --version
```

**Neither the binary nor the `.pkg` is signed or notarized.** They are ad-hoc signed — the
minimum an arm64 Mach-O needs to execute at all — so Gatekeeper has no developer identity
to check. Whether you notice depends entirely on the quarantine flag, which `tar` and
`unzip` do not propagate and Finder's Archive Utility does. If macOS refuses it:

```bash
xattr -d com.apple.quarantine ./sendspin-cli-0.1.0-macos-arm64/usr/local/bin/sendspin-cli
```

The full picture — including why `sudo installer` is not gated at all, and why the `.pkg`
exists despite not fixing Gatekeeper — is in
[macOS, and Gatekeeper](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#macos-and-gatekeeper)
and
[The macOS installer `.pkg`](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#the-macos-installer-pkg).
A Developer ID signature and notarization are owed and tracked as roadmap item 10.

There is no launchd job in the payload. On macOS the player runs from a shell or under a
launch agent you write.

## Raspberry Pi

The Pi takes the `linux-arm64` archive like any other arm64 Linux host, with one hard
requirement: **a 64-bit OS**. See
[Getting Started on a Raspberry Pi](Getting-Started-on-a-Raspberry-Pi).

## Build from source

For an architecture with no release — 32-bit ARM, an Intel Mac, anything not in the matrix
— or to build against a different version of the library.

```bash
sudo apt install pkg-config libasound2-dev portaudio19-dev libavahi-compat-libdnssd-dev  # Debian / Ubuntu
sudo dnf install pkgconf alsa-lib-devel portaudio-devel avahi-compat-libdns_sd-devel     # Fedora / RHEL
brew install portaudio pkgconf                                                           # macOS

git clone https://github.com/chrisuthe/sendspin-cpp-cli.git
cd sendspin-cpp-cli
cmake -B build
cmake --build build
./build/sendspin-cli --help
```

Needs CMake ≥ 3.16, a C++20 compiler, and network access on the first configure —
sendspin-cpp is pulled in with `FetchContent` at a pinned tag and fetches its own
dependencies in turn.

**The audio backends and mDNS are optional and auto-detected**, so read the configure
output rather than assuming; a missing `-dev` package does not fail a configure, it just
produces a binary that cannot do that thing:

```
-- sendspin-cli audio backends: null, stdout, alsa, portaudio
-- sendspin-cli mDNS: dns_sd (/usr/lib/aarch64-linux-gnu/libdns_sd.so)
```

To install what you built:

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local     # the prefix is chosen HERE
cmake --build build
sudo cmake --install build --component sendspin-cli
```

`--component sendspin-cli` is not garnish — without it, `cmake --install` also stages 143
files belonging to a fetched dependency. The prefix is fixed at *configure* time because
the unit's `ExecStart` names it absolutely, so reconfigure rather than passing
`--install --prefix`. Both points, at length, in
[Install](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#install).

## Next

- [Configuration](Configuration) — pick an output device and make it stick
- [Running as a Service](Running-as-a-Service) — the systemd unit
- [Controlling the Player](Controlling-the-Player) — drive it from its own host
