# Pairing a Player

Every connection is encrypted, and a server has to **pair** with the player before it may
play on it. Pairing is done once per server: the player remembers it in its
[state file](Configuration#the-state-file), and from then on that server just
connects and plays.

There are two ways to pair. Both work from a terminal on the player's own host, and both
end the same way:

```console
$ sendspin-cli status | grep -E '^(trust|paired|pairing)'
trust: paired
paired servers: 1
pairing: none
```

`trust` is about the server connected right now; `paired servers` counts the servers this
player holds a pairing for, connected or not. An unpaired server drops out of it.

| Method | You carry to the server | Player setup | Use it when |
|---|---|---|---|
| [Dynamic code](#1-dynamic-code) | 6 digits, fresh each attempt | none | you can read the player's log or run `status` while pairing. This is the default |
| [Static code](#2-static-code) | 8 digits you chose | `pairing-code` | you want a code written down ahead of time, e.g. on a label |

Where a step says "in the server", it means the server's own screen for adding or pairing a
player; the wording differs from server to server. To pair from a script or another system
instead, see [the pairing token](Advanced-Usage#pairing-from-another-system).

## 1. Dynamic code

The player makes up a 6-digit code for each attempt and shows it to you; you type it into
the server.

1. In the server, choose this player and ask to pair with a code.
2. Read the code on the player's host, from the log or from `status`:

   ```
   I cli: *** Pairing code: 211030 *** -- type it into the server to pair
   ```

   ```console
   $ sendspin-cli status | grep pairing
   pairing: in progress with ZWOKuw7JVK2W…
   pairing code: 211030
   ```

3. Type it into the server.

The code is withdrawn when the attempt ends, whichever way it ends. After repeated wrong
codes the player holds further attempts back until you run `sendspin-cli pair confirm` (see
[the pairing window](#the-pairing-window)).

## 2. Static code

A code you choose once, for a player you cannot easily read a fresh code from.

1. Pick 8 random digits and give them to the player, in the
   [config file](Configuration):

   ```ini
   pairing-code = 48151623
   ```

   `--pairing-code 48151623` works too, but a flag is visible to every local user in `ps`.
   Anything that is not exactly 8 digits stops the player at startup. Restart the player
   after changing it.

2. In the server, choose this player, ask to pair with its static code and type the 8 digits.
3. The player now waits for you to allow it:

   ```
   I cli: A server is waiting to pair -- run 'sendspin-cli pair confirm' to let it, or 'sendspin-cli pair cancel' to refuse
   ```

   ```console
   $ sendspin-cli pair confirm
   ```

A static code never changes, so every attempt with it needs that `pair confirm`: the code
alone does not let a server in. A player with a static code offers it **instead of** the
dynamic code.

## The pairing window

`pair confirm` is this player's stand-in for pressing a button on a device. It answers the
attempt that is waiting, and then keeps a **pairing window** open for 5 minutes, during
which that server may retry without another confirm. The window closes early when a pairing
succeeds, when the connection drops, or after five failed attempts.

```console
$ sendspin-cli status | grep pairing
pairing: in progress with YkOLFE2fZeml…
pairing window: open -- run 'sendspin-cli pair confirm' to let the server pair, or 'sendspin-cli pair cancel' to refuse it
```

`sendspin-cli pair cancel` refuses a waiting attempt and closes the window. The
`pairing window` line is only there while an attempt is waiting on you.

## When it does not pair

A failed attempt is logged with its reason:

```
W cli: Pairing with server wn_sX1iDNhQ… failed: the pairing code did not match
```

- **`sendspin-cli status` says nothing is listening** — the player is not running, or it
  runs on another `--port`; see [Finding the socket](Controlling-the-Player#finding-the-socket).
- **The player forgets its pairings on every restart** — it has no state directory. See
  [Running as a Service](Running-as-a-Service).

To let any server play without pairing at all, see
[`--allow-unpaired`](Advanced-Usage#pairing-and-unpaired-access).
