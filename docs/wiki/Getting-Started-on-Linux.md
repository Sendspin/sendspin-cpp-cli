# Getting Started on Linux

From nothing to a player your Sendspin server can find. On a Raspberry Pi, read
[Getting Started on a Raspberry Pi](Getting-Started-on-a-Raspberry-Pi) instead — it is this
page plus the handful of things a Pi does differently.

**You need:** a Linux host (`x86_64`, `arm64`, or `armv7` on a 32-bit Raspberry Pi OS),
systemd, a sound card, and root.

## The short way

```bash
curl -fLO https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_linux.sh
less get_started_linux.sh          # it is about to run things as root; read it
chmod +x get_started_linux.sh
./get_started_linux.sh
```

It installs the release for this machine's architecture and sets the service up. It does
not pipe into a shell, and it does not run anything as root without printing the exact
commands first and waiting for you to say yes:

```
==> These are the commands that need root

  sudo tar -xzf /tmp/tmp.XXXX/sendspin-cli-0.1.0-linux-arm64.tar.gz --strip-components=1 -C / sendspin-cli-0.1.0-linux-arm64/usr
  sudo cp /usr/local/share/doc/sendspin-cli/sendspin-cli.conf.example /etc/sendspin-cli.conf
  sudo systemd-sysusers
  sudo systemctl daemon-reload
  sudo systemctl enable sendspin-cli
  sudo /usr/local/bin/sendspin-cli -l

Run them? [y/N]
```

`--yes` skips the prompt, and is required when stdin is not a terminal — a script that
cannot ask refuses rather than assuming. `--version v0.1.0` installs a specific release
instead of the newest.

### What it actually does

1. **Checks the architecture** — the userland's, read from `dpkg --print-architecture`
   rather than from `uname -m`, which names the kernel and disagrees with the userland on a
   32-bit Raspberry Pi OS. `amd64`, `arm64` and `armhf` have builds. ARMv6 — a Pi Zero, a Pi
   Zero W, an original Pi — does not, and is refused with the reason rather than an
   "unsupported" shrug.
2. **Finds the newest release** and downloads that archive plus `SHA256SUMS`.
3. **Verifies the checksum**, and stops without installing anything if it does not match.
4. **Unpacks it into `/`** with the member-selected `tar` form, so `BUILD-INFO.txt` stays in
   the archive.
5. **Copies the annotated example config** to `/etc/sendspin-cli.conf` if you have none.
   Every line in it is commented out, so it chooses nothing.
6. **Runs `systemd-sysusers`**, which creates the unprivileged `sendspin-cli` account the
   unit runs as out of the declaration the payload just installed. This is the one step a
   tarball cannot do for itself, and without it the unit does not start at all — see
   [Running as a Service](Running-as-a-Service#it-runs-as-its-own-account).
7. **Enables the unit — and starts it only if an output is already configured.** More on
   that below; it is the one surprising thing the script does.
8. **Lists this host's sound devices** and prints the two commands that finish the job.

Re-running it is how you upgrade: it overwrites the same paths, and restarts the service if
step 7 finds an `output` configured — which after a first run it will. Both step 6 and step 7
are idempotent, so nothing there minds being run twice.

### Why it does not start the player

A systemd **system** unit has no user session, so there is no PipeWire or PulseAudio for
ALSA's `default` PCM to follow — and the device that opens perfectly from your shell usually
will not open under `systemctl`. The unit is `Restart=on-failure` with `RestartSec=5`, so a
player started before you have named a card fails and is retried every five seconds forever,
while the script that started it prints congratulations.

So it enables the unit, shows you the devices, and leaves starting it to you. Once
`/etc/sendspin-cli.conf` names an `output`, the script starts the player itself on every
subsequent run.

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
[Choosing an output](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/README.md#choosing-an-output):

| Value | What it means |
|---|---|
| `hw:1,0` | that card directly, bypassing any sound server |
| `plughw:1,0` | the same card, letting ALSA convert rate and format for it |
| `default` | follow the host's own configuration — PipeWire, PulseAudio or bare hardware |

`default` is the right answer from a login shell and usually the wrong one under a system
unit, for the reason above. Under a **user** unit (`systemctl --user`) it is right again,
because there the session and its sound server exist.

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
should appear under the name it logged — which is `-n`, falling back to this host's name.
Nothing needs configuring on the server side.

To go the other way and have the player dial the server instead, set `server` in the config:

```ini
server = 192.168.1.10          # a host, port 8927 assumed
server = mdns:Music Assistant  # or discover one by its advertised name
```

Any `server` value turns the mDNS advertisement off. That is the spec's rule rather than a
preference here, and the two modes are mutually exclusive by design — see
[The two connection modes](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/README.md#the-two-connection-modes).

## Next

- [Configuration](Configuration) — every key, and what the player remembers by itself
- [Controlling the Player](Controlling-the-Player) — `pause`, `vol`, `delay` and the rest
- [Running as a Service](Running-as-a-Service) — the account it runs as, drop-ins, and what
  the hardening block takes away
- [Troubleshooting](Troubleshooting) — when it starts and stays silent
