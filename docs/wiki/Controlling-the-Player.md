# Controlling the Player

The player listens on a **Unix socket**, and the same binary is its own client. This is the
deliberate addition to the squeezelite model: `sendspin-cli pause` on the player's own host
drives it, with no server and no controller app in the loop.

```console
$ sendspin-cli status
name: living-room
server: Music Assistant (connected)
state: playing
stream: receiving
track: Nils Frahm - Says
position: 2:05 / 9:03 (estimated)
group volume: 55
repeat: off
shuffle: off
player volume: 80
static delay: 0 ms
note: state, position, repeat and shuffle are the server's last report; a server that does not resend them after a change will show stale values here
output: default (48000 Hz / 2 ch / 16-bit)

$ sendspin-cli pause
$ sendspin-cli vol 40
$ sendspin-cli seek-rel -30000
$ sendspin-cli delay 250
```

**The subcommand comes first, before any flag** — `sendspin-cli vol 50 --port 9000`, never
`sendspin-cli --port 9000 vol 50`. `argv[1]` is split off before the flag parser runs, which
is also what lets `seek-rel -5000` be an offset rather than a flag cluster.

## Every subcommand

| Command | Argument | What it does |
|---|---|---|
| `status` | | what this player and its group are doing — **answered locally** |
| `play` | | resume or start playback |
| `pause` | | pause playback |
| `stop` | | stop playback |
| `next` | | skip to the next track |
| `prev` | | skip to the previous track |
| `vol` | `<0-100>` | set the **group** volume |
| `mute` | `on\|off` | mute or unmute the group |
| `seek` | `<ms>` | seek to an absolute position |
| `seek-rel` | `<+/-ms>` | seek by an offset; negative goes backwards |
| `repeat` | `off\|one\|all` | set the repeat mode |
| `shuffle` | `on\|off` | turn shuffle on or off |
| `switch` | | move this player through the groups available to it |
| `delay` | `<0-5000>` | this endpoint's static delay — **answered locally** |

Twelve of those go out to the server as `controller@v1` commands. Two never leave the host:
`status`, formatted from the daemon's own view, and `delay`, which drives this endpoint's
own player role.

## Three that are easy to misread

**`vol` is the *group* volume, not this box's output level.** It goes to the server, which
spreads it across every player in the group and clamps it per player. A squeezelite refugee
will expect `vol 50` to move *this* box, and it does not — which is why `status` prints
`group volume` and `player volume` as two named lines rather than one ambiguous `volume:`.

**`switch` is not a source selector.** Per the spec's switch cycle it re-homes this client
between the groups available to it. It sits next to `play` and `pause` and means something
quite different.

**`delay` really is this endpoint's own**, and its direction is the opposite of what the
name suggests. It is not "play this speaker later"; it is "my gear is *already* this far
behind". The sync task **subtracts** the figure from every chunk's timestamp, so the player
hands audio to the device that much **earlier** and the sound lands on the timestamp the
server meant. If this speaker sounds 250 ms late against the rest of the group:

```console
$ sendspin-cli delay 250      # my amp adds 250 ms, so hand audio over 250 ms early
$ sendspin-cli status | grep 'static delay'
static delay: 250 ms
$ sendspin-cli delay 0        # off again
```

It works with **no server connected**, it is **remembered across restarts** (the spec
requires that of a client), and the player still tells the server, which needs it to work
out how far ahead to send audio. Out-of-range values are refused rather than clamped.
Changing it mid-stream re-times chunk scheduling, so expect a brief resync — set it while
stopped where you can.

The long version of all three is in
[The local control channel](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/README.md#the-local-control-channel).

## Reading `status`

**Four fields are the server's word and can lag what is true**: `state`, `position`,
`repeat` and `shuffle` all come from the server's last report, and the spec does not oblige
it to resend them after acting. Observed against a real server, `shuffle` read `off` for
minutes while it was demonstrably shuffling. If you have just changed something and the
figure has not moved, that is the likely reason — not a failed command. The `note:` line
above `output:` says so, and appears whenever a server is connected.

- **`position` says `(estimated)` while playing**, because the library interpolates forward
  from the last progress the server sent. After a seek the server does not re-report, the
  estimate drifts by however far you jumped. Paused, it is the server's own snapshot and
  carries no marker.
- **`player volume` is the gain this box's output is applying**, and says
  `(default; no server has set it)` until a server sends a volume command — which is how you
  tell "nobody has set this" from a server that chose full output.
- **`state` and `stream` are different facts.** `state` is the *group's* transport state.
  `stream` is whether audio is arriving at *this* endpoint — a player dropped from the group
  loses it while the group plays on. A `stream: receiving` line with **no format after the
  device name** means the device refused the stream's format and its audio is being
  discarded; the log says so loudly at the same moment.

## Finding the socket

The default is `$XDG_RUNTIME_DIR/sendspin-cli-<port>.sock`, mode `0600`, where `<port>` is
`--port`. The port is in the name so two players on one host each get their own — **so a
subcommand needs the same `--port` as the player**, or an explicit `--control-socket`:

```bash
sendspin-cli --port 9000 &                                 # this player's socket carries 9000
sendspin-cli status --port 9000                            # ...so its subcommands need it too
sendspin-cli status --control-socket /run/user/1000/sendspin-cli-9000.sock   # or name it
```

**Under the systemd system unit**, both change. A system unit has no `$XDG_RUNTIME_DIR`, so
the unit passes `--control-socket /run/sendspin-cli/control.sock`, and the socket belongs to
the unprivileged `sendspin-cli` account the unit runs as, at mode `0600`. Root is what
connects to it, not being subject to the mode:

```bash
sudo sendspin-cli status --control-socket /run/sendspin-cli/control.sock
```

Put `control-socket = /run/sendspin-cli/control.sock` in `/etc/sendspin-cli.conf` and the
flag becomes unnecessary. The `sudo` is still needed. Why the daemon ignores that same key
while its subcommands read it is [Configuration](Configuration)'s precedence rule.

**On macOS the default works with nothing set**, because there is no `$XDG_RUNTIME_DIR`
there either and the path comes from the per-user directory under `/var/folders` that
launchd already provides. There is deliberately **no `/tmp` fallback anywhere**: `/tmp` is
world-writable, and a socket there would let any local user pause your music.

## Exit status is the interface for scripts

| Status | Means |
|---|---|
| `0` | sent, or answered locally (`status`, `delay`) |
| `1` | the command line did not parse (`vol 500`, `delay 5001`) |
| `2` | the player refused the argument (a `seek` past the server's `seek_max_ms`) |
| `3` | **nothing is listening on that socket** — no player, or the wrong `--port` |
| `4` | the player is up but has **no server connection** |
| `5` | the server does not offer that command |
| `6` | the exchange broke down |

`3`, `4` and `5` are kept apart because they call for three different actions: start the
daemon, connect it to a server, or stop asking for a command this server does not offer. `4`
and `5` especially — a dropped connection *empties* the server's advertised command list, so
collapsing them would answer "pause is not supported" when the truth is that nothing is
connected.

`status` and `delay` are never refused by any of them: nothing about either is sent, so a
missing connection is no obstacle. A disconnected player is exactly when reading `status` is
worth doing.

```bash
if ! sendspin-cli status >/dev/null 2>&1; then
    case $? in
        3) echo 'no player running there' ;;
        *) echo 'something else' ;;
    esac
fi
```

## Driving it without this binary

Connect, send one line, read until the player closes. The first line back is `ok` or
`error <kind>: <reason>`; a `status` payload follows the `ok`. One command per connection.

```console
$ printf 'status\n' | socat - UNIX-CONNECT:/run/user/1000/sendspin-cli-8928.sock
ok
name: living-room
...
```

The socket is polled from the main loop rather than from a thread, so a request round-trips
in up to 10 ms. That is a deliberate trade — the library calls behind it are documented
main-thread-only, and a reader thread would be a data race.

## The player driving *your* hardware: stream hooks

The control channel is you driving the player; `--hook-start` and `--hook-stop` are the
player driving whatever sits around it. Each runs a shell command — through `/bin/sh -c`,
so pipes and `&&` work — when a stream starts and when it stops: the amplifier relay, the
light, the notification.

```bash
sendspin-cli -o hw:1,0 \
  --hook-start 'amixer -c 1 set Master unmute' \
  --hook-stop  'amixer -c 1 set Master mute'
```

The event's facts arrive in the environment, in the same vocabulary the Python
`sendspin-cli` uses — a hook script written against one runs unchanged against the other:
`SENDSPIN_EVENT` (`start` or `stop`) always, and `SENDSPIN_SERVER_ID`,
`SENDSPIN_SERVER_NAME`, `SENDSPIN_SERVER_URL` (outbound `-s` runs only) and
`SENDSPIN_CLIENT_NAME` where known. An unknown is left *unset* rather than exported
empty, so `[ -n "$SENDSPIN_SERVER_ID" ]` means what it says. A stop event carries the
same server facts as the start it pairs with — gathered when the stream started, because
a stream usually ends when its connection goes and there is nothing left to ask by then.

The hook never blocks playback: it is spawned and reaped from the main loop, its output
goes to the log, and a non-zero exit is a `W hook:` warning rather than a player failure.
Stopping the player while a stream is playing runs the stop hook before it exits, so
`systemctl stop` switches the amplifier off rather than leaving it on. The hook is handed
nothing of the player's but that output stream — every other descriptor is closed and
SIGPIPE is back at its default — so `something | head -1` behaves as it would in any other
shell, and a slow hook cannot sit on the port a restart needs.
It fires on the stream lifecycle — for exactly as long as `status` says
`stream: receiving` — so a stream whose format the device refused still switches the
amplifier: audio is arriving either way. Both flags can also come from the config file, as
`hook-start` and `hook-stop`.

## Next

- [Configuration](Configuration) — making `control-socket` stick
- [Running as a Service](Running-as-a-Service)
- [Troubleshooting](Troubleshooting) — what exit `3` usually means
