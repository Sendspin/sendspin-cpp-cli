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

/// @file pulse_sink.h
/// @brief AudioSink over libpulse, with server-reported latency as the sync feedback

#pragma once

#include "audio_sink.h"
#include "pcm_volume.h"
#include "sink_recovery.h"

#include <pulse/pulseaudio.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief How long to wait for the server to answer, in milliseconds.
///
/// Bounds every wait in this backend: the context handshake, a stream connect, and each
/// enumeration query. libpulse's own waits have no timeout at all -- `pa_threaded_mainloop_wait()`
/// returns when a callback signals it and never otherwise -- so a socket that accepts and then
/// says nothing would hang the player at startup with no output. That is exactly the case this
/// backend exists to be honest about, so every wait here is our own condition variable with a
/// deadline instead.
///
/// Three seconds because the only servers reachable through libpulse are on a Unix socket or a
/// LAN: a handshake that has not finished by then is not going to.
inline constexpr int PULSE_TIMEOUT_MS = 3000;

/// @brief How long a *recovery* attempt may wait on the server, in milliseconds.
///
/// Much shorter than the startup budget, because both threads that spend it are on somebody's
/// deadline: poll() runs on the main loop, which also drives the protocol client, mDNS and the
/// control socket -- AudioSink::poll() asks for a bound and three seconds is barely one -- and
/// reopen_in_place_() spends it out of a write()'s own timeout.
///
/// Half a second is generous for a local socket that is actually there. One that is not fails
/// immediately rather than waiting, and one that is merely slow costs nothing here: the next
/// configure() reconnects with the full budget anyway.
inline constexpr int PULSE_RECOVERY_TIMEOUT_MS = 500;

/// @brief A libpulse threaded mainloop and the context running on it, as one lifetime.
///
/// Owned for as long as the sink lives, and taken briefly by probe() and list_devices() -- each
/// of which needs a connected context and nothing else. Every libpulse call on `context()` must
/// hold the mainloop lock (`pa_threaded_mainloop_lock()`); that is libpulse's rule, not ours.
///
/// THREAD SAFETY: connect(), disconnect() and the destructor must run on a thread that is *not*
/// the mainloop's -- `pa_threaded_mainloop_stop()` deadlocks if called from inside it. Every
/// caller is on the main loop, and deliberately so: the sink's constructor and destructor,
/// configure(), and poll(). **write() never reconnects** -- reopen_in_place_() refuses when the
/// context is down and escalates to poll() instead, which is what keeps a wait on the server off
/// the sync task's thread. ready() and notify() are safe from anywhere.
class PulseConnection {
public:
    PulseConnection() = default;
    ~PulseConnection();

    PulseConnection(const PulseConnection&) = delete;
    PulseConnection& operator=(const PulseConnection&) = delete;

    /// @brief Brings up the mainloop and connects a context, waiting for it to become ready.
    ///
    /// Idempotent only in the sense that a failed attempt leaves nothing behind: it tears its own
    /// half-built state down before returning, so the caller may simply try again later.
    /// @param error Set to a human-readable reason when the return value is false.
    /// @param timeout_ms How long to wait for the server to answer. PULSE_RECOVERY_TIMEOUT_MS on
    /// a path that is already on somebody's deadline; the default everywhere else.
    /// @return true once the context is PA_CONTEXT_READY.
    bool connect(std::string& error, int timeout_ms = PULSE_TIMEOUT_MS);

    /// @brief Tears the context and the mainloop down. Idempotent.
    void disconnect();

    /// @brief True while the context is connected and usable.
    bool ready() const;

    /// @brief The server this context reached, for log lines and errors. "(no server)" if none.
    ///
    /// Takes the mainloop lock itself, so it must not be called with that lock already held.
    std::string server_name() const;

    pa_threaded_mainloop* mainloop() const {
        return this->loop_;
    }
    pa_context* context() const {
        return this->context_;
    }

    /// @brief Blocks until `predicate` holds or `timeout_ms` passes. Mainloop lock NOT held.
    ///
    /// The one waiting primitive this backend has. Callbacks run on the mainloop thread and call
    /// notify() when they have changed something a waiter might be watching for.
    ///
    /// The predicate must read nothing but plain values and atomics: it is evaluated under
    /// wait_mutex_, which the mainloop thread takes to notify, so a libpulse call in there would
    /// need the mainloop lock and close a cycle between the two.
    /// @return true if the predicate held before the deadline.
    template <typename Predicate>
    bool wait_for(Predicate predicate, int timeout_ms = PULSE_TIMEOUT_MS) {
        std::unique_lock<std::mutex> lock(this->wait_mutex_);
        return this->wait_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate);
    }

    /// @brief Wakes every waiter. Called from libpulse callbacks, on the mainloop thread.
    void notify();

private:
    static void state_cb(pa_context* context, void* userdata);

    pa_threaded_mainloop* loop_{nullptr};
    pa_context* context_{nullptr};
    /// Written by the context state callback on the mainloop thread, read by waiters.
    std::atomic<pa_context_state_t> state_{PA_CONTEXT_UNCONNECTED};

    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
};

/// @brief An AudioSink that plays through a PulseAudio server, natively.
///
/// The device string is what followed `-o pulse:` -- a sink name as `-l` prints it, or empty for
/// whichever sink the server itself calls default. It is passed to the server verbatim and
/// resolved there, which is why a sink that appears after startup is picked up by the next
/// stream without anything here having to rescan.
///
/// **What this buys over `-o alsa:pulse`**, which reaches the same server through ALSA's plugin
/// PCM and has always worked: the sinks are enumerable, so `-o` can name one; the stream carries
/// an application name, so it appears as `sendspin-cli` in the host's mixer and can be routed
/// per-application; and `pa_stream_get_latency()` answers for the server's real playout rather
/// than for the plugin's own buffering, which is what a synchronised multi-room player needs.
/// The ALSA route stays reachable and documented -- see resolve_device_spec().
///
/// THREAD SAFETY: mutex_ serialises everything, as AlsaAudioSink does and unlike PortAudioSink:
/// libpulse lets any thread write to a stream under the mainloop lock, so there is no audio
/// callback to keep lock-free and no ring to bridge to one. **The server's own buffer is the
/// ring**, sized by --buffer-ms through pa_buffer_attr::tlength, which is also what
/// pa_stream_get_latency() answers about -- so a second buffer here would be latency the sync
/// feedback could not see.
///
/// Lock order is mutex_ then the mainloop lock, never the other way, and no libpulse callback
/// takes mutex_ -- they only notify a condition variable. write() releases mutex_ while waiting
/// for room, exactly as PortAudioSink's does, so configure(), clear() and stop() are not stuck
/// behind a parked writer; stream_generation_ is what makes that safe.
///
/// Volume is applied on the way in, into a scratch buffer, as AlsaAudioSink does -- the bytes
/// handed to pa_stream_write() are the caller's, so scaling them in place is not ours to do. A
/// volume change therefore reaches only audio not yet written, where PortAudio's callback-side
/// scaling also reaches what is already buffered. Both are correct; this one takes effect a
/// buffer later.
///
/// Volume is deliberately *not* pushed to the server as a sink-input volume. Three reasons: the
/// spec's `(volume/100)^1.5` perceived-loudness curve is not PulseAudio's cubic taper; stacking a
/// server gain on the software one would square the taper (ROADMAP item 15 says so for ALSA's
/// mixer, and this is the same trap); and a gain `pavucontrol` can move behind our back would
/// break the guarantee in audio_sink.h that a player never reports a volume the speaker is not
/// at, which group volume is derived from.
class PulseAudioSink final : public AudioSink {
public:
    /// @param device The sink name from `-o pulse:<sink>`, or empty for the server's default.
    /// @param buffer_ms How much audio the server should keep queued, from --buffer-ms. A
    /// request rather than a promise: the server's own minimum wins, and says so at `debug`.
    PulseAudioSink(std::string device, uint32_t buffer_ms);
    ~PulseAudioSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// @brief Spends the delayed half of this sink's reconnect budget, when write()'s failed.
    ///
    /// Does nothing on an ordinary tick and takes no lock to establish that, like
    /// PortAudioSink::poll(). What it does when there is work is a full reconnect --
    /// SinkRecovery's second attempt, which for a sound server is what a *restarted* daemon
    /// needs: the socket is gone for a moment and comes back, so an immediate retry fails and
    /// one SINK_RESCAN_DELAY_MS later succeeds.
    ///
    /// A daemon slower than SINK_RESCAN_DELAY_MS to come back is not chased further; see
    /// SinkRecovery's own note on where that mapping stops fitting, which is where that lives.
    void poll(int64_t now_ms) override;

    /// @brief Everything this player can emit, because the server converts.
    ///
    /// PulseAudio resamples and reformats whatever a stream sends into what the sink runs at, so
    /// "what can I push through this -o value" really is the whole ladder -- the same answer, for
    /// the same reason, that ALSA's `default` plug PCM gives. Asking the sink's own sample spec
    /// would describe the hardware behind the server rather than the question being asked.
    SinkCapabilities capabilities() const override;

    /// @brief Checks that a server is reachable and that it has the named sink.
    ///
    /// Called from make_audio_sink() so an unreachable socket or a mistyped sink fails at
    /// startup, while someone is watching the terminal, rather than at the first track.
    /// @param error Set to a human-readable reason when the return value is false.
    static bool probe(const std::string& device, std::string& error);

    /// @brief Prints this server's sinks, with the default marked. Backs part of -l.
    static void list_devices(std::FILE* out);

private:
    static void stream_state_cb(pa_stream* stream, void* userdata);
    static void stream_write_cb(pa_stream* stream, size_t nbytes, void* userdata);
    static void stream_underflow_cb(pa_stream* stream, void* userdata);

    /// Opens a stream on the configured sink for a format. Caller holds mutex_ and has already
    /// closed any previous stream.
    /// @param timeout_ms How long to wait for the server to accept the stream. See
    /// PULSE_RECOVERY_TIMEOUT_MS.
    bool open_stream_(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample,
                      int timeout_ms);
    /// Disconnects and frees the stream if open, and forgets the format. Caller holds mutex_.
    /// Idempotent, and deliberately leaves stopping_ alone -- see stop().
    void close_stream_();
    /// Makes the one in-place attempt a dead stream gets: reconnect the context if it has
    /// dropped, then reopen at last_format_. Called from write(), so on the sync task's thread.
    /// Caller holds mutex_.
    /// @return true if a stream is running again.
    bool reopen_in_place_();
    /// True while the context and the stream are both usable. Caller holds mutex_.
    bool stream_alive_() const;
    /// Prepares `frames` frames of `data` for the server: scaled by a ramp from `start` toward
    /// `target`, and converted to unsigned where the stream is 8-bit. Caller holds mutex_.
    ///
    /// Deliberately does not advance the ramp -- write() commits that by the frames it really
    /// wrote, which is not necessarily the frames scaled here.
    /// @return scratch_ when anything had to change, or `data` itself when nothing did.
    const uint8_t* stage_(const uint8_t* data, size_t frames, uint64_t start, uint64_t target);
    /// Recomputes target_multiplier_ from volume_ and muted_.
    void update_target_multiplier_();

    /// Held for the sink's whole life. Declared first so it is destroyed last: the stream has to
    /// be freed before the mainloop it runs on goes away.
    PulseConnection conn_;

    /// The sink name as -o spelled it, empty for the server's default.
    std::string device_;
    uint32_t buffer_ms_;

    /// Serialises the stream, the format fields and the volume ramp.
    std::mutex mutex_;
    /// Signalled by the stream's write callback once the server has room again.
    std::condition_variable space_available_;
    pa_stream* stream_{nullptr};
    uint32_t rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    size_t bytes_per_frame_{0};
    /// Per-frame gain increment for this stream's rate, from volume_ramp_step().
    uint64_t ramp_step_{0};
    /// Scaled copy of the caller's PCM, reused across writes so a steady stream allocates once.
    std::vector<uint8_t> scratch_;

    /// Bumped whenever the stream is opened, closed or flushed. write() captures it before
    /// waiting and compares it on waking, because waiting releases mutex_ -- see
    /// PortAudioSink::stream_generation_, which this mirrors for the same reason.
    uint64_t stream_generation_{0};

    /// Set before stop() takes the mutex, so a write() waiting for room bails out promptly
    /// instead of making shutdown wait for the server. Latches: nothing clears it.
    std::atomic<bool> stopping_{false};
    /// Latches when write() first finds no usable stream, purely so it complains once rather
    /// than on every buffer. Cleared by a configure() that succeeds.
    std::atomic<bool> failed_{false};
    /// Set by the stream state callback when the server kills the stream under us. Read by
    /// stream_alive_(), which is what turns a dead stream into a reconnect.
    std::atomic<bool> stream_failed_{false};
    /// The state the server last reported, as a pa_stream_state_t.
    ///
    /// Mirrored into an atomic rather than read back with pa_stream_get_state() because the one
    /// caller is open_stream_()'s wait predicate, which runs under the connection's own wait
    /// mutex: a libpulse call there would have to take the mainloop lock, and the mainloop thread
    /// takes that wait mutex to notify. Reading a plain value instead is what keeps the two lock
    /// orders from meeting.
    std::atomic<int> stream_state_{PA_STREAM_UNCONNECTED};

    /// The format the sink was last *asked* to play, which outlives the stream playing it, so
    /// recovery has something to reopen at. Guarded by mutex_; zeroed by stop().
    StreamFormat last_format_{};
    /// What is left to try about a stream that has died, and when. Guarded by mutex_, bar
    /// SinkRecovery::pending().
    SinkRecovery recovery_;

    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
    /// The Q32 gain the ramp is heading for. Atomic because set_volume()/set_muted() write it
    /// from the main loop and write() reads it from the sync task's thread.
    std::atomic<uint64_t> target_multiplier_{Q32_ONE};
    /// The Q32 gain actually being applied. Plain: only write() and clear() touch it, and both
    /// hold mutex_ -- the same rule AlsaAudioSink's copy follows.
    uint64_t current_multiplier_{Q32_ONE};
};

}  // namespace sendspin_cli
