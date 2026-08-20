# Troubleshooting

Every failure in this player is meant to say what it is in one line, in the log. Start
there:

```bash
journalctl -u sendspin-cli -n 50 --no-pager     # under systemd
sendspin-cli -d debug                           # or run it in the foreground, loudly
```

## It starts, and there is no sound

The three common causes, in the order to check them.

### The device refused the stream's format

The loudest failure there is, and the one that most looks like health from the outside: the
player is connected, the server thinks it is playing, and the audio is being thrown away.

```
E audio: alsa: 'hw:1,0' rejected S24_3LE for 96000 Hz / 2 ch / 24-bit: Invalid argument
E audio: alsa: 'hw:1,0' is not open -- discarding audio until a stream reconfigures it
```

`status` shows the same thing more quietly — **a `stream: receiving` line with no format
after the device name**:

```console
$ sendspin-cli status | grep -E 'stream|output'
stream: receiving
output: hw:1,0
```

Fix it by giving ALSA permission to convert, which is what the `plug` layer is for:

```ini
output = plughw:1,0
```

`hw:` is the card exactly as it is; `plughw:` is the same card with rate and format
conversion in front of it. Use `sendspin-cli -l` to see what the bare device really takes —
only the four formats this player can emit are listed, since anything else is unreachable
anyway.

### `output = default` under a system unit

The single most common first-install problem, and it usually shows as the unit failing
rather than as silence:

```
E audio: cannot open ALSA device 'default': No such file or directory -- run with -l to list
this host's PCMs
```

Name a card instead of `default`, which under a system unit has no session to follow —
[Getting Started on Linux](Getting-Started-on-Linux) has the whole of why:

```bash
sendspin-cli -l                        # find it
sudo nano /etc/sendspin-cli.conf       # output = hw:1,0
sudo systemctl restart sendspin-cli
```

Under a **user** unit, or from your own shell, `default` is right again.

### The player has the wrong device, or none

```console
$ sudo sendspin-cli status --control-socket /run/sendspin-cli/control.sock
output: null
```

`output: null` means audio is being discarded on purpose — either `output = null` in the
config, or a build with no audio backend at all. Check what the binary has:

```console
$ sendspin-cli -l
Output devices (-o):
  null      discard audio; needs no sound card at all
  ...
```

A build with no `alsa` in that list was configured without `libasound2-dev` present. See
[Installation](Installation).

## The unit keeps restarting

```console
$ systemctl status sendspin-cli
   Active: activating (auto-restart) (Result: exit-code)
```

`Restart=on-failure` with `RestartSec=5` retries indefinitely, so the journal has the reason
repeated once every five seconds. The usual ones:

**`status=217/USER` — the account the unit runs as does not exist.** The first thing to check
on a fresh install, and the only failure here where the player never runs at all:

```
sendspin-cli.service: Main process exited, code=exited, status=217/USER
```

The unit names `User=sendspin-cli`, and creating that account is the one step unpacking a
tarball cannot do for itself. One idempotent command fixes it:

```bash
sudo systemd-sysusers
sudo systemctl restart sendspin-cli
```

The getting-started script runs it for you; a by-hand install has to. See
[Running as a Service](Running-as-a-Service#it-runs-as-its-own-account).

**A config file that does not parse.** It names the file and the line, every attempt:

```
error: /etc/sendspin-cli.conf:4: invalid --buffer-ms '5' -- expected 10-2000
```

Fix the line and the player comes back on its own — there is no `systemctl reset-failed` to
do, which is deliberate.

**`Read-only file system` on a `logfile` or `pidfile`.** The unit runs under
`ProtectSystem=strict`, so a path outside `/run/sendspin-cli` and `/var/lib/sendspin-cli` is
refused rather than silently unwritten:

```
error: cannot open logfile /var/log/sendspin-cli.log: Read-only file system
```

Neither key is the shape for this unit — journald already has stderr, and `-z`/`-f` are for a
supervisor without one — so removing the key is usually the answer. If you want a logfile
regardless, a drop-in with `ReadWritePaths=/var/log` is the way back.

**A device that will not open.** See above.

## Nothing discovers it

### Check it is advertising

```console
$ journalctl -u sendspin-cli | grep mdns
I mdns: advertising _sendspin._tcp as "kitchen" on port 8928 (path /sendspin)
```

If that line is absent, one of three things is true.

**`server` is set.** Any `-s` or `server =` makes this player the one dialling, which
suppresses the advertisement — the spec forbids advertising while this end initiates, and
the run says so:

```
Not advertising _sendspin._tcp: -s makes this player the one initiating the connection,
and the Sendspin spec forbids advertising while it is
```

There is deliberately no flag that turns both modes on together. See
[The two connection modes](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#the-two-connection-modes).

**`no-mdns` is set.** Remove it.

**There is no mDNS daemon.** On Linux that is `avahi-daemon`, reached through
`libavahi-compat-libdnssd`. The player warns and retries on a backoff rather than failing,
because a player nobody can find is still a player:

```bash
systemctl status avahi-daemon
sudo apt install avahi-daemon libavahi-compat-libdnssd1
```

**Or this build has no mDNS at all**, which it says at parse time rather than starting and
quietly finding nothing:

```
I mdns: This build has no mDNS support, so it cannot be discovered: point a server at
ws://<this-host>:8928/sendspin, or dial one with -s. See docs/ROADMAP.md.
```

Rebuild with `libavahi-compat-libdnssd-dev` present, or point the server at the URL by hand.

### It advertises and still nothing finds it

- **mDNS does not cross subnets or most VLANs.** The server and the player must be on the
  same broadcast domain, or you need an mDNS reflector on the router.
- **Wi-Fi power saving** puts the NIC to sleep and the advertisement with it. Common on a
  Raspberry Pi: `sudo iw dev wlan0 set power_save off`, or use Ethernet.
- **Docker's default bridge network does not carry mDNS.** Use `--network host`.
- **A firewall blocking UDP 5353** blocks discovery, and TCP on `--port` (8928 by default)
  blocks the connection that follows it.

Verify the advertisement independently:

```bash
avahi-browse -rt _sendspin._tcp     # Linux
dns-sd -B _sendspin._tcp            # macOS
```

## The subcommands cannot find the player

```console
$ sendspin-cli status ; echo $?
error: no sendspin-cli is listening on /run/user/1000/sendspin-cli-8928.sock. Start one, or
point this at the right socket with --control-socket -- and note that a non-default --port
moves the default path, so the same --port has to be given here
3
```

Exit `3` is "nothing is listening on that socket", which is almost always one of:

**The player is on a different `--port`.** The socket path carries the port, so a subcommand
needs the same one:

```bash
sendspin-cli status --port 9000
```

**It is the systemd system unit**, whose socket is somewhere else and belongs to the
`sendspin-cli` account at mode `0600` — so root, which is not subject to the mode, is what
reads it:

```bash
sudo sendspin-cli status --control-socket /run/sendspin-cli/control.sock
```

**There is no control socket at all.** A system unit with no `RuntimeDirectory=` pairing
warns once and carries on:

```
W control: No control socket: $XDG_RUNTIME_DIR is not set, so there is no user-private
directory to put a control socket in. Give --control-socket <path> to choose one, or
--no-control to stop asking
```

**Or `no-control` is set**, which turns the channel off deliberately.

The other statuses are on [Controlling the Player](Controlling-the-Player). The one worth
repeating here: **`4` is "no server connection" and `5` is "the server does not offer that
command"**, and they are kept apart because a dropped connection empties the server's
command list, so collapsing them would send you to read your server's capabilities when the
truth is that nothing is connected.

## "another sendspin-cli is already running"

```console
$ sendspin-cli
E control: another sendspin-cli is already running -- it holds the lock on
/run/user/1000/sendspin-cli-8928.sock.lock
```

Exactly what it says: a live player holds an exclusive lock. **Leftover files are not the
cause and need no cleanup** — the lock is a `flock()` held for the process's life, so a
player killed with `SIGKILL` has its descriptor closed by the kernel and its leftover socket
is simply taken over by the next start. Nothing ever parses a stale file.

The same wording, from the same helper, covers `-P`:

```console
$ sendspin-cli -z -P /run/sendspin-cli.pid
error: another sendspin-cli is already running -- it holds the lock on /run/sendspin-cli.pid
```

Find it and decide:

```bash
systemctl status sendspin-cli
pgrep -a sendspin-cli
```

To run a second player on purpose, give it its own `--port`, its own `--control-socket` and
its own `--state-dir` — they share the state file otherwise, and the second to save its
volume overwrites the first's.

## `status` is telling me something odd

**`position` runs to the end of the track in seconds.** That is `output = null`. The null
sink consumes instantly and paces nothing, so the server sends the whole track as fast as it
can and `status` faithfully reports a server that believes it has finished. Behind a real
device it advances at 1×. Not a bug, and not visible on any sink with a clock.

**`state`, `position`, `repeat` and `shuffle` are stale.** All four are the server's last
word, and the spec does not oblige it to resend them after acting — `shuffle` reading `off`
while it is demonstrably shuffling has been observed against a real server. If you have just
changed something and the figure has not moved, that is the likely reason rather than a
failed command. The block's own `note:` line says so.

**`position` says `(estimated)`.** Expected while playing: the library interpolates forward
from the last progress the server sent, so after a seek the server does not re-report, the
estimate drifts by however far you jumped.

**`state: unknown`.** The server has sent no progress yet. Nothing is wrong.

## Audio drops out, or clicks

Raise the buffer. The default is 100 ms, and the range is 10–2000:

```ini
buffer-ms = 250
```

That is one figure for every backend — ALSA divides it into periods, PortAudio makes it the
ring size, and a device-less sink ignores it. A figure smaller than one device buffer is
raised to the floor and says so at `debug`. See
[Buffering, and what gets advertised](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#buffering-and-what-gets-advertised).

If it is one speaker out of sync with the others rather than dropping out, that is
`delay`, not `buffer-ms` — see [Controlling the Player](Controlling-the-Player).

## macOS: "cannot be opened because the developer cannot be verified"

Clear the quarantine flag:

```bash
xattr -d com.apple.quarantine ./sendspin-cli-0.1.0-macos-arm64/usr/local/bin/sendspin-cli
```

Unpacking from a terminal avoids it in the first place, and `sudo installer -pkg … -target /`
is not gated at all. Why — and which of the four ways you can come by these files you are in
— is on [Installation](Installation).

## The daemon exits and says nothing

`-z` with no `-f` is the one case where a failure can be genuinely invisible: the parent
returns `0` immediately, and everything after the fork — the output device, the WebSocket
server, mDNS — can only report into a log that is going to `/dev/null`. The player warns
about exactly this at startup.

Give it a logfile, or do not daemonize:

```bash
sendspin-cli -z -f /var/log/sendspin-cli.log -P /run/sendspin-cli.pid
```

Under systemd, neither flag belongs: `Type=simple` in the foreground puts everything in the
journal. See
[Running as a daemon](https://github.com/chrisuthe/sendspin-cpp-cli/blob/main/README.md#running-as-a-daemon).

## Still stuck

Collect this and open an issue at
[the repository](https://github.com/chrisuthe/sendspin-cpp-cli/issues):

```bash
sendspin-cli --version
uname -srm
sendspin-cli -l
sudo sendspin-cli status --control-socket /run/sendspin-cli/control.sock
journalctl -u sendspin-cli -n 100 --no-pager
```

Say which of the two connection modes you are in — waiting to be discovered, or dialling
with `server` — because almost everything about the failure differs between them.
