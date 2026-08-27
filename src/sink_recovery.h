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

/// @brief How long after an outage escalates the second attempt is first worth making.
///
/// Not politeness. The case it exists for -- a USB DAC pulled out and pushed back in, a sound
/// server being restarted -- takes a moment to come back. Making the attempt the instant the
/// in-place reopen failed would usually make it before the device is there. Two seconds clears a
/// USB enumeration comfortably and is still well inside a track.
///
/// It is also not free to the caller: rebuilding a device list means enumerating every host API,
/// and reconnecting to a sound server means waiting on that server. Either blocks whichever
/// thread asks. See PortAudioSink::poll(), which pays the first on the main loop.
///
/// **This is what bounds the second attempt across streams, and the per-stream budget below is
/// not.** reset() refills that budget at every stream, and how often streams start is the
/// server's choice, not ours -- so on hardware that opens and dies repeatedly the budget alone
/// would allow one cycle per stream, however fast they came. Because every cycle needs a fresh
/// escalation and each escalation must then wait this long, cycles are floored this far apart
/// whatever the server does. Both halves of the bound are load-bearing; neither is decoration.
inline constexpr int64_t SINK_RESCAN_DELAY_MS = 2000;

/// @brief The ceiling the delay between retried attempts grows to, in milliseconds.
///
/// Only a *retried* attempt waits longer than SINK_RESCAN_DELAY_MS -- see rescan_done(). The
/// delay doubles per attempt and stops here, which is what keeps the cost to the caller's thread
/// falling as an outage lengthens: a reconnect that has to wait on an unresponsive socket costs
/// the same each time, so the only way to stop paying it at a fixed duty cycle is to ask less
/// often. Thirty seconds is still inside most tracks.
inline constexpr int64_t SINK_RESCAN_MAX_DELAY_MS = 30000;

/// @brief How many times the second attempt may be made for one configured stream.
///
/// With the doubling above, five attempts span 2 + 4 + 8 + 16 + 30 seconds -- a minute of cover,
/// which is more than a sound server takes to restart and more than a device takes to be plugged
/// back in. Past that the sink discards until the next configure(), which re-resolves the device
/// and reconnects anyway.
///
/// A cap rather than an open-ended retry, deliberately: the delay keeps the *rate* down, and this
/// keeps the total down. An outage that outlives both was never going to be recovered by asking
/// the same question again.
inline constexpr int SINK_RESCAN_ATTEMPTS = 5;

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
///     One per configured stream, whatever it reports.
///  2. **Rescan** the backend's device list, which only the main loop can do, and only
///     `SINK_RESCAN_DELAY_MS` after the reopen gave up. This is what a *replugged* device needs:
///     PortAudio enumerates at `Pa_Initialize()` and never revisits that list, so the device that
///     came back is one the list has never seen. Up to `SINK_RESCAN_ATTEMPTS` per configured
///     stream, behind a doubling delay -- but only for a caller that reports back; see
///     `rescan_done()`.
///
/// "Rescan" is PortAudio's spelling of the second attempt rather than the whole of what it means.
/// The sound-server backends map it onto reconnecting to the server, which is the same shape --
/// too expensive for the sync task's thread, and pointless until the delay is up, because what it
/// exists for is a *restarted* daemon whose socket is gone for a moment and then back.
///
/// The two are not otherwise interchangeable, which is what `rescan_done()` exists for: a
/// rebuilt device list leaves nothing further to try whatever it found, where a reconnect that
/// failed leaves everything to try, because a daemon still down now may be up in a few seconds.
/// So the second attempt is retired by what the caller reports rather than by being handed out,
/// and a caller that reports nothing keeps the one-shot behaviour PortAudio has always had.
///
/// **The budget is per configured stream, not per outage**, which is what stops a half-present
/// device -- a dock mid-handshake, a hub browning out -- from having `write()` call
/// `Pa_OpenStream()` on the sync task's thread fifty times a second. A stream that reopens and
/// dies again therefore goes straight to the second attempt rather than reopening twice, and once
/// the budget is gone the sink discards until the next `configure()` -- which re-resolves the
/// device anyway, and against a list a rescan has already refreshed. `reset()` refills it, and
/// only a `configure()` that really got a stream running calls it.
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
    ///
    /// Exactly once per reopen_due() that returned true, and unlike rescan_done() this does not
    /// check: a second call would escalate again and arm a second attempt that was never earned.
    /// Every caller pairs them inline, which is what keeps that a rule rather than a guard.
    void reopen_done(bool recovered);

    /// @brief Asks whether to make the second attempt. For the main loop's tick.
    ///
    /// What the attempt *is* belongs to the backend: PortAudio rebuilds its device list, and the
    /// sound-server sinks reconnect to their server. Both are too expensive for the sync task's
    /// thread and both are pointless until the delay is up, which is the whole of what this
    /// decides.
    /// @param now_ms Monotonic milliseconds. The delay is measured from the first call after the
    /// escalation -- or, for a retry, after the report that asked for it -- because neither
    /// happens on a thread with any business reading this clock. One tick of slack either way.
    /// @return true at most SINK_RESCAN_ATTEMPTS times per configured stream, never before the
    /// delay is up, and never twice without a `rescan_done(false)` in between. **A caller that
    /// reports nothing gets exactly one**, which is what keeps PortAudio's behaviour what it was.
    bool rescan_due(int64_t now_ms);

    /// @brief Records the outcome of the attempt that rescan_due() asked for.
    ///
    /// Whether an attempt is worth repeating is the *backend's* to say, which is the whole reason
    /// this exists: see the class docstring for why a device-list rebuild and a server reconnect
    /// answer that differently. A backend whose attempt is repeatable reports what really
    /// happened; one whose attempt is one-shot reports `true` whatever the outcome, which is what
    /// PortAudioSink does and why its budget is untouched by any of this.
    /// Reporting on an attempt that is not outstanding does nothing, so a caller may not be made
    /// to double-count by calling twice, and a `reset()` that lands between the two cannot be
    /// undone by the report that follows it.
    /// @param recovered true if the sink is playing again. Retires the budget either way once
    /// SINK_RESCAN_ATTEMPTS have been made.
    void rescan_done(bool recovered);

    /// @brief True while a rescan is still owed. The one method safe to call without the lock.
    ///
    /// So the main loop can skip taking a lock it has no use for on all but a handful of ticks.
    /// Reading it unlocked costs at worst one tick of staleness in either direction -- a stale
    /// false delays the rescan by one main-loop tick, a stale true buys one uncontended lock --
    /// and rescan_due() decides again under the lock regardless.
    ///
    /// Read *under* the lock straight after rescan_done(), it answers a second question exactly:
    /// whether another attempt is coming. Both sound-server sinks use it that way to decide how
    /// loudly to report a failed one, which is why "still owed" is worded as a state rather than
    /// as an errand for the main loop.
    bool pending() const;

    /// @brief Puts both attempts back in hand, for a configure() that really opened a stream.
    void reset();

private:
    /// Hands what is left of the outage to the rescan, if it has not already been spent.
    void escalate_();

    /// rescan_at_ms_ before the first tick after an escalation has stamped a deadline on it.
    static constexpr int64_t NOT_STAMPED = INT64_MIN;

    /// How long to wait before the attempt after `attempts_made` of them, doubling from
    /// SINK_RESCAN_DELAY_MS and stopping at SINK_RESCAN_MAX_DELAY_MS.
    static int64_t delay_for_(int attempts_made);

    bool reopen_spent_{false};
    /// Latches when the second attempt is retired -- recovered, or out of attempts.
    bool rescan_spent_{false};
    /// True between an attempt being handed out and rescan_done() reporting on it.
    ///
    /// Two things turn on it, and the sharper one is the delay. `rescan_at_ms_` still holds the
    /// deadline the in-flight attempt was released on, which is now in the past -- so a
    /// `write()` that re-armed the attempt from here would have the very next tick fire the next
    /// one with no delay at all, defeating the doubling at exactly the moment it earns its keep.
    /// The other is the budget: an attempt nobody has reported on must not be counted twice.
    ///
    /// It is also what makes "a caller that reports nothing gets exactly one" true rather than
    /// nearly true -- such a caller stays here for good, which is exactly the one-shot behaviour
    /// PortAudioSink had before rescan_done() existed.
    bool rescan_in_flight_{false};
    /// How many second attempts have been handed out for this configured stream.
    int rescan_attempts_{0};
    /// Read by the main loop without the sink's lock; see pending().
    std::atomic<bool> rescan_owed_{false};
    int64_t rescan_at_ms_{NOT_STAMPED};
};

}  // namespace sendspin_cli
