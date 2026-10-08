# Getting Started on Linux

From nothing to a player your Sendspin server can find. On a Raspberry Pi, read
[Getting Started on a Raspberry Pi](Getting-Started-on-a-Raspberry-Pi) instead — it is this
page plus the handful of things a Pi does differently.

**You need:** a Linux host (`x86_64`, `arm64`, or `armv7`/`armv6` on a 32-bit Raspberry Pi
OS), systemd, a sound card, and root.

## The short way

```bash
curl -fsSL https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_linux.sh | bash
```

Or read it before it runs anything:

```bash
curl -fLO https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_linux.sh
less get_started_linux.sh
bash get_started_linux.sh
```

Both behave the same. It installs the release for this machine's architecture, asks for a
player name and an output device, writes them to the config, and starts the service. Piped
into `bash`, its questions are still asked on your terminal. It does not run anything as
root without printing the exact commands first and waiting for you to say yes:

```
==> These are the commands that need root

  sudo tar -xzf /tmp/tmp.XXXX/sendspin-cli-0.3.0-linux-arm64.tar.gz --strip-components=1 -C / sendspin-cli-0.3.0-linux-arm64/usr
  sudo systemd-sysusers
  sudo systemctl daemon-reload
  sudo systemctl enable sendspin-cli
  sudo /usr/local/bin/sendspin-cli -l
  sudo install -m 0644 /tmp/tmp.XXXX/sendspin-cli.conf /etc/sendspin-cli.conf
  sudo systemctl restart sendspin-cli

Run them? [y/N]
```

The config it installs is composed in the temporary directory from the annotated example
plus your two answers, and the lines it sets are printed again just before it is copied.

| Flag | What it does |
|---|---|
| `--version v0.3.0` | Install that release instead of the newest. |
| `--name <name>` | The player's name, instead of being asked. |
| `--output <device>` | The output device, instead of being asked. |
| `--user-service` | Run as you rather than as the `sendspin-cli` account — see [below](#as-a-system-service-or-as-you). |
| `--system-service` | The default; also switches a user-service install back. |
| `--yes` | Ask nothing: run the root commands as printed, and take the name and output only from the flags. |

Piped, flags go after `bash -s --`:

```bash
curl -fsSL https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_linux.sh | bash -s -- --yes --name kitchen --output hw:1,0
```

With no terminal to ask on and no `--yes`, it refuses and installs nothing.

### What it actually does

1. **Checks the architecture** — the userland's, read from `dpkg --print-architecture`
   rather than from `uname -m`, which names the kernel and disagrees with the userland on a
   32-bit Raspberry Pi OS. `amd64`, `arm64` and `armhf` have builds. `armhf` then splits on
   `uname -m`, which is the one question the userland cannot answer: `armv6l` — a Pi Zero, a
   Pi Zero W, an original Pi — takes `linux-armv6`, and everything else `linux-armv7`. ARMv5
   and older is refused with the reason rather than an "unsupported" shrug.
2. **Finds the newest release** and downloads that archive plus `SHA256SUMS`.
3. **Verifies the checksum**, and stops without installing anything if it does not match.
4. **Checks the binary can load here.** Shared libraries this host lacks are mapped to
   their `apt`, `dnf` or `pacman` packages and the install command joins the list you
   confirm. A library it cannot map, a host with none of those package managers, or a glibc
   older than the build needs stops it there, naming what is missing, with nothing installed.
5. **Unpacks it into `/`** with the member-selected `tar` form, so `BUILD-INFO.txt` stays in
   the archive.
6. **Runs `systemd-sysusers`**, which creates the unprivileged `sendspin-cli` account the
   unit runs as out of the declaration the payload just installed — see
   [Running as a Service](Running-as-a-Service#it-runs-as-its-own-account).
7. **Enables the unit.**
8. **Asks for a name and an output device**, after listing what this host can play
   through, and writes `name` and `output` into `/etc/sendspin-cli.conf` — seeded from the
   annotated example if you have no config yet. A key your config already sets is never
   asked for and never replaced.
9. **Starts the service**, waits two seconds, and either confirms it is running or shows
   the journal's account of why it is not.

Re-running it is how you upgrade: it overwrites the same paths and restarts the service.
On a host whose config already names an `output` it asks nothing but the root confirmation.

### As a system service, or as you

On a fresh host the script asks which:

- **A system service** (the default, `--system-service`). The hardened unit, running as the
  unprivileged `sendspin-cli` account, configured in `/etc/sendspin-cli.conf`. It has no
  desktop session, so name a card — `hw:1,0` — rather than `default`. Right for a headless
  box or a Pi.
- **A user service** (`--user-service`). Runs as you under `systemctl --user`, configured in
  `~/.config/sendspin-cli/config`, so `output = default` follows your session's PipeWire or
  PulseAudio. The script enables lingering so it starts at boot and survives logout, and adds
  you to the `audio` group for direct `hw:` devices. Subcommands need no `sudo` and no
  flags: `sendspin-cli status`.

Both listen on the same port, so choosing one stops and disables the other. See
[Running as a Service](Running-as-a-Service#a-user-unit-instead) for what the user unit is.

### When it does not start the player

One case: `--yes` with no `--output`, on a host whose config names none. There is nobody to
ask, and a system unit started without a card fails and is retried every five seconds —
`Restart=on-failure`, and no session for ALSA's `default` to follow. So the unit is enabled,
the device list is printed, and the two commands that finish the job are the last thing it
says. Answering the output question with nothing ends the same way.

### Removing it

```bash
curl -fsSL https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/uninstall_linux.sh | bash
```

See [Uninstalling](Running-as-a-Service#uninstalling).

## The long way

If you would rather do it by hand, or the script refuses this host:

```bash
# 1. Install — see the Installation page for the checksum step
sudo tar -xzf sendspin-cli-0.1.0-linux-arm64.tar.gz --strip-components=1 -C / \
  sendspin-cli-0.1.0-linux-arm64/usr

# 2. Find a device
sendspin-cli -l

# 3. Tell it which one
sudo cp /usr/local/share/doc/sendspin-cli/sendspin-cli.conf.example /etc/sendspin-cli.conf
sudo nano /etc/sendspin-cli.conf            # output = hw:1,0

# 4. Create the account the unit runs as, then start it
sudo systemd-sysusers
sudo systemctl daemon-reload
sudo systemctl enable --now sendspin-cli
```

`systemd-sysusers` is not optional and is not a `useradd` you have to compose — the payload
installs the declaration, and this reads it. Leave it out and `systemctl status` reports
`217/USER`.

## Choosing an output

`sendspin-cli -l` lists every device this host can play through, and for each one the rates,
formats and channel counts it really accepts:

```
  hw:CARD=Headphones,DEV=0
      bcm2835 Headphones
      rates:    8000 11025 16000 22050 32000 44100 48000
      formats:  S16_LE
      channels: 2
```

Put the name in the config file as `output`, without the dashes of the flag it mirrors:

```ini
output = hw:1,0
```

Three forms are worth knowing, and there are more in
[Choosing an output](Advanced-Usage#choosing-an-output):

| Value | What it means |
|---|---|
| `hw:1,0` | that card directly, bypassing any sound server |
| `plughw:1,0` | the same card, letting ALSA convert rate and format for it |
| `default` | follow the host's own configuration — PipeWire, PulseAudio or bare hardware |

`default` is the right answer from a login shell and usually the wrong one under a system
unit, which has no session to follow. Under the **user** service (`--user-service`) it is
right again, because there the session and its sound server exist.

## Check it worked

```bash
systemctl status sendspin-cli
journalctl -u sendspin-cli -f
```

A healthy start looks like this:

```
I cli: sendspin-cli 0.1.0 listening on port 8928 as "kitchen" (output: hw:1,0, mDNS: dns_sd (avahi-compat))
I mdns: advertising _sendspin._tcp as "kitchen" on port 8928 (path /sendspin)
I control: Listening on /run/sendspin-cli/control.sock
```

Then ask the player itself:

```bash
sudo sendspin-cli status --control-socket /run/sendspin-cli/control.sock
```

`sudo`, and the flag, are both needed under the system unit: the socket is mode `0600` and
belongs to the `sendspin-cli` account, and root has no `$XDG_RUNTIME_DIR` for the default path
to come from. Root can read the socket regardless, not being subject to the mode. Adding
`control-socket = /run/sendspin-cli/control.sock` to the config saves repeating the flag —
see [Controlling the Player](Controlling-the-Player).

## Now find it from your server

The player advertises `_sendspin._tcp` and waits. Open your Sendspin controller and it
should appear under the name it logged — which is `name`, falling back to this host's name.
A server has to pair with it before it may play; the script prints the `pair-token` command
for that as its last step, and [Pairing a Player](Pairing-a-Player) has the rest.

To go the other way and have the player dial the server instead, set `server` in the config:

```ini
# Pick one — a repeated key takes its last value, so keep a single `server` line:
server = 192.168.1.10          # a host, port 8927 assumed
#server = mdns:Music Assistant  # or discover one by its advertised name
```

Any `server` value turns the mDNS advertisement off. That is the spec's rule rather than a
preference here, and the two modes are mutually exclusive by design — see
[Connection modes](Advanced-Usage#connection-modes).

## Next

- [Configuration](Configuration) — every key, and what the player remembers by itself
- [Controlling the Player](Controlling-the-Player) — `pause`, `vol`, `delay` and the rest
- [Running as a Service](Running-as-a-Service) — the account it runs as, drop-ins, and what
  the hardening block takes away
- [Troubleshooting](Troubleshooting) — when it starts and stays silent
