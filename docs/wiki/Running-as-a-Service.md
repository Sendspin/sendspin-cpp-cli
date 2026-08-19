# Running as a Service

One systemd unit ships in the Linux payload, with sane defaults and nothing to fill in.

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now sendspin-cli
systemctl status sendspin-cli
journalctl -u sendspin-cli -f
```

The unit is installed at `/usr/local/lib/systemd/system/sendspin-cli.service`, which is
already on systemd's search path — nothing needs copying by hand. `daemon-reload` after
installing is what makes systemd notice it.

> **Set an `output` before enabling it.** A system unit has no user session, so ALSA's
> `default` PCM has no PipeWire or PulseAudio to follow and usually will not open. The unit
> is `Restart=on-failure` with `RestartSec=5`, so a player that cannot open its device
> retries every five seconds indefinitely. Run `sendspin-cli -l`, pick a card, and put
> `output = hw:1,0` in `/etc/sendspin-cli.conf`. This is why
> [`scripts/get_started_linux.sh`](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/scripts/get_started_linux.sh)
> enables the unit but does not start it until an output is configured.

## What the payload installs

| Path | What |
|---|---|
| `/usr/local/bin/sendspin-cli` | the binary |
| `/usr/local/lib/systemd/system/sendspin-cli.service` | the unit |
| `/usr/local/share/doc/sendspin-cli/README.md` | the reference |
| `/usr/local/share/doc/sendspin-cli/LICENSE` | Apache 2.0 |
| `/usr/local/share/doc/sendspin-cli/sendspin-cli.conf.example` | an annotated config |

The unit goes in `lib/systemd/system` and not a multiarch `libdir` because a unit file is
architecture-independent, and systemd reads `/usr/lib/systemd/system` and
`/usr/local/lib/systemd/system` — never `lib/x86_64-linux-gnu/systemd/system`.

**`ExecStart` names the binary absolutely**, at the prefix the build was configured for, so
a binary moved out of `/usr/local` leaves the unit pointing at nothing.

## The shape of it

`Type=simple`, running the player in the **foreground**, so the log goes to the journal
rather than to a file something has to rotate. `-z` and `-f` would both be working around
the supervisor. Two other shapes exist and are not what ships: `Type=forking` with
`PIDFile=` pointing at `-P` is right for a supervisor with no journal, and `Type=notify` is
unavailable because `sd_notify` is not wired up.

`After=network.target sound.target` and `After=avahi-daemon.service` — ordering only, and
deliberately no `Wants=`. The player retries its advertisement and its outbound dial on a
backoff, so it comes up perfectly well ahead of the network, and a host whose operator
turned `avahi-daemon` off should stay that way rather than have this unit pull it back in.

## The two flags on the `ExecStart` line

Both are there because a system unit has neither of the environment variables the default
path would come from:

| Unit directive | Flag it pairs with | Without the pair |
|---|---|---|
| `RuntimeDirectory=sendspin-cli` | `--control-socket /run/sendspin-cli/control.sock` | no `$XDG_RUNTIME_DIR`, so no control socket — one warning, and the player carries on |
| `StateDirectory=sendspin-cli` | `--state-dir /var/lib/sendspin-cli` | no `$XDG_STATE_HOME`, so volume, mute and the static delay are forgotten every restart |

systemd creates and owns both directories, and removes the runtime one when the unit stops
— which is why this unit never meets a stale socket.

**One consequence**, since the command line beats the config file per option: `state-dir`
and `control-socket` in `/etc/sendspin-cli.conf` are *silently ignored* by the service.
Setting `control-socket` to the same path is still worth doing — it is what lets a
*subcommand* find the socket with no flags.

## Configure it in the config file, not the unit

Every config key is a long flag name, so there is nothing the `ExecStart` line can say that
`/etc/sendspin-cli.conf` cannot. Editing the unit means merging your changes by hand on
every upgrade; editing the config does not. See [Configuration](Configuration).

```bash
sudo nano /etc/sendspin-cli.conf
sudo systemctl restart sendspin-cli
```

A config file that does not parse exits non-zero, and `Restart=on-failure` retries it every
five seconds indefinitely. That is the wanted end of it rather than an oversight: the parse
error names the file and its line in the journal on every attempt, and an operator who fixes
the file gets a player back without also having to `systemctl reset-failed` a unit that gave
up.

## The subcommands need `sudo` here

The control socket is mode `0600` and the service is root's, so an unprivileged shell cannot
connect to it — and root has no `$XDG_RUNTIME_DIR` for the default path to come from:

```bash
sudo sendspin-cli status --control-socket /run/sendspin-cli/control.sock
```

Adding `control-socket = /run/sendspin-cli/control.sock` to the config removes the flag. The
`sudo` stays. See [Controlling the Player](Controlling-the-Player).

## It runs as root, and that is a decision

There is no `User=` and no hardening block, so the player runs as root — its WebSocket
server, which listens on the network, included. A tarball has no `postinst` to create a
dedicated user with, and root already has the sound card (`/dev/snd` is `root:audio` mode
`0660`), the mDNS daemon and `/run` with nothing to arrange first.

> A change to that user model is in flight — a dedicated user with a hardening block is being
> worked on in a separate change. This page describes what `main` ships today.

A drop-in is where to change it now:

```bash
sudo systemctl edit sendspin-cli
```

```ini
[Service]
User=sendspin
SupplementaryGroups=audio
```

**Both lines together, and neither alone**: a user with no membership of `audio` is a player
that starts and cannot open a device. Create the user first and hand it the state directory
once, since systemd owns the directory it creates but not one a root-run player left behind:

```bash
sudo useradd --system --no-create-home sendspin
sudo chown -R sendspin /var/lib/sendspin-cli
```

An `ExecStart=` in a drop-in has to be cleared first — `ExecStart=` on its own line, then
the replacement — which is systemd's rule for every list-valued directive rather than
anything about this unit.

## Reading the log

Everything goes to the journal, and every line carries a level letter and a subsystem tag:

```console
$ journalctl -u sendspin-cli -f
I cli: sendspin-cli 0.1.0 listening on port 8928 as "kitchen" (output: hw:1,0)
I mdns: advertising _sendspin._tcp as "kitchen" on port 8928 (path /sendspin)
I sendspin.ws_server: Starting server on port: 8928 (max connections: 4)
```

The third line is the library's. That is the point of the format — it is the shape
sendspin-cpp's own logging already emits, so one `grep` reaches either half:

```bash
journalctl -u sendspin-cli | grep ' mdns:'              # this player's mDNS lines
journalctl -u sendspin-cli | grep ' sendspin\.'         # the library's, all of them
journalctl -u sendspin-cli -p err                       # only failures
```

Ours are `cli`, `audio`, `mdns`, `discovery`, `outbound`, `player`, `metadata` and
`control`; the library's are all `sendspin.<subsystem>`.

Turn it up with `log-level = debug` in the config. One level covers this player and the
library together — deliberately, so a single key turns up everything about one run. Fatal
startup errors are **not** gated by it: `none` means "do not narrate", not "exit without
saying why".

Lines are not timestamped by the player under systemd, because journald already stamps them
and a second one would be noise. Only a `-f` logfile gets our own timestamp. See
[Logging](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#logging).

## A user unit instead

If the player should follow your desktop session's sound server, a user unit is the better
fit — `$XDG_RUNTIME_DIR` and `$XDG_STATE_HOME` both exist there, so neither flag is needed
and `output = default` works as it does from your shell:

```bash
mkdir -p ~/.config/systemd/user
cp /usr/local/lib/systemd/system/sendspin-cli.service ~/.config/systemd/user/
# edit out the --control-socket and --state-dir arguments, and the two Directory= lines
systemctl --user daemon-reload
systemctl --user enable --now sendspin-cli
loginctl enable-linger "$USER"     # so it runs when you are not logged in
```

The unit that ships is the system one; this is a recipe rather than something the project
installs or tests.

## Uninstalling

```bash
sudo systemctl disable --now sendspin-cli
sudo rm -f /usr/local/bin/sendspin-cli
sudo rm -f /usr/local/lib/systemd/system/sendspin-cli.service
sudo rm -rf /usr/local/share/doc/sendspin-cli
sudo rm -rf /var/lib/sendspin-cli          # what it remembered
sudo rm -f /etc/sendspin-cli.conf          # your config
sudo systemctl daemon-reload
```

## Next

- [Configuration](Configuration)
- [Controlling the Player](Controlling-the-Player)
- [Troubleshooting](Troubleshooting)
