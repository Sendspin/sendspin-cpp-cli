# Getting Started on Windows

From nothing to a player your Sendspin server can find, running whenever you are logged on.

**You need:** Windows 10 1809 or later, or Windows 11, on x64 (Windows on ARM runs the x64
build through its emulation), and something to play through. You do **not** need
administrator rights, except for the optional firewall rule.

## The short way

```powershell
Invoke-WebRequest https://raw.githubusercontent.com/Sendspin/sendspin-cpp-cli/main/scripts/get_started_windows.ps1 -OutFile get_started_windows.ps1
notepad get_started_windows.ps1    # it is about to install things; read it
powershell -ExecutionPolicy Bypass -File .\get_started_windows.ps1
```

It prints what it is about to do, as you, and waits for you to say yes:

```
==> This is what it will do, as you, with no administrator rights

  install   C:\Users\you\AppData\Local\Programs\sendspin-cli\  (sendspin-cli.exe and its README, LICENSE and config example)
  config    C:\Users\you\AppData\Roaming\sendspin-cli\config  (from the example; every line in it is commented out)
  PATH      add C:\Users\you\AppData\Local\Programs\sendspin-cli to your user PATH, so 'sendspin-cli status' works anywhere
  task      'sendspin-cli' in Task Scheduler: start the player, without a window, when you
            log on, logging to C:\Users\you\AppData\Local\sendspin-cli\sendspin-cli.log; it checks every minute
            and starts the player again if it has stopped
  start     the task, now

Go ahead? [y/N]
```

`-Yes` skips the prompt, and is required when there is no console to ask on. `-Version v0.5.0`
installs a specific release instead of the newest. `-Firewall` adds the firewall rule without
asking, though Windows still asks for administrator approval.

### What it actually does

1. **Checks this is Windows 10 1809 or later**, where the player's control socket and mDNS
   both work, on x64 or ARM64.
2. **Finds the newest release** and downloads its `windows-x86_64` zip plus `SHA256SUMS`.
3. **Verifies the checksum**, and stops without installing anything if it does not match.
4. **Installs into your own LocalAppData**, `AppData\Local\Programs\sendspin-cli`, and clears
   the mark Windows puts on downloaded files, since it has just verified them. A player
   already running from there is stopped cleanly first, so running the script again upgrades
   it.
5. **Copies the annotated example config** to `AppData\Roaming\sendspin-cli\config` if you
   have none. Every line in it is commented out, so it chooses nothing: the player plays
   through your **default output device**, and follows it when you change it.
6. **Adds the install folder to your user PATH**, so `sendspin-cli status` works in any new
   terminal.
7. **Registers a scheduled task** that runs `sendspin-cli -z` when you log on, with no window.
   The task also checks every minute: while the player runs, that check finds its pidfile
   held and does nothing, and if the player has stopped, it starts it again.
8. **Starts it now**, and shows the last lines of its log.
9. **Offers the firewall rule** — see [Letting a server reach it](#letting-a-server-reach-it).

## Pair it

Your Sendspin server finds the player over mDNS. When you pair it from the server, the
player prints a six-digit code in its log; type it into the server:

```powershell
Get-Content $env:LOCALAPPDATA\sendspin-cli\sendspin-cli.log -Wait -Tail 20
```

```
I cli: *** Pairing code: 110948 *** -- type it into the server to pair
```

Each pairing attempt has its own code, so keep the server's dialog open while you read it.
Closing the dialog cancels the attempt and withdraws the code. [Pairing a Player](Pairing-a-Player)
has the rest.

## Drive it

```powershell
sendspin-cli status        # what it is doing
sendspin-cli vol 30        # and the other subcommands
```

These talk to the player over its control socket, in your LocalAppData, which only you can
reach. See [Controlling the Player](Controlling-the-Player).

**To restart it with new settings**, stop it; the task starts it again within a minute:

```powershell
taskkill /PID (Get-Content $env:LOCALAPPDATA\sendspin-cli\sendspin-cli.pid)
```

`taskkill` without `/F` is a clean stop: the player says goodbye to the server first. **To keep
it stopped**, disable the task before stopping it, and enable it again to bring it back:

```powershell
Disable-ScheduledTask sendspin-cli
taskkill /PID (Get-Content $env:LOCALAPPDATA\sendspin-cli\sendspin-cli.pid)

Enable-ScheduledTask sendspin-cli
```

`Stop-ScheduledTask` is **not** the way to stop it: the task has already handed over to the
detached player, so there is nothing for it to end.

## Letting a server reach it

A server finds the player over mDNS and then **connects to it** on port 8928. Windows
Firewall blocks that connection until a rule allows it, and adding a rule needs
administrator rights, so the script offers it apart from everything else:

```powershell
New-NetFirewallRule -DisplayName sendspin-cli -Direction Inbound -Action Allow -Program "$env:LOCALAPPDATA\Programs\sendspin-cli\sendspin-cli.exe" -Protocol TCP -Profile Private
```

The rule covers **private networks only**. If Windows has your home network marked Public,
change that in Settings → Network & internet, or the rule does not apply.

Without the rule, the player can still **dial a server itself**: put `server = <host>` in the
config, or `server = mdns:` to find one over mDNS. See
[Connection modes](Advanced-Usage#connection-modes).

## SmartScreen

The release is **unsigned**, so the first time it runs, Windows SmartScreen may say it
protected your PC. The script checks the zip against the `SHA256SUMS` the release publishes
before installing it; if you got the binary some other way, check it the same way. A signed
build is tracked in the [roadmap](https://github.com/Sendspin/sendspin-cpp-cli/blob/main/docs/ROADMAP.md).

## Where things are

| What | Where |
|---|---|
| The player | `%LOCALAPPDATA%\Programs\sendspin-cli\sendspin-cli.exe` |
| Your config | `%APPDATA%\sendspin-cli\config` |
| What it remembers: its identity and pairings, volume, last server | `%LOCALAPPDATA%\sendspin-cli\state` |
| Its log | `%LOCALAPPDATA%\sendspin-cli\sendspin-cli.log` |
| Its pidfile | `%LOCALAPPDATA%\sendspin-cli\sendspin-cli.pid` |
| Its control socket | `%LOCALAPPDATA%\sendspin-cli-8928.sock` |

The state file and the control socket's lock are readable by you alone. The log is never
rotated, so it only grows; it is safe to delete while the player is stopped.

## Uninstalling

```powershell
Disable-ScheduledTask sendspin-cli
taskkill /PID (Get-Content $env:LOCALAPPDATA\sendspin-cli\sendspin-cli.pid)
Unregister-ScheduledTask sendspin-cli -Confirm:$false
$dir = "$env:LOCALAPPDATA\Programs\sendspin-cli"
[Environment]::SetEnvironmentVariable('Path', (([Environment]::GetEnvironmentVariable('Path', 'User') -split ';' | Where-Object { $_ -and $_ -ne $dir }) -join ';'), 'User')
Remove-Item $dir -Recurse
```

Your config, state and log are left where they are, so reinstalling keeps the player's
identity and pairings. Remove `%APPDATA%\sendspin-cli` and `%LOCALAPPDATA%\sendspin-cli` too
to forget it entirely; a server then sees a new, unpaired player. The firewall rule, if you
added it, goes from an administrator PowerShell:

```powershell
Remove-NetFirewallRule -DisplayName sendspin-cli
```

## Without the script

Take `sendspin-cli-<version>-windows-x86_64.zip` and `SHA256SUMS` from
[Releases](https://github.com/Sendspin/sendspin-cpp-cli/releases), check one against the
other, and run it from wherever you unpacked it:

```powershell
Get-FileHash .\sendspin-cli-0.5.0-windows-x86_64.zip -Algorithm SHA256   # compare with SHA256SUMS
Expand-Archive .\sendspin-cli-0.5.0-windows-x86_64.zip .
Get-ChildItem .\sendspin-cli-0.5.0-windows-x86_64 | Unblock-File
.\sendspin-cli-0.5.0-windows-x86_64\sendspin-cli.exe -l
.\sendspin-cli-0.5.0-windows-x86_64\sendspin-cli.exe
```

`-l` lists what it can play through. Windows lists each device once per host API, so pick one
by that column and its name, e.g. `-o "portaudio:directsound:Speakers (USB DAC)"`; an index
works too, but shifts as devices come and go. Ctrl+C and closing the window stop it cleanly,
and it handles logging off and shutting down the same way.

## Next

- [Configuration](Configuration) — every setting, and how the config file is found
- [Controlling the Player](Controlling-the-Player) — the subcommands
- [Troubleshooting](Troubleshooting#windows) — when nothing finds it
