# Getting Started on a Raspberry Pi

A Pi makes an excellent Sendspin endpoint: it is quiet, it is cheap, and one per room is the
whole point of a synchronized multi-room protocol. Installing on one is
[Getting Started on Linux](Getting-Started-on-Linux) — a Pi is an ordinary Linux box and takes
whichever archive its architecture names — plus the five things on this page.

## 1. Check which build your OS wants

Two archives serve a Pi, and `uname -m` is what picks between them:

```console
$ uname -m
aarch64
```

| It says | What installs |
|---|---|
| `aarch64` | `linux-arm64`, the 64-bit build. |
| `armv7l` | `linux-armv7`, the 32-bit build. |
| `armv6l` | Nothing. See below. |

Either archive gets you a working player, and the getting-started script chooses for you. A
64-bit OS is still the better answer on hardware that can run one: `linux-arm64` is built and
run on a real arm64 machine, systemd unit and all, where `linux-armv7` is cross-compiled and
its suite run under emulation. In Raspberry Pi Imager the 64-bit builds are under **Raspberry
Pi OS (other)**.

| Model | 64-bit capable |
|---|---|
| Pi 5, Pi 4, Pi 400, Pi 3, Pi Zero 2 W, CM3/CM4/CM5 | Yes |
| Pi 1, Pi Zero, Pi Zero W, and Pi 2 boards before v1.2 | **No** |

**`armv6l` has no build.** A Pi Zero, a Pi Zero W and an original Pi are ARMv6, and the
32-bit archive is compiled for ARMv7 — its instructions are illegal on those cores, so there
is nothing to install and the getting-started script says so rather than handing you a binary
that traps.
[`docs/ROADMAP.md`](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/docs/ROADMAP.md)
item 12 records what an ARMv6 leg would take. Building from source on the Pi itself works in
the meantime.

## 2. Install

Exactly as on any Linux host:

```bash
curl -fLO https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_linux.sh
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

`/dev/snd/*` is `root:audio` mode `0660`, so `audio` membership is what decides whether a
player can open a card at all. Whoever runs it needs it.

**As a service, this is already arranged.** The unit runs as an unprivileged `sendspin-cli`
account, and the declaration the payload installs puts that account in `audio` in the same
file that creates it — the two are owed together, and arrive together. What you do have to do
once is turn the declaration into an account:

```bash
sudo systemd-sysusers
```

The getting-started script does that for you. Skipping it is a unit that does not start and a
`systemctl status` reading `217/USER`; it is not a player that starts and stays silent. See
[Running as a Service](Running-as-a-Service#it-runs-as-its-own-account).

**From your own shell, it is on you.** Put yourself in the `audio` group once, then log out
and back in — group membership is only picked up at login:

```bash
sudo usermod -aG audio "$USER"
```

Without it the player starts and opens nothing. The `sendspin-cli` account's membership does
nothing for a player running as you.

## 5. Things a Pi does that a server does not

- **SD cards wear out**, and the state file is rewritten whole on every *distinct* volume a
  server sends. A repeat of the current value is skipped, but a slider drag is one rewrite
  per step; debouncing is a known gap, listed under
  [`docs/ROADMAP.md`](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/docs/ROADMAP.md)
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
  [Buffering, and what gets advertised](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/README.md#buffering-and-what-gets-advertised).
- **`avahi-daemon` is what provides mDNS on a Pi**, and Raspberry Pi OS ships it running. If
  you have turned it off, the player warns and retries rather than failing — but nothing will
  discover it until it is back.
- **One player per Pi.** Two on one host need different `--id`, different `--port`,
  different `--state-dir` and different control sockets. It works; it is just not what a
  Pi is usually for.

## Next

- [Configuration](Configuration) — every key the config file takes
- [Controlling the Player](Controlling-the-Player) — including `delay`, for a Pi feeding an
  amplifier that adds latency of its own
- [Troubleshooting](Troubleshooting) — silent player, no discovery, and the rest
