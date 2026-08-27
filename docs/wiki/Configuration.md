# Configuration

Two files. **One you write and the player only ever reads**, and **one the player writes and
you never need to touch.** The split is deliberate: a daemon that rewrote its own config
would destroy the comments and the ordering you put there, and a config the daemon could not
write would have nowhere to record a volume.

## The config file

Anything you would otherwise type on the command line. **Keys are the long flag names
without their dashes**, one `key = value` per line, and a value is exactly the string that
flag would have been given — so `sendspin-cli --help` is this file's reference rather than a
second document to keep in step with it.

```ini
# /etc/sendspin-cli.conf
name = kitchen
output = hw:1,0
buffer-ms = 250
static-delay = 40
control-socket = /run/sendspin-cli/control.sock
```

An annotated example is installed at
`/usr/local/share/doc/sendspin-cli/sendspin-cli.conf.example`, with every line commented
out. Copying it is the intended way to start:

```bash
sudo cp /usr/local/share/doc/sendspin-cli/sendspin-cli.conf.example /etc/sendspin-cli.conf
```

### Where it is looked for

The **first of these that exists is read whole**, and nothing below it is merged over the
top:

1. `--config <path>` — **fatal if it cannot be read**, because you named it. Falling back
   would start a player on options nobody chose.
2. `$XDG_CONFIG_HOME/sendspin-cli/config`
3. `$HOME/.config/sendspin-cli/config`
4. `/etc/sendspin-cli.conf`

Finding none is silent and normal. There is no `--no-config` flag — `--config /dev/null`
already does that.

### Every key

| Key | Same as | Value | Default |
|---|---|---|---|
| `output` | `-o`, `--output` | a device: `hw:1,0`, `default`, `portaudio:2`, `null`, `stdout` | `default` where ALSA is built in, else `portaudio`, else `null` |
| `name` | `-n`, `--name` | the friendly name a controller shows | this host's name |
| `id` | `--id` | the stable client id a server files this player's settings under — two players on one host must not share it | derived from the interface MAC |
| `manufacturer` | `--manufacturer` | the manufacturer `client/hello` reports | `sendspin-cpp-cli` |
| `product-name` | `--product-name` | the product name `client/hello` reports | `sendspin-cli` |
| `server` | `-s`, `--server` | `<host>[:<port>]`, a `ws://` URL, or `mdns:[<name>]` | none — wait to be discovered |
| `port` | `--port` | the port this player's own WebSocket server listens on | `8928` |
| `buffer-ms` | `--buffer-ms` | audio the output backend keeps queued, 10–2000 | `100` |
| `static-delay` | `--static-delay` | latency this endpoint's hardware adds after the audio port, 0–5000 | `0` |
| `no-mdns` | `--no-mdns` | `true`/`false` — do not advertise `_sendspin._tcp` | `false` |
| `mdns-name` | `--mdns-name` | the instance label to advertise, when it should differ from `name` | `name` |
| `control-socket` | `--control-socket` | the Unix socket the subcommands talk to | `$XDG_RUNTIME_DIR/sendspin-cli-<port>.sock` |
| `no-control` | `--no-control` | `true`/`false` — bind no control socket at all | `false` |
| `state-dir` | `--state-dir` | where the player keeps what it remembers | `$XDG_STATE_HOME/sendspin-cli` |
| `hook-start` | `--hook-start` | a shell command run when a stream starts — an amplifier relay, a light | none |
| `hook-stop` | `--hook-stop` | the same, when a stream stops | none |
| `log-level` | `-d`, `--log-level` | `none`, `error`, `warn`, `info`, `debug`, `verbose` | `info` |
| `logfile` | `-f`, `--logfile` | write the log here instead of to stderr | stderr |
| `pidfile` | `-P`, `--pidfile` | hold this path as a locked pidfile | none |

The middle column is the point rather than a convenience: the key **is** the long flag name
minus its dashes, which is what makes `sendspin-cli --help` this file's reference. The six
that had a short flag first — `-o -n -s -P -f -d` — grew long spellings for exactly this
reason, so that one vocabulary covers both.

Booleans take `true`/`yes`/`on`/`1` or `false`/`no`/`off`/`0`. A line whose first non-blank
character is `#` is a comment; a `#` anywhere else is not, so a name or a path is free to
contain one. Where a key appears twice, the last one wins.

**A `server` URL that carries userinfo belongs here rather than on the command line — and it
does not authenticate anything.** `ws://user:token@host:8927/sendspin` is accepted, and the
`user:token` is dropped before the handshake: Sendspin authenticates in the handshake, not in
the URL, so nothing turns userinfo into an `Authorization` header. If a proxy in front of
your server wants HTTP Basic, this is not the way to give it to it — and the player accepting
a credential it cannot send is a wrong it owes a fix, not a feature to configure around.

Where it still matters is what gets written down. A URL typed at `-s` is in the process's
`argv`, which `ps` shows to **every** local user for as long as the player runs; in this file
it is protected by the file's own permissions, so `chmod 0600` and an owner is the whole of
the fix. The player also masks userinfo out of every line *it* writes — `Connecting to
ws://user:***@host:8927/sendspin` — but every line tagged `sendspin.*` is the library's own
and prints the URL in full, at the default log level. Treat the log of a `-s` run with
userinfo in it as sensitive either way.

**Five things cannot come from a file**: `-l`, `-z`, `--config`, `--help` and `--version`.
Run shape stays on the command line, and a config naming one is refused as an unknown key.
Excluding them is reversible; debugging a `daemonize` that came out of a file under systemd
is not.

### Precedence

**Command line > config file > built-in default**, per option rather than per file. `-n
bathroom` on the command line of a player whose config also sets `buffer-ms` overrides only
the name.

That is why the system unit's own two flags win: `ExecStart` passes `--control-socket` and
`--state-dir`, so those two keys in a config file are **silently ignored** by the service.
Setting `control-socket` to the unit's path is still worth doing — it is what lets a
*subcommand* find the socket with no flags.

### It refuses rather than guessing

A configured value is validated by exactly the code that validates a typed one, with the
same message and the line to go and fix:

```console
$ sendspin-cli
error: /etc/sendspin-cli.conf:4: invalid --buffer-ms '5' -- expected 10-2000
```

An unknown key, or a line that is not `key = value`, is refused the same way — and a file
that exists and does not parse stops the run rather than falling through to `/etc`. A
silently ignored typo is the failure mode this whole surface exists to avoid. `--help`,
`--version` and `-l` short-circuit above all of it, so a broken config cannot stop `--help`
from telling you how to fix it.

Under systemd that means a bad config is a unit that fails and is retried every five
seconds, naming the file and the line in the journal each time. See
[Troubleshooting](Troubleshooting).

**Two keys parse and then fail under the shipped unit.** `logfile` and `pidfile` pointing
anywhere but `/run/sendspin-cli` or `/var/lib/sendspin-cli` are refused by
`ProtectSystem=strict` — `Read-only file system`, loudly, on every restart. Neither is the
shape for a unit whose stderr journald already has. See
[Running as a Service](Running-as-a-Service#what-is-hardened).

The full argument, including why the search does not merge layers, is in
[The config file, and what the player remembers](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/README.md#the-config-file-and-what-the-player-remembers).

## The state file

The other half: what the player remembers **for itself**, across restarts. You do not write
this one.

```ini
# Written by sendspin-cli. Edits are overwritten.
last-server = 7f3a…
last-server-hash = 3387423128
muted = true
static-delay-ms = 375
volume = 42
```

| Key | What it is |
|---|---|
| `static-delay-ms` | this endpoint's static delay. The spec **requires** a client to persist it |
| `volume`, `muted` | the gain and mute the output was last told to apply. RECOMMENDED by the spec |
| `last-server` | the server id, which mDNS discovery uses to break a tie between candidates |
| `last-server-hash` | an opaque `uint32_t` the library asks us to keep so *it* can prefer the last-played server among inbound connections |

The two server keys mean different things and are deliberately not reconciled with each
other.

It lives at `$XDG_STATE_HOME/sendspin-cli/state`, then
`$HOME/.local/state/sendspin-cli/state`, and **`--state-dir <dir>` overrides both** — a
systemd *system* unit has neither variable and is handed `/var/lib/sendspin-cli` by
`StateDirectory=`. With none of the three the player still runs and simply remembers
nothing.

Under the unit the directory and the file belong to the unprivileged `sendspin-cli` account,
and a `/var/lib/sendspin-cli` left root-owned by an earlier root-run version needs nothing
done to it — `StateDirectory=` chowns a directory it finds as well as one it creates, so what
was remembered carries over. See
[Running as a Service](Running-as-a-Service#upgrading-from-a-version-that-ran-as-root).

Writes go through a temporary, an `fsync` and a `rename` at mode `0600`, so a player that
loses power mid-write leaves either the old file or the new one and never half of either.

**Two players on one host share this file** unless you give each its own `--state-dir`. They
already need different `--id`s and `--port`s; give them different state directories too, or
the second one to save its volume overwrites the first's.

### `static-delay` versus a remembered `static-delay-ms`

`static-delay` in the config is a **first-run default, not an override**. The library prefers
whatever the state store remembers and reads the config value only when there is nothing
remembered — exactly as a restored volume beats the sink's default. So once a server or
`sendspin-cli delay` has set one, the remembered value wins every run after and the config
key is inert. Nothing in the log names which of the two won, so `sendspin-cli status` is
where you read the value actually in force.

Three things can set it: a server's `set_static_delay`, the `delay` subcommand, and this key
on a first run with nothing yet remembered. To make the config value take again, remove the
state file.

## Applying a change

The player reads its config once, at startup. There is no reload signal:

```bash
sudo systemctl restart sendspin-cli
```

`SIGHUP` reopens the `-f` logfile and nothing else — that is for `logrotate`, not for
configuration.

## Next

- [Controlling the Player](Controlling-the-Player) — what you can change without a restart
- [Running as a Service](Running-as-a-Service) — the two flags the unit passes, and why
- [Troubleshooting](Troubleshooting)
