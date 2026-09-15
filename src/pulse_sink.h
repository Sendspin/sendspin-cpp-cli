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

/// AudioSink over libpulse, with server-reported latency as the sync feedback.

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

/// Bound on every wait for the server; libpulse's own waits have no timeout.
inline constexpr int PULSE_TIMEOUT_MS = 3000;

/// Shorter bound for recovery waits, which run on the main loop or inside a write().
inline constexpr int PULSE_RECOVERY_TIMEOUT_MS = 500;

/// A libpulse threaded mainloop and its context, as one lifetime.
/// connect(), disconnect() and the destructor deadlock if called on the mainloop thread.
class PulseConnection {
public:
    PulseConnection() = default;
    ~PulseConnection();

    PulseConnection(const PulseConnection&) = delete;
    PulseConnection& operator=(const PulseConnection&) = delete;

    /// Starts the mainloop and connects a context; a failed attempt leaves nothing behind.
    /// @param timeout_ms PULSE_RECOVERY_TIMEOUT_MS on paths already on a deadline.
    /// @return true once the context is PA_CONTEXT_READY.
    bool connect(std::string& error, int timeout_ms = PULSE_TIMEOUT_MS);

    /// Tears the context and the mainloop down. Idempotent.
    void disconnect();

    /// True while the context is connected and usable.
    bool ready() const;

    /// The server this context reached, or "(no server)".
    /// Takes the mainloop lock itself: do not call with it held.
    std::string server_name() const;

    pa_threaded_mainloop* mainloop() const {
        return this->loop_;
    }
    pa_context* context() const {
        return this->context_;
    }

    /// Blocks until `predicate` holds or `timeout_ms` passes, without the mainloop lock.
    /// The predicate may read only plain values and atomics: no libpulse calls.
    /// @return true if the predicate held before the deadline.
    template <typename Predicate>
    bool wait_for(Predicate predicate, int timeout_ms = PULSE_TIMEOUT_MS) {
        std::unique_lock<std::mutex> lock(this->wait_mutex_);
        return this->wait_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), predicate);
    }

    /// Wakes every waiter. Called from libpulse callbacks, on the mainloop thread.
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

/// An AudioSink that plays through a PulseAudio server; `-o pulse:` a sink name or empty.
/// Lock order is mutex_ then the mainloop lock; no libpulse callback takes mutex_.
class PulseAudioSink final : public AudioSink {
public:
    /// @param device Sink name, or empty for the server's default.
    /// @param buffer_ms Requested server buffer; the server's minimum wins.
    PulseAudioSink(std::string device, uint32_t buffer_ms);
    ~PulseAudioSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// Makes the delayed reconnect once write()'s in-place attempt has failed.
    void poll(int64_t now_ms) override;

    /// The whole ladder: the server converts whatever the stream sends.
    SinkCapabilities capabilities() const override;

    /// Checks that a server is reachable and has the named sink.
    static bool probe(const std::string& device, std::string& error);

    /// Prints this server's sinks, with the default marked.
    static void list_devices(std::FILE* out);

private:
    static void stream_state_cb(pa_stream* stream, void* userdata);
    static void stream_write_cb(pa_stream* stream, size_t nbytes, void* userdata);
    static void stream_underflow_cb(pa_stream* stream, void* userdata);

    /// Opens a stream for a format. Caller holds mutex_ and has closed any previous stream.
    /// @param timeout_ms How long to wait for the server to accept the stream.
    bool open_stream_(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample,
                      int timeout_ms);
    /// Disconnects and frees the stream and forgets the format. Caller holds mutex_.
    /// Must not touch stopping_; see stop().
    void close_stream_();
    /// The one in-place attempt a dead stream gets, from write(). Caller holds mutex_.
    /// @return true if a stream is running again.
    bool reopen_in_place_();
    /// Logs a failed recovery attempt and what happens next. Caller holds mutex_.
    void report_failed_recovery_(const std::string& reason);
    /// True while the context and the stream are both usable. Caller holds mutex_.
    bool stream_alive_() const;
    /// Scales `frames` of `data` along a ramp and converts 8-bit; does not advance the ramp.
    /// @return scratch_ when anything changed, else `data`. Caller holds mutex_.
    const uint8_t* stage_(const uint8_t* data, size_t frames, uint64_t start, uint64_t target);
    void update_target_multiplier_();

    /// Declared first so it is destroyed last, after the stream is freed.
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
    /// Scaled copy of the caller's PCM, reused across writes.
    std::vector<uint8_t> scratch_;

    /// Bumped on open, close or flush; a write() that waited compares it on waking.
    uint64_t stream_generation_{0};

    /// Set before stop() takes the mutex so a waiting write() bails out; latches.
    std::atomic<bool> stopping_{false};
    /// Latches so write() complains once about a missing stream; cleared by a good configure().
    std::atomic<bool> failed_{false};
    /// Set when the server kills the stream; stream_alive_() turns it into a reconnect.
    std::atomic<bool> stream_failed_{false};
    /// Mirrored from the state callback so wait predicates need no libpulse call.
    std::atomic<int> stream_state_{PA_STREAM_UNCONNECTED};

    /// Format last asked for; recovery reopens at it. Guarded by mutex_, zeroed by stop().
    StreamFormat last_format_{};
    /// Guarded by mutex_, except SinkRecovery::pending().
    SinkRecovery recovery_;

    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
    /// Q32 gain the ramp is heading for; written by the main loop, read by write().
    std::atomic<uint64_t> target_multiplier_{Q32_ONE};
    /// Q32 gain being applied; only touched under mutex_.
    uint64_t current_multiplier_{Q32_ONE};
};

}  // namespace sendspin_cli
