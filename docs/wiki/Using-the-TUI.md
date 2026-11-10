# Using the TUI

`sendspin-cli tui` opens a terminal screen for the player running on this host: what is
playing, the keys you would otherwise type as `pause` and `vol`, and a large pairing code when
a server wants to pair.

```console
$ sendspin-cli tui
```

**It is a remote, not a player.** It attaches to the running player over the
[control socket](Controlling-the-Player), never opens audio and has no identity of its own, so
quitting it changes nothing about the player. Several can be open at once.

It finds the player exactly as `sendspin-cli status` does: the same `--port`,
`--control-socket` and config file. A player on another port needs the same flag here, and
the rules are in [Finding the socket](Controlling-the-Player#finding-the-socket).

**Under the Linux system service the socket is root-only**, so:

```console
$ sudo sendspin-cli tui --control-socket /run/sendspin-cli/control.sock
```

It needs a terminal on both ends. Piped or redirected, it exits `1` with
`error: sendspin-cli tui needs a terminal`.

## The screen

The panels stack in one column on a terminal narrower than 80 columns.

- **Now Playing** is the title, artist and album, with the previous, play or pause, and next
  keys. With nothing loaded it says `Press <space> to play`.
- **Volume** is the group and this player's own level, each with `[MUTED]` when it is.
- **Progress** is a bar with the position and length. The position moves between polls on its
  own, and says `(live)` for a stream with no length.
- **Playback** is repeat and shuffle. **Stream** is the codec, the format and the output
  delay. **Server** is the connected server, its trust, the group, and a prompt to pair when
  this player has no pairing yet.
- **The footer** carries the `p` token line and short notices, such as
  `No server is connected`, which clear after 3 seconds.

A value nobody has reported shows as `--`, not as a zero. The screen polls the player about
twice a second, so a change made elsewhere appears within that.

## Keys

| Key | What it does | Same as |
|---|---|---|
| `space` | play, or pause while playing | `play`, `pause` |
| left, right | previous, next track | `prev`, `next` |
| up, down | this player's volume, 5 a step | `player-vol` |
| `m` | mute or unmute this player | `player-mute` |
| `]`, `[` | group volume, 5 a step | `vol` |
| `M` | mute or unmute the group | `mute` |
| `r` | repeat: off, all, one | `repeat` |
| `x` | shuffle on or off | `shuffle` |
| `g` | switch group | `switch` |
| `,`, `.` | output delay, 10 ms a step | `delay` |
| `p` | show or hide the pairing token | `pair-token` |
| `q`, Ctrl-C | quit | |

The group keys and this player's own keys are different things; that split is explained in
[Three that are easy to misread](Controlling-the-Player#three-that-are-easy-to-misread). The
group keys do nothing until the server has sent the group's state. A key the player or server
refuses shows as a notice rather than an error.

**`p` shows the pairing token, so treat it like a password.** It is fetched when you show it
and forgotten when you hide it again.

## Pairing

Start pairing in Music Assistant, with the TUI open on the player's host. The screen is taken
over so the code cannot be missed:

- **A dynamic code** fills the screen in large digits, with the server's name above it.
  Type it into the server. On a terminal too narrow for the large digits, or too short, it is
  the same code as plain spaced digits.
- **A static code waiting for you** shows `A server wants to pair. Y to allow, N to refuse`.
  `y` is `pair confirm` and `n` is `pair cancel`; see
  [the pairing window](Pairing-a-Player#the-pairing-window).
- **The outcome** takes over the screen for 3 seconds: `Paired with <server>`,
  `Pairing failed: <reason>`, or `The server stopped pairing`, and then the main screen
  returns. An attempt that ended before you opened the TUI is not replayed.

While a pairing screen is up, only `y` and `n` (when asked), `p` and `q` do anything; the
playback keys wait until it is over.

## When there is no player to show

These screens recheck every half second, so starting or fixing the player brings the main
screen up by itself. `q` still quits.

- **No sendspin-cli player is running here, or it did not answer.** Nothing listens on the
  socket, the port is not the player's, or the player took longer than 3 seconds to answer,
  which counts as not answering. The line below the heading says which. Start the player, or
  pass the same `--port` or `--control-socket` it uses.
- **A sendspin-cli player is running, but this user may not connect to it.** The socket
  refused you, which is the normal case under the system service. The screen prints the
  command to run, `sudo sendspin-cli tui --control-socket <the path it tried>`.
- **The running sendspin-cli player is too old for the TUI.** It predates `status-kv`.
  Restart it on this version.

## Next

- [Controlling the Player](Controlling-the-Player) -- the subcommands the keys stand for, and
  `status-kv`
- [Pairing a Player](Pairing-a-Player)
- [Troubleshooting](Troubleshooting) -- what exit `3` usually means
- [Running as a Service](Running-as-a-Service) -- why the socket is root-only there
