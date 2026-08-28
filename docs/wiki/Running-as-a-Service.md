# Running as a Service

One systemd unit ships in the Linux payload, with sane defaults and nothing to fill in.

```bash
sudo systemd-sysusers
sudo systemctl daemon-reload
sudo systemctl enable --now sendspin-cli
systemctl status sendspin-cli
journalctl -u sendspin-cli -f
```

The unit is installed at `/usr/local/lib/systemd/system/sendspin-cli.service`, which is
already on systemd's search path — nothing needs copying by hand. `daemon-reload` after
installing is what makes systemd notice it.

**`systemd-sysusers` is the line that is not optional.** It creates the unprivileged account
the unit runs as, out of a declaration installed beside the unit, and a tarball has no
`postinst` to run it for you. Skip it and the unit does not start at all — see
[It runs as its own account](#it-runs-as-its-own-account).

> **Set an `output` before enabling it**, or this unit will fail and be retried every five
> seconds indefinitely — `Restart=on-failure` with `RestartSec=5`, and a system unit has no
> session for ALSA's `default` PCM to follow. Run `sendspin-cli -l`, pick a card, and put
> `output = hw:1,0` in `/etc/sendspin-cli.conf`.
> [Getting Started on Linux](Getting-Started-on-Linux) has the argument in full, and is why
> [`scripts/get_started_linux.sh`](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/scripts/get_started_linux.sh)
> enables this unit without starting it.

## What the payload installs

| Path | What |
|---|---|
| `/usr/local/bin/sendspin-cli` | the binary |
| `/usr/local/lib/systemd/system/sendspin-cli.service` | the unit |
| `/usr/local/lib/sysusers.d/sendspin-cli.conf` | the account the unit runs as, declared |
| `/usr/local/share/doc/sendspin-cli/README.md` | the reference |
| `/usr/local/share/doc/sendspin-cli/LICENSE` | Apache 2.0 |
| `/usr/local/share/doc/sendspin-cli/sendspin-cli.conf.example` | an annotated config |

The unit goes in `lib/systemd/system` and not a multiarch `libdir` because a unit file is
architecture-independent, and systemd reads `/usr/lib/systemd/system` and
`/usr/local/lib/systemd/system` — never `lib/x86_64-linux-gnu/systemd/system`. The account
declaration is in `lib/sysusers.d` for both of the same reasons: a list of users has no
architecture either, and `systemd-sysusers` searches `/usr/local/lib/sysusers.d` alongside
`/usr/lib/sysusers.d`.

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

**One consequence**: `state-dir` and `control-socket` in `/etc/sendspin-cli.conf` are
*silently ignored* by the service, because the command line beats the file per option — see
[Configuration](Configuration). Setting `control-socket` to the same path is still worth
doing, since it is what lets a *subcommand* find the socket with no flags.

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

The control socket is mode `0600` and belongs to the service account, so an unprivileged
shell cannot connect to it — while root can, because root is not subject to the mode. And
root has no `$XDG_RUNTIME_DIR` for the default path to come from either:

```bash
sudo sendspin-cli status --control-socket /run/sendspin-cli/control.sock
```

Adding `control-socket = /run/sendspin-cli/control.sock` to the config removes the flag. The
`sudo` stays. See [Controlling the Player](Controlling-the-Player).

## It runs as its own account

The unit names `User=sendspin-cli` — an unprivileged system account with no home and no
shell — so the player, its network-facing WebSocket server included, is not root.

**Creating that account is the one step installing cannot do for you.** A tarball has no
`postinst`, so the declaration ships beside the unit as `lib/sysusers.d/sendspin-cli.conf`
and one idempotent command turns it into an account:

```console
$ sudo systemd-sysusers
Creating group 'sendspin-cli' with GID 997.
Creating user 'sendspin-cli' (Sendspin audio player) with UID 997 and GID 997.
```

Skip it and the unit does not start at all, which `systemctl status` says in the words that
name the cause rather than hiding it:

```
sendspin-cli.service: Main process exited, code=exited, status=217/USER
```

The fragment carries two lines and they are owed **together**: the account, and its
membership of `audio`. A `sendspin-cli` in no `audio` group is a player that starts and
cannot open a device, because `/dev/snd` is `root:audio` mode `0660` — which is why this is a
shipped declaration rather than a `useradd` line in a document. If you manage accounts with
your own tooling, the equivalent is
`useradd --system --no-create-home -G audio sendspin-cli`.

`DynamicUser=` looks like it would avoid all of this and does not: it hands the player a uid
in no supplementary group at all, which deafens the ALSA backend.

### Upgrading from a version that ran as root

**Nothing needs doing to `/var/lib/sendspin-cli`.** `StateDirectory=` chowns the directory it
finds as well as the one it creates, recursively, so a root-owned state file from an earlier
install becomes the new account's on the first start and the remembered volume, mute and
static delay carry over.

Two things are worth checking before the upgrade, and both come from the hardening block:

- **A `logfile` or `pidfile` in `/etc/sendspin-cli.conf`** pointing anywhere but
  `/run/sendspin-cli` or `/var/lib/sendspin-cli` now fails under `ProtectSystem=strict` —
  `cannot open logfile /var/log/sendspin-cli.log: Read-only file system`, loudly and on every
  restart, rather than a player logging nowhere in silence. Neither key is the shape for this
  unit anyway, since journald already has stderr. A drop-in with `ReadWritePaths=/var/log` is
  the way back if you want one regardless.
- **A drop-in of your own that set `User=` and `SupplementaryGroups=audio`** — the recipe for
  getting off root when the unit had no account of its own — is now overriding a unit that
  already names one. Remove the drop-in and take the shipped account instead; `systemctl
  revert sendspin-cli` drops every drop-in at once.

### What is hardened

The unit carries `ProtectSystem=strict`, `NoNewPrivileges=`, an empty
`CapabilityBoundingSet=`, `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6`,
`SystemCallFilter=@system-service` and the `Protect*=` family, each commented where it sits.
Read the installed unit for the full block. Three operator-visible edges:

- The whole block wants systemd **247**; the unit itself still starts on **236**. Below 247
  systemd warns `Unknown key name 'ProtectProc' … ignoring` and runs the unit with one
  directive fewer.
- `RuntimeDirectory=` and `StateDirectory=` stay writable under `ProtectSystem=strict`, which
  is what leaves the control socket and the state file somewhere to be. `/etc` is only ever
  read.
- Four directives that would gate what the ALSA backend reaches are deliberately *absent* —
  `PrivateDevices=`, `DeviceAllow=`, `ProcSubset=pid` and `RestrictRealtime=` — because they
  pass every check a machine with no sound card can make, and tracked as
  [`docs/ROADMAP.md`](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/docs/ROADMAP.md)
  item 10.

The full argument for every directive, and the `systemd-analyze security` figures, are in
[The systemd unit](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/README.md#the-systemd-unit).

### Changing any of it

Use a drop-in rather than editing the installed unit, which an upgrade overwrites:

```bash
sudo systemctl edit sendspin-cli
```

An `ExecStart=` in a drop-in has to be cleared first — `ExecStart=` on its own line, then
the replacement — which is systemd's rule for every list-valued directive rather than
anything about this unit.

## Reading the log

Everything goes to the journal, and every line carries a level letter and a subsystem tag:

```console
$ journalctl -u sendspin-cli -f
I cli: sendspin-cli 0.1.5 listening on port 8928 as "kitchen" (output: hw:1,0, mDNS: dns_sd (avahi-compat))
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
[Logging](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/README.md#logging).

## A user unit instead

If the player should follow your desktop session's sound server, a user unit is the better
fit — `$XDG_RUNTIME_DIR` and `$XDG_STATE_HOME` both exist there, so neither flag is needed
and `output = default` works as it does from your shell. So do the native `pulse` and
`pipewire` backends, which talk to that same session's server:

```bash
mkdir -p ~/.config/systemd/user
cp /usr/local/lib/systemd/system/sendspin-cli.service ~/.config/systemd/user/
# edit out the --control-socket and --state-dir arguments, the two Directory= lines,
# and the User= line -- a user unit cannot set one, and would refuse to start with it
systemctl --user daemon-reload
systemctl --user enable --now sendspin-cli
loginctl enable-linger "$USER"     # so it runs when you are not logged in
```

Your own account needs `audio` membership for `/dev/snd` here, since the `sendspin-cli`
account's membership does nothing for a unit that is not running as it:
`sudo usermod -aG audio "$USER"`, then log out and back in.

The unit that ships is the system one; this is a recipe rather than something the project
installs or tests.

## Uninstalling

```bash
sudo systemctl disable --now sendspin-cli
sudo rm -f /usr/local/bin/sendspin-cli
sudo rm -f /usr/local/lib/systemd/system/sendspin-cli.service
sudo rm -f /usr/local/lib/sysusers.d/sendspin-cli.conf
sudo rm -rf /usr/local/share/doc/sendspin-cli
sudo rm -rf /var/lib/sendspin-cli          # what it remembered
sudo rm -f /etc/sendspin-cli.conf          # your config
sudo systemctl daemon-reload
sudo userdel sendspin-cli                  # the account, if you want it gone too
```

Removing the fragment does not remove the account — `systemd-sysusers` creates users and
never deletes them — so `userdel` is a separate line, and an optional one: a system account
with no home, no shell and nothing running as it costs a passwd entry.

## Next

- [Configuration](Configuration)
- [Controlling the Player](Controlling-the-Player)
- [Troubleshooting](Troubleshooting)
