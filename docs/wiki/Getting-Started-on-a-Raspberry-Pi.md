# Getting Started on a Raspberry Pi

A Pi makes an excellent Sendspin endpoint: it is quiet, it is cheap, and one per room is the
whole point of a synchronized multi-room protocol. Installing on one is
[Getting Started on Linux](Getting-Started-on-Linux) — a Pi is an arm64 Linux box and takes
the same `linux-arm64` archive an arm64 server does — plus the five things on this page.

## 1. You need a 64-bit OS. This is not negotiable

**The builds are `arm64` only. There is no 32-bit ARM build, and none is coming from CI.**
The matrix has no armv7 or 32-bit Pi leg, which is recorded in
[`docs/ROADMAP.md`](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/docs/ROADMAP.md),
item 12.

Check what you are running before anything else:

```console
$ uname -m
aarch64
```

| It says | Where you are |
|---|---|
| `aarch64` | Good. Carry on. |
| `armv7l`, `armv6l` | A 32-bit userland. No archive will install. |

A 32-bit answer on 64-bit hardware is the common case — Raspberry Pi OS shipped a 32-bit
userland by default for years, and plenty of installed cards still run it. The fix is to
reimage with **Raspberry Pi OS (64-bit)**, or *Raspberry Pi OS Lite (64-bit)* for a headless
player, which is what a Sendspin endpoint wants anyway. In Raspberry Pi Imager the 64-bit
builds are under **Raspberry Pi OS (other)**.

| Model | 64-bit capable |
|---|---|
| Pi 5, Pi 4, Pi 400, Pi 3, Pi Zero 2 W, CM3/CM4/CM5 | Yes |
| Pi 1, Pi Zero, Pi Zero W, and Pi 2 boards before v1.2 | **No** — build from source, or use other hardware |

If your board is not on either row, do not go looking it up: install a 64-bit image and run
`uname -m`. That answer is the only one that decides anything here.

The getting-started script refuses a 32-bit userland with this whole answer rather than an
"unsupported architecture", because it is the single most common way a Pi install goes
wrong.

## 2. Install

Exactly as on any Linux host:

```bash
curl -fLO https://raw.githubusercontent.com/chrisuthe/sendspin-cpp-cli/main/scripts/get_started_linux.sh
less get_started_linux.sh          # read it before it runs things as root
chmod +x get_started_linux.sh
./get_started_linux.sh
```

The script detects the Pi from `/proc/device-tree/model` and adds the notes below to what it
prints at the end. Everything on
[Getting Started on Linux](Getting-Started-on-Linux) — what it does, why it enables the unit
without starting it, and the by-hand equivalent — applies unchanged.

## 3. The Pi has several sound cards, and you must pick one

The headphone jack and each HDMI output are separate ALSA cards, and there is no useful
"just play" default under a system unit. Ask:

```console
$ sendspin-cli -l
  hw:CARD=Headphones,DEV=0
      bcm2835 Headphones
      rates:    8000 11025 16000 22050 32000 44100 48000
      formats:  S16_LE
      channels: 2
  hw:CARD=vc4hdmi0,DEV=0
      vc4-hdmi-0
      ...
```

Then name it in `/etc/sendspin-cli.conf`:

```ini
output = hw:1,0
```

`output = default` is what usually leaves a Pi silent as a service, for the reason on the
Linux page: no user session, so nothing for ALSA's `default` to follow.

**On a USB DAC or a HAT**, the card is in that same list — a HiFiBerry, an IQaudIO, a
Pi-DAC and a plain USB DAC all appear as ordinary ALSA cards once their overlay is enabled
in `/boot/firmware/config.txt`. Enable the overlay, reboot, and run `-l` again; this player
has nothing Pi-HAT-specific to configure.

**The 3.5 mm jack is not a good listening output.** It is a PWM output on the SoC and it
sounds like one. It is fine for proving the chain works. For anything else, use HDMI to a
receiver, a USB DAC, or an I²S HAT.

## 4. `/dev/snd` belongs to root and the `audio` group

The systemd unit runs as root, which already has the sound card, so a service install needs
nothing here. Two cases do:

**Running it from your own shell.** Put yourself in the `audio` group once, then log out and
back in — group membership is only picked up at login:

```bash
sudo usermod -aG audio "$USER"
```

Without it the player starts and opens nothing, because `/dev/snd/*` is `root:audio` mode
`0660`.

**Running the service as a non-root user.** Both lines together, never one alone:

```bash
sudo systemctl edit sendspin-cli
```

```ini
[Service]
User=sendspin
SupplementaryGroups=audio
```

A user with no `audio` membership is a player that starts and cannot open a device. Create
the user first (`sudo useradd --system --no-create-home sendspin`) and chown any existing
`/var/lib/sendspin-cli` to it. See
[Running as a Service](Running-as-a-Service).

## 5. Things a Pi does that a server does not

- **SD cards wear out**, and the state file is rewritten whole on every *distinct* volume a
  server sends. A repeat of the current value is skipped, but a slider drag is one rewrite
  per step; debouncing is a known gap, listed under
  [`docs/ROADMAP.md`](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/docs/ROADMAP.md)
  item 8. If that worries you, point the state at the runtime directory, which is already a
  tmpfs — and accept that volume, mute and the static delay are then forgotten across
  reboots. It has to be a **drop-in** rather than a config key, because the unit passes
  `--state-dir` itself and the command line wins (`sudo systemctl edit sendspin-cli`):

  ```ini
  [Service]
  ExecStart=
  ExecStart=/usr/local/bin/sendspin-cli --control-socket /run/sendspin-cli/control.sock --state-dir /run/sendspin-cli
  ```

  `ExecStart=` on its own line first, which is systemd's rule for clearing any list-valued
  directive.
- **Wi-Fi power saving breaks mDNS.** A Pi that vanishes from your controller after a few
  idle minutes and comes back when you ping it is the wireless NIC sleeping, not this
  player. `sudo iw dev wlan0 set power_save off` is the usual fix; Ethernet avoids it
  entirely, and a fixed endpoint deserves a cable.
- **Underruns on a busy Pi** show as clicks or dropouts. Raise the buffer:
  `buffer-ms = 250` in the config. The default is 100 ms and the range is 10–2000; see
  [Buffering, and what gets advertised](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#buffering-and-what-gets-advertised).
- **`avahi-daemon` is what provides mDNS on a Pi**, and Raspberry Pi OS ships it running. If
  you have turned it off, the player warns and retries rather than failing — but nothing will
  discover it until it is back.
- **One player per Pi.** Two on one host need different `--port`, different `--state-dir`
  and different control sockets. It works; it is just not what a Pi is usually for.

## Next

- [Configuration](Configuration) — every key the config file takes
- [Controlling the Player](Controlling-the-Player) — including `delay`, for a Pi feeding an
  amplifier that adds latency of its own
- [Troubleshooting](Troubleshooting) — silent player, no discovery, and the rest
