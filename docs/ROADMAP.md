# Roadmap

`sendspin-cli` is a headless [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) player
that advertises itself, is discovered, and is driven remotely. Each item below is work still
owed. What already ships is described in `README.md` and the
[wiki](https://github.com/Sendspin/sendspin-cpp-cli/wiki); git history holds how each piece
got there.

## Open items

### 1. PortAudio on Linux

PortAudio is now a Linux-only backend in practice (macOS releases use CoreAudio), and it has
only ever been driven on macOS.

- **Raw libasound diagnostics.** `AlsaAudioSink` routes libasound's stderr through the CLI
  logger, but the handler is installed only from the ALSA sink's and source's own paths. On a
  dual-backend host, `-o portaudio:...` lets PortAudio's ALSA host API print raw
  `ALSA lib pcm.c:...` lines during device enumeration. (`-l` is clean, because the ALSA
  section runs first.)
- **No Linux field pass.** `-l`'s PortAudio section, playback against a real server (sync
  convergence, controller volume, pause/resume/next) are unexercised on Linux. CI only builds
  the configuration.

### 2. Refuse a `-s` URL carrying userinfo

`-s ws://user:token@host/...` is accepted, and the credential is masked in our log lines
(`redact_url_userinfo()`), but neither IXWebSocket nor the library ever sends it: the upgrade
request carries no `Authorization` header. Masking has two holes: a credential in the query
string, and userinfo holding an unencoded `/`, `?` or `#`. Refusing userinfo at parse time
closes both and stops an operator believing the player authenticates. **Breaking** for anyone
passing such a URL today.

### 3. Upstream gaps in sendspin-cpp

Each needs a library change; nothing this repo can call fixes them. Re-check on every
`SENDSPIN_GIT_TAG` bump (currently `b0fcf5bb`, past v0.9.5).

- **No log sink hook.** The host `SS_LOG*` macros are bare `fprintf(stderr, ...)` gated on one
  global level. So library lines carry no timestamp under `-f`, cannot be filtered per tag
  (`-d <category>=<level>` stays warn-and-ignore), and print the dialled URL unmasked —
  `Initiating client connection to:` at `info` and `WebSocket error on connection to` at
  `error`. That last one is why item 2 matters.
- **A taken `--port` is invisible to the CLI.** The library logs `Error starting server` and
  retries, but exposes no signal, so our `listening on port` line is still printed and the
  player stays up with no listener.
- **No connection origin.** The library does not say whether the live connection was dialled
  or accepted. After a literal `-s <address>` dial, `SENDSPIN_SERVER_URL` in a hook is that
  address even if a different server dialled in; `SENDSPIN_SERVER_ID` is the reliable field.

### 4. `switch` with more than one group

`sendspin-cli switch` has only been exercised against a server with a single group, where the
cycle returns to where it began. The multi-group path is unproven.

### 5. Debounce the state-file write

Every distinct volume a server sends rewrites the state file whole, with an `fsync`, on the main
loop. A slider drag costs at most one write per main-loop tick, and audio is unaffected, but on
a slow SD card it can delay the control socket and mDNS polling, and it wears the card. The fix
is a dirty flag flushed on a timer and at shutdown, keeping the file correct at any instant
power may be lost.

### 6. Docker

Image and compose file. ALSA with `/dev/snd` passed through for real output, and the
null sink for device-less containers and CI.

### 7. macOS signing and notarization

Gated on a paid Apple Developer enrolment. Everything shipped is ad-hoc signed, so
`spctl -a -t exec` rejects the binary and `spctl -a -t install` rejects the `.pkg`;
`xattr -d com.apple.quarantine` is a workaround, not a fix.

- A Developer ID Application and a Developer ID Installer certificate, then notarization.
  Staple to the `.pkg`: `xcrun stapler` refuses a bare Mach-O.
- `scripts/build_macos_pkg.sh` grows `--sign` on its two calls, or a `productsign` over the
  finished archive. Secrets are absent on fork pull requests, so the step must be conditional.
- **A minimum-macOS gate.** Nothing sets `CMAKE_OSX_DEPLOYMENT_TARGET`, so the binary's `minos`
  is whatever the runner's SDK defaults to, and the distribution XML declares no OS floor. Pin
  the deployment target, then declare it beside `hostArchitectures`.

### 8. systemd unit hardening against a real card

`PrivateDevices=`, `DeviceAllow=`, `ProcSubset=pid` and `RestrictRealtime=` are left out of
`packaging/sendspin-cli.service.in` because each gates what the ALSA backend reaches
(`/dev/snd`, `/proc/asound`, `SCHED_FIFO`) and all of them pass CI under `-o null`. Settling
them needs a player on hardware with a device configured, listened to. Do the four in one
sitting; `DeviceAllow=char-alsa rw` with `DevicePolicy=closed` is what the first three are
reaching for.

### 9. Interactive TUI mode

Optional, later. Upstream's `examples/tui_client` shows the shape. The control socket is one
command per connection, so a long-lived subscribing client is a new shape rather than an
extension of it.

### 10. CI and test gaps

- **A sink contract suite.** `NullAudioSink`, `AlsaAudioSink` and `PortAudioSink` (and the
  sound-server sinks) have no shared suite exercising the `AudioSink` contract itself — the
  largest gap in `tests/`.
- **`src/mdns_dnssd.cpp` has no unit coverage.** The poll/retire lifecycle, settle window,
  interface refcount and restart backoff are exercised only by hand. `Impl` is factored so a
  fake dns_sd could drive it.
- **A ThreadSanitizer leg.** Configure `-DCMAKE_CXX_FLAGS=-fsanitize=thread` and run
  `scripts/smoke_test.sh`. The main-loop-only threading model is otherwise verified only by
  hand.
- **An Intel-Mac build.** There is no `macos-x86_64` leg; the `.pkg` refuses an Intel Mac.
- **A lower glibc floor** for `linux-x86_64`, `linux-arm64` and `linux-armv7`, which need
  `GLIBC_2.38` and so refuse to load on Raspberry Pi OS bookworm (2.36). `linux-armv6` already
  loads there.
- **Drift guards.** Nothing checks that every key in `packaging/sendspin-cli.conf.example` is
  still accepted (feed the uncommented file through `config_file`), or that the artifact names
  spelled out in `docs/wiki/` match `project(VERSION)` — they still say `0.1.0`.

### 11. Device-recovery hardware passes

Recovery logic is unit-tested in `src/sink_recovery.{h,cpp}`; the device calls around it are
not. CoreAudio has had its pass. Owed:

- **PortAudio** (`-o portaudio`): a USB DAC unplugged and left out mid-track (logs once, does
  not spin); the same DAC replugged mid-track (resumes without a track boundary); the host
  default output switched mid-track under a bare `-o portaudio`; normal playback, track changes
  and stop unaffected; and a measurement of how long the device-list rescan stalls the main
  loop.
- **ALSA** (`-o hw:CARD=<name>`): a USB DAC powered off mid-track (two `ERROR`s, then quiet);
  powered back on inside the retry budget (resumes with no track boundary); powered back on
  after the budget (recovers on the next stream, not before); two outage/recovery cycles in one
  stream; and a measurement of how long `snd_pcm_open()` stalls the loop on an absent `hw:`
  device.
- **PortAudio: a replug between tracks is not found.** The next stream resolves against
  PortAudio's stale device list and fails as though the format were refused. Telling the two
  apart means classifying `Pa_OpenStream()` errors per host API, which wants hardware to check.
  `--input portaudio` also blocks the rescan, since PortAudio's init is reference-counted.
- **`extra_startup_silence_ms`** stays at the library's 50 ms until underflow is measured on
  real hardware across backends.

### 12. ALSA hardware mixer (`-V`)

`snd_mixer_*` where the device has a real control to drive — chiefly `hw:` output straight
to a card, where a hardware mixer is the actual volume rather than a scaling of the samples.

Keep `src/pcm_volume.{h,cpp}` for plugin devices: the usual `default` PCM is PipeWire's ALSA
plugin, where a mixer element is either absent or moves something other than this stream.
One path or the other per device, chosen at open time — never both stacked, which would
square the taper.

### 13. Avahi-native mDNS backend

Linux only, and a *second* backend beside the `dns_sd.h` one rather than a replacement. The
`MdnsService` seam (`src/mdns.h`, one implementation per build chosen by CMake) is what makes it
a drop-in.

What it buys, all of it specific to `libavahi-compat-libdnssd`:

- **Daemon-restart recovery that is told to us.** The compat shim reports
  `kDNSServiceErr_ServiceNotRunning` on every ref once `avahi-daemon` restarts, which the
  current backend handles by re-registering on a backoff. Avahi's own client has a state
  callback that says it directly.
- **Collision signalling.** With `flags = 0` the daemon renames on a conflict and the
  register callback reports the new name — good enough, but Avahi names the collision.
- **No compat shim**, and so no `*** WARNING *** The program 'sendspin-cli' uses the Apache
  Portable Runtime...`-style stderr banner the shim prints on some distributions, outside
  the CLI logger entirely.
- **A first-class address resolver**, in place of the `DNSServiceQueryRecord` workaround for
  the `DNSServiceGetAddrInfo` the shim lacks.

### 14. PipeWire's ring is sized before the graph says what it wants

`process()` asks for a whole quantum, and a ring that cannot hold one zero-fills part of every
cycle for the life of the stream. The sink requests a latency (`PW_KEY_NODE_LATENCY`) and warns
with the `--buffer-ms` that would fit, but a graph with `default.clock.force-quantum` overrides
the request. Fixing it means observing the quantum and reopening the stream with a resized ring
on its first `process()` — a reconnect inside the audible path, so it wants the same care as
`SinkRecovery`.

**Also:** the `pipewire-minimum` job in `.github/workflows/build.yml` is the only build without
`-DSENDSPIN_CLI_WERROR=ON`, because of gcc 12's `-Wrestrict` false positive on
`line += " " + std::to_string(...)` in `src/control_common.cpp`. The ARMv6 build carries
`-Wno-error=restrict` for the same sites (`scripts/build_armv6_container.sh`). Rewriting them
lets both legs drop their exemption.

### 15. `--interface`

Bind the inbound listener — and scope the advertisement — to one interface, instead of every
address the host holds. **Still blocked upstream at v0.9.5:** `src/host/ws_server.cpp:61`
constructs `ix::WebSocketServer(server_port_, "0.0.0.0", ...)`, and `SendspinClientConfig`
carries `server_port` and `server_max_connections` but no bind address. (Its `mac_address`
override is identity in `client/hello`, not a socket.) The library needs a bind address threaded
through `ConnectionManager` into `SendspinWsServer`; there is no other route.

**Why it matters less than it did.** v0.9.5 authenticates inbound servers with PSK pairing and
activity-based admission, and this player exposes them (`--pairing-code`,
`--allow-unpaired`), so reachability is no longer authorization. What is left is
multi-homed hosts, VPSes, tunnels and untrusted VLANs, where a bind address narrows exposure and
an interface-scoped advertisement stops a server picking an unreachable address. A host firewall
covers the first today.

**The flag, settled.** `--interface <name-or-address>` (config key `interface`), long-only. One
value yields both halves: the bind needs an address, `DNSServiceRegister` an interface index.
A name goes through `if_nametoindex()` and `getifaddrs()` — prefer the first global v4, then a
global v6, and refuse a link-local v6. A literal address binds directly and finds its index by
scanning `getifaddrs()`; no match or several matches is a refusal. Shape is validated at parse
time (a name within `IFNAMSIZ`, or an address `inet_pton()` accepts); resolution happens at
startup, where the interfaces are real. The outbound `-s` dial stays kernel-routed. Scoping the
browse (`src/mdns_dnssd.cpp:180`) can follow as an extension.

The mDNS half alone is not access control: scoping the advertisement while the socket still
listens on `0.0.0.0` hides the player without protecting it. Both halves land together, or the
mDNS half lands under its own name.

The Python CLI's `--interface` takes an IP address only, and `aiosendspin` binds to it without
scoping its advertisement, so this flag is a superset of it.

### 16. Windows

The player runs on Windows 10 1809+ at parity with Linux and macOS, from a per-user install
that starts at logon. Still owed:

- **A Windows service**, so a player runs before anyone logs on. It needs Service Control
  Manager integration, a service account, a state directory under that account's profile,
  and a deliberate ACL on the control socket so an administrator can drive it — the
  counterpart of the `sudo` the Linux unit needs.
- **A system-wide config.** ProgramData lets any user create files, so a file there needs an
  ACL check (owned by, and writable only by, SYSTEM or Administrators) before a service could
  trust it.
- **Signing.** The exe is unsigned, so SmartScreen warns on first run.
- **Log rotation.** There is no SIGHUP to reopen `-f` on, so the logfile only grows.
- **Logoff and shutdown** are handled through the console control events and are untested.
- **PortAudio picks MME's default device**, with around 180 ms of device latency; WASAPI
  would cut that. `-l` lists the WASAPI devices, and `-o portaudio:<index>` picks one.
- **A no-mDNS build's `-s mdns:` refusal** names Linux packages to install, even on Windows.
