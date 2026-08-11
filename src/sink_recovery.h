// Copyright 2026 sendspin-cpp-cli Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file sink_recovery.h
/// @brief When a sink whose device died mid-stream should try to get it back, and when to stop

#pragma once

#include <atomic>
#include <cstdint>

namespace sendspin_cli {

/// @brief How long after an outage escalates the backend-wide device rescan is worth attempting.
///
/// Not politeness. The rescan is the last attempt there is, and the case it exists for -- a USB
/// DAC pulled out and pushed back in -- takes the host a moment to enumerate. Spending it the
/// instant the in-place reopen failed would usually spend it before the device is back, and
/// there is no second one. Two seconds clears a USB enumeration comfortably and is still well
/// inside a track.
///
/// It is also not free to the caller: rebuilding a device list means enumerating every host API,
/// which blocks whichever thread asks for hundreds of milliseconds to seconds. See
/// PortAudioSink::poll(), which pays that on the main loop.
///
/// **This is what bounds the rescan across streams, and the per-stream budget below is not.**
/// reset() refills that budget at every stream, and how often streams start is the server's
/// choice, not ours -- so on hardware that opens and dies repeatedly the budget alone would allow
/// one cycle per stream, however fast they came. Because every cycle needs a fresh escalation and
/// each escalation must then wait this long, cycles are floored this far apart whatever the
/// server does. Both halves of the bound are load-bearing; neither is decoration.
inline constexpr int64_t SINK_RESCAN_DELAY_MS = 2000;

/// @brief The decision half of getting a dead output device back mid-stream.
///
/// Device-free and clock-free on purpose, so it is compiled and tested on a host with no audio
/// backend at all: `src/portaudio_sink.cpp` is built only under `SENDSPIN_CLI_PORTAUDIO_ENABLED`
/// where `sendspin-cli-tests` is built unconditionally, so a policy living in the sink would go
/// untested exactly where PortAudio is absent. The same split `mdns_common.cpp` and
/// `pcm_volume.cpp` make.
///
/// It encodes two attempts, in that order, and then silence until the next stream:
///
///  1. **Reopen in place**, which `write()` makes inline the moment it finds the stream dead.
///     Cheap enough to try first, and it is the whole of what a host default-output switch needs.
///  2. **Rescan** the backend's device list, which only the main loop can do, and only
///     `SINK_RESCAN_DELAY_MS` after the reopen gave up. This is what a *replugged* device needs:
///     PortAudio enumerates at `Pa_Initialize()` and never revisits that list, so the device that
///     came back is one the list has never seen.
///
/// **Each is spent once per configured stream, not once per outage**, which is what stops a
/// half-present device -- a dock mid-handshake, a hub browning out -- from having `write()` call
/// `Pa_OpenStream()` on the sync task's thread fifty times a second. A stream that reopens and
/// dies again therefore goes straight to the rescan rather than reopening twice, and once both
/// are gone the sink discards until the next `configure()` -- which re-resolves the device
/// anyway, and against a list the rescan has already refreshed. `reset()` refills both, and only
/// a `configure()` that really got a stream running calls it.
///
/// THREAD SAFETY: every method but `pending()` must be called under whatever lock already
/// serialises the sink's stream, because the two askers are on different threads.
class SinkRecovery {
public:
    /// @brief Asks whether to reopen the device in place. For a write() that found no stream.
    ///
    /// Cheap enough to call on every write of an outage. At most one call per *configured
    /// stream* says yes -- not one per outage -- so a stream that was reopened once and has died
    /// again is told no, whether or not that first reopen worked.
    /// @return true if the caller should attempt the reopen now, and report back to
    /// reopen_done(). false means there is nothing left to try inline -- either the attempt is
    /// spent, in which case this escalates to the rescan, or that is spent too.
    bool reopen_due();

    /// @brief Records the outcome of the reopen that reopen_due() asked for.
    ///
    /// A success owes nothing further: the stream is playing again, and its rescan stays in hand
    /// for the next outage. A failure escalates, so rescan_due() starts counting.
    void reopen_done(bool recovered);

    /// @brief Asks whether to rescan the backend's device list. For the main loop's tick.
    /// @param now_ms Monotonic milliseconds. The delay is measured from the first call after the
    /// escalation, because the escalation itself happens on a thread with no business reading
    /// this clock -- one tick of slack against SINK_RESCAN_DELAY_MS.
    /// @return true at most once per configured stream, and never before the delay is up. There
    /// is no reporting call back: a rescan that failed and one that worked leave nothing further
    /// to try either way.
    bool rescan_due(int64_t now_ms);

    /// @brief True while a rescan is still owed. The one method safe to call without the lock.
    ///
    /// So the main loop can skip taking a lock it has no use for on all but a handful of ticks.
    /// Reading it unlocked costs at worst one tick of staleness in either direction -- a stale
    /// false delays the rescan by one main-loop tick, a stale true buys one uncontended lock --
    /// and rescan_due() decides again under the lock regardless.
    bool pending() const;

    /// @brief Puts both attempts back in hand, for a configure() that really opened a stream.
    void reset();

private:
    /// Hands what is left of the outage to the rescan, if it has not already been spent.
    void escalate_();

    /// rescan_at_ms_ before the first tick after an escalation has stamped a deadline on it.
    static constexpr int64_t NOT_STAMPED = INT64_MIN;

    bool reopen_spent_{false};
    bool rescan_spent_{false};
    /// Read by the main loop without the sink's lock; see pending().
    std::atomic<bool> rescan_owed_{false};
    int64_t rescan_at_ms_{NOT_STAMPED};
};

}  // namespace sendspin_cli
