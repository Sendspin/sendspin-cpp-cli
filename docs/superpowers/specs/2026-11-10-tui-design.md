# `sendspin-cli tui`: a terminal remote for the running player

Status: approved design (2026-11-10). Roadmap item 9, "Interactive TUI mode".

## Why

The player runs in the background as a service: a systemd unit, a launchd agent, or the
Windows scheduled task. Today, finding out what it is doing means running `sendspin-cli status`
again and again. Pairing means grepping a log, or running `status` at the right moment, for a
six-digit code that lasts as long as one attempt.

The Python sendspin-cli has a TUI, but it is a *player*: launching it while the daemon plays
starts a second, independent player with its own identity and pairing. It also shows a pairing
code in small text and never reports a failed attempt.

What we want is a remote control for the player that is already running. You open it at any
time and it attaches. When you start pairing in Music Assistant, it shows the code so big you
cannot miss it. The rest of the time it shows what is playing and lets you drive playback.

## Goals

1. **It attaches; it never plays.** `sendspin-cli tui` finds the running player the same way
   `sendspin-cli status` does: the same default socket, and the same `--port`,
   `--control-socket` and config-file handling. It talks to the player only over the control
   socket. It opens no audio device, pidfile, server connection or mDNS, and has no identity of
   its own.
2. **Pairing takes over the screen.** Within about a second of a server starting a pairing
   attempt, the screen clears to the dynamic code in large block digits, with the server's name
   and "type this into Music Assistant".
   - **Success:** shows "Paired with <server>" briefly, then returns to the main screen.
   - **Failure:** shows the reason (for example, the code did not match), then returns to the
     main screen.
   - **The server abandons the attempt** (for example, the dialog in Music Assistant is
     closed): shows "The server stopped pairing", then returns. Never a silent return. *(My
     call: the anti-goal below rules out a failure that vanishes without a word, and an
     abandoned attempt looks exactly like one.)*
   - **Static code:** when a server is waiting on `pair confirm` (the pairing-window case), the
     takeover asks instead: "A server wants to pair. Y to allow, N to refuse".
3. **A now-playing screen.** It matches the Python TUI's layout and keys (below), so anyone who
   knows one knows the other.
4. **A useful screen when no player is running.** It shows one plain screen ("No sendspin-cli
   player is running here", plus the reason the socket probe gives) and keeps checking. When a
   player starts, the TUI switches to the main screen on its own.
   - **Permission denied is a different message.** It means a player is running but this user
     may not connect, which is the system service without `sudo`. The screen says so and names
     the command to use, for example
     `sudo sendspin-cli tui --control-socket /run/sendspin-cli/control.sock`, rather than
     claiming no player is running.
5. **Every platform the player ships on.** Linux (x86_64, arm64, armv7, armv6), macOS arm64 and
   Windows x86_64. It must also work over SSH to a Pi.

## Non-goals for this version

These are left out for now, and are later candidates:
- album artwork;
- the spectrum visualizer;
- album-colour theming;
- an mDNS server picker.

The TUI never starts, stops or restarts the player, and never edits the config file.

**Access without `sudo` under the Linux system service is deferred.** Under
`--system-service` the socket is `/run/sendspin-cli/control.sock`, mode `0600`, owned by the
`sendspin-cli` account, so the TUI runs as `sudo sendspin-cli tui` there, just as `status`
does today. The user decided to ship version 1 that way and revisit later. The candidate is a
`sendspin-cli` group with a `0660` socket, which has a security cost: group members could read
the pairing token. Add that to the roadmap. Version 1 changes nothing about the socket's
ownership or mode.

## Anti-goals

These would make the TUI a failure even if it "works":
- **A second player, identity or pairing store.** This is the Python TUI's flaw.
- **Pairing that is easy to miss.** That means a code smaller than the rest of the screen, a
  code that appears late, or a failure that vanishes without a word.
- **Showing the pairing token without being asked.** It is a password; showing it takes a
  deliberate key press.
- **Breaking existing consumers of `status`.** The Windows installer and users' scripts parse
  the text output, including the `paired servers:` line. The human format stays as it is.
- **The TUI disturbing playback.** A TUI that crashes, hangs or polls too often must never cost
  the player an audio dropout.

## Decided

The user decided these:
- It attaches to the running player and never runs its own.
- The pairing takeover shows the dynamic code, impossible to miss (Goal 2).
- Version 1 copies the Python TUI's screen and keys, minus the non-goals.
- The player starts reporting the album, the stream's codec and the group's name, which the
  screen needs.
- I proposed three defaults and the user accepted them:
  - **Same binary:** it ships as the `tui` subcommand of `sendspin-cli`, not a separate
    program.
  - **FTXUI:** the UI is C++ on FTXUI, fetched with FetchContent and pinned like the other
    dependencies. sendspin-cpp's own `examples/tui_client` uses FTXUI too.
  - **Machine-readable status:** the player gains a status format meant for programs, and the
    TUI reads that rather than the human text.

## The screen

From the Python TUI (`sendspin/tui/ui.py`). At 80 columns or more:

```
+-Now Playing-------------------------------+-Volume------------------+
| Title:  Says                              | Group:   55%            |
| Artist: Nils Frahm                        | Player:  80%   [MUTED]  |
| Album:  Spaces                            |                         |
|      <- prev   <space> pause   next ->    | up/dn player  m mute    |
|                                           | [/] group     M mute    |
+-------------------------------------------+-------------------------+
+-Progress------------------------------------------------------------+
| [=====================>-------------------------------]  2:05 / 9:03 |
+---------------------------------------------------------------------+
+-Playback-------------+-Stream----------------+-Server----------------+
| Repeat:  off         | Codec: FLAC           | Music Assistant       |
| Shuffle: off         | 48 kHz / 16-bit / 2ch | connected, paired     |
|                      | Delay: 0 ms           | Group: Living room    |
| r repeat  x shuffle  | ,/. adjust delay      | g switch group        |
+----------------------+-----------------------+-----------------------+
                                                       p pairing token  q quit
```

Below 80 columns, the panels stack vertically.

When nothing is playing, the Now Playing panel shows "Press <space> to play" and "Press g to
switch group".

When no server has ever paired with this player (`paired servers` is 0) and no attempt is
under way, the Server panel says so: "Not paired yet. In Music Assistant, add this player and
choose to pair with a code."

### Keys

| Key | Action | Control command |
|---|---|---|
| space | play or pause, depending on the current state | `play` / `pause` (both exist) |
| left / right | previous / next | `prev` / `next` (exist) |
| up / down | this player's volume, +/- 5 | **new**, see below |
| `m` | mute or unmute this player | **new**, see below |
| `]` / `[` | group volume, +/- 5 | `vol` (exists) |
| `M` | mute or unmute the group | `mute` (exists) |
| `r` | cycle repeat: off, all, one | `repeat` (exists) |
| `x` | toggle shuffle | `shuffle` (exists) |
| `g` | switch group | `switch` (exists) |
| `,` / `.` | output delay -/+ 10 ms, kept within 0-5000 | `delay` (exists) |
| `p` | show or hide the pairing token | `pair-token` (exists) |
| `y` / `n` | allow or refuse a waiting pairing, only during the static-code takeover | `pair confirm` / `pair cancel` (exist) |
| `q`, Ctrl-C | quit the TUI; the player keeps running | |

A key the server does not offer (exit status 5, "server does not offer that command") shows a
short notice on the screen rather than failing silently.

## Changes the player needs

1. **Machine-readable status.** A program-facing form of `status`, alongside the existing text
   form.
   - **Fields:** everything the text form carries, plus:
     - the album;
     - the stream codec;
     - the group name;
     - the position and duration as raw milliseconds, together with the time they were taken,
       so the TUI can move the progress bar between polls;
     - the volumes and mutes as plain numbers and booleans.
   - **Stability:** the keys are documented, and a new player may add keys but never renames
     one. The TUI must tolerate unknown keys, and missing keys from an older player.
   - **Shape:** the exact syntax and its name on the command line are the builder's call.
     Options include `status --format kv` with `key=value` lines, or JSON. The format must be
     parseable without a JSON dependency on the client side, unless one is already in the tree.
2. **Album, codec and group name.** The library already has them:
   - `MetadataRole`'s `album`;
   - `ServerPlayerStreamObject::codec` from `stream/start`;
   - `GroupUpdateObject::group_name`, through `SendspinClient::get_group_state()`.

   Carry them into `StatusSnapshot`. Add them to the human `status` text too, as new lines;
   new lines don't break line-oriented parsers.
3. **This player's own volume and mute.** Today `vol` and `mute` address the *group*. Add
   commands for this endpoint's player volume and mute, with names the builder chooses (for
   example `player-vol` and `player-mute`). They:
   - are answered locally, like `delay`;
   - apply to the sink;
   - persist, as a server-set volume does;
   - publish `client/state`, so the server and every controller see the change. The spec
     allows a volume change made through device controls.

   PR #96's `PlayerListener::on_sink_volume_changed` already does the "applied locally,
   publish it" half for a hardware mixer. Under `--fixed-volume` the commands are refused,
   with a reason.
4. **How the last pairing attempt ended.** Today `PairingListener` (`src/main.cpp`) clears the
   server and code the moment an attempt ends, and the reason for a failure goes only to the
   log. A wrong code fails within a second, so a TUI polling once a second would see the code
   vanish and could not tell success from failure. Goal 2 cannot be built without this
   change.
   - **Keep the last attempt's outcome in `StatusSnapshot`:**
     - an attempt counter that increases with each attempt, so the TUI can tell a new outcome
       from one it has already shown;
     - the server;
     - the outcome: paired, failed, or abandoned (the server cleared the code with neither
       success nor failure);
     - the failure reason.
   - **Both formats get them.** They go in the machine-readable status, and the human text gets
     a `last pairing:` line, for example
     `last pairing: failed with Music Assistant -- the pairing code did not match`. *(My call:
     a script or installer waiting on a pairing has the same need as the TUI.)*

## Constraints

- **The control socket protocol stays as it is:** one command per connection, the first reply
  line `ok` or `error <kind>: <reason>`, and a `status` payload after `ok`. The player serves
  the socket from its main loop, every 10 ms. Polling `status` once a second (faster during a
  pairing takeover is fine) is well within budget. A streaming or subscribe mode is
  explicitly *not* required for this version.
- **The library's threading rule holds.** Everything the control handler reads is main-loop
  state. The TUI changes nothing about the player's threads.
- **The build:**
  - `clang-cl` with `-Werror` and the static CRT on Windows, the CI matrix's compilers
    elsewhere, and `scripts/format.sh` (clang-format 20.1.8).
  - FTXUI must build on every leg, including the Raspbian armv6 container and the
    `linux-x86_64-nomdns` leg.
  - Pin it by tag or SHA, as sendspin-cpp and googletest are pinned.
- **Binary size matters on the Pi.** Report the size growth in the PR. The project has not
  set a budget for the TUI; if FTXUI's cost is surprising, raise it before going further
  rather than working around it.
- **A terminal is required.** With stdin or stdout not a TTY, `tui` exits non-zero with a
  one-line message.
- **Windows:**
  - It finds the socket in the same `%LOCALAPPDATA%` location the subcommands use.
  - It must work in Windows Terminal and in the classic console host.
- **Tests:** the parts that are not drawing are unit-tested:
  - parsing the machine-readable status, including unknown and missing keys;
  - deciding which screen to show;
  - mapping keys to control requests;
  - the play/pause choice;
  - the repeat cycle;
  - the delay clamp.

  The new player commands and the snapshot fields get the same kind of tests the existing
  subcommands have.
- **Docs:** a wiki page for the TUI, a row in the README's command list, and the new status
  fields in "Controlling the Player". The roadmap's item 9 is marked done.

## Left to the builder

- FTXUI's component structure, colours, borders and the large-digit font for the pairing code.
- The exact machine-readable syntax, the flag that selects it, and its documentation.
- Refresh cadence beyond "a pairing code appears within about a second". The progress bar
  should move smoothly between polls.
- How long success and failure messages stay on screen.
- Whether the new player-volume commands become user-facing subcommands too. They will be,
  since they're control commands, so name and document them for people as well.
- How the TUI shows a failed command, and how long that notice stays.

## Interplay with open work

- **#115** re-pins sendspin-cpp and reads pairing from `is_paired()`. The TUI should consume
  pairing state only through the status snapshot, so it is unaffected by where the player
  gets that state from.
- **#110** serves the pairing token on an internal endpoint. The TUI's `p` key uses the
  existing `pair-token` command; it doesn't depend on #110.
- **#117** (the Windows installer prompts) is independent.
