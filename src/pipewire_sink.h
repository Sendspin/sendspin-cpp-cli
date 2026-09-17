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

/// AudioSink over libpipewire, with graph-reported playout delay as the sync feedback.

#pragma once

#include "audio_sink.h"
#include "pcm_ring.h"
#include "pcm_volume.h"
#include "sink_recovery.h"

#include <pipewire/pipewire.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

/// Bound on the registry walk, in seconds.
inline constexpr int PIPEWIRE_TIMEOUT_S = 3;

/// Bound on waiting for the graph to accept a stream.
inline constexpr int PIPEWIRE_STREAM_TIMEOUT_MS = PIPEWIRE_TIMEOUT_S * 1000;

/// Shorter bound for recovery waits, which run on the main loop or inside a write().
inline constexpr int PIPEWIRE_RECOVERY_TIMEOUT_MS = 500;

/// Floor on the ring, in quanta: process() takes a whole quantum per cycle.
inline constexpr size_t RING_QUANTUM_MULTIPLE = 3;

/// Absolute floor on the ring, for a stream whose quantum is not known yet.
inline constexpr size_t MIN_RING_FRAMES = 1024;

/// Frames the ring holds for `buffer_ms` at `rate`, at least MIN_RING_FRAMES.
size_t pipewire_ring_frames(uint32_t rate, uint32_t buffer_ms);

/// What a ring of `ring_frames` is worth to a graph running `quantum`-frame cycles.
struct PipeWireQuantumFit {
    /// The ring cannot hold one quantum, so every cycle zero-fills.
    bool starves{false};
    /// Fewer than RING_QUANTUM_MULTIPLE quanta, so a busy graph can outrun the writer.
    bool tight{false};
    /// --buffer-ms that would hold RING_QUANTUM_MULTIPLE quanta; 0 when unknown.
    uint32_t recommended_buffer_ms{0};
};

/// Measures the ring against the graph's quantum; a zero `quantum` or `rate` reports no fault.
PipeWireQuantumFit pipewire_quantum_fit(size_t ring_frames, uint32_t quantum, uint32_t rate);

/// Holds one reference-counted pw_init()/pw_deinit() pair.
/// Construct and destroy on the main loop thread only.
class PipeWireGuard {
public:
    PipeWireGuard();
    ~PipeWireGuard();

    PipeWireGuard(const PipeWireGuard&) = delete;
    PipeWireGuard& operator=(const PipeWireGuard&) = delete;
};

/// An AudioSink that plays through a PipeWire graph; `-o pipewire:` a node name or empty.
/// process() reads the ring and format fields unlocked: change them only while disconnected.
/// Lock order is mutex_ then the thread-loop lock; no PipeWire callback takes mutex_.
class PipeWireSink final : public AudioSink {
public:
    /// @param device Node name, or empty for the graph's default.
    /// @param buffer_ms Requested ring size; the graph's quantum is the floor.
    PipeWireSink(std::string device, uint32_t buffer_ms);
    ~PipeWireSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// Logs the graph's quantum once per stream and makes the delayed reconnect when one is due.
    void poll(int64_t now_ms) override;

    /// The whole ladder: the graph converts whatever the stream sends.
    SinkCapabilities capabilities() const override;

    /// Checks that a daemon is reachable and has the named node.
    static bool probe(const std::string& device, std::string& error);

    /// Prints this graph's audio sink nodes.
    static void list_devices(std::FILE* out);

private:
    static void stream_state_cb(void* userdata, enum pw_stream_state old,
                                enum pw_stream_state state, const char* error);
    static void stream_process_cb(void* userdata);

    /// Connects a stream for a format. Caller holds mutex_ and has closed any previous stream.
    /// @param timeout_ms How long to wait for the graph to accept it.
    bool open_stream_(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample,
                      int timeout_ms);
    /// Disconnects and destroys the stream and forgets the format. Caller holds mutex_.
    /// Must not touch stopping_; see stop().
    void close_stream_();
    /// Brings up the thread loop this sink's stream runs on. Caller holds mutex_.
    bool start_loop_();
    /// Stops and frees the thread loop, after any stream on it has gone. Caller holds mutex_.
    void stop_loop_();
    /// The one in-place reconnect a dead stream gets, from write(). Caller holds mutex_.
    /// @return true if a stream is running again.
    bool reopen_in_place_();
    /// Adds the frames still in the ring to the outage gap: the player has counted them, and the
    /// lost stream recovery is about to close will never report them. Only for those closes --
    /// stop(), configure() and clear() end the stream the gap belonged to. Caller holds mutex_.
    void discard_ring_tail_();
    /// True while the stream is connected and the graph is still driving it. Caller holds mutex_.
    bool stream_alive_() const;
    /// Ring size in bytes. Caller holds mutex_ and the format fields are set.
    size_t ring_capacity_() const;
    /// Scales `frames` of `data` along a ramp, without advancing it. Caller holds mutex_.
    /// @return scratch_ when anything was scaled, else `data`.
    const uint8_t* stage_(const uint8_t* data, size_t frames, uint64_t start, uint64_t target);
    void update_target_multiplier_();

    /// Declared first so it is destroyed last, after the stream and its loop.
    PipeWireGuard pw_;

    /// The node as -o spelled it, empty for the graph's default.
    std::string device_;
    /// Ring size to aim for, in milliseconds, before the floors in ring_capacity_() apply.
    uint32_t buffer_ms_;

    /// Serialises the stream, the format fields and the ring's producer side.
    std::mutex mutex_;
    /// Signalled by the process callback once it has drained a quantum's worth of the ring.
    std::condition_variable space_available_;
    PcmRingBuffer ring_;

    pw_thread_loop* loop_{nullptr};
    pw_stream* stream_{nullptr};

    uint32_t rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    /// Read by process() unlocked; see the class comment.
    size_t bytes_per_frame_{0};
    /// Per-frame gain increment for this stream's rate, from volume_ramp_step().
    uint64_t ramp_step_{0};
    /// Scaled copy of the caller's PCM, reused across writes.
    std::vector<uint8_t> scratch_;
    /// The quantum the graph last asked for, in frames; written by process().
    std::atomic<uint32_t> quantum_frames_{0};
    /// Latches once the quantum is logged for this stream.
    std::atomic<bool> quantum_logged_{false};

    /// Bumped on open, close or flush; a write() that waited compares it on waking.
    uint64_t stream_generation_{0};

    /// Set before stop() takes the mutex so a blocked write() bails out; latches.
    std::atomic<bool> stopping_{false};
    /// Latches so write() complains once about a missing stream; cleared by a good configure().
    std::atomic<bool> failed_{false};
    /// Set when the graph errors or unlinks the stream; read by stream_alive_().
    std::atomic<bool> stream_failed_{false};
    /// The state the graph last reported, for the state wait in open_stream_().
    std::atomic<int> stream_state_{PW_STREAM_STATE_UNCONNECTED};

    /// Format last asked for; recovery reopens at it. Guarded by mutex_, zeroed by stop().
    StreamFormat last_format_{};
    /// Guarded by mutex_, except SinkRecovery::pending().
    SinkRecovery recovery_;
    /// The outage gap once write() has handed it on, for the process callback to retire lock-free
    /// with its next timed report. recovery_ cannot be read there; see OutageGapHandoff.
    OutageGapHandoff gap_handoff_;

    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
    /// Q32 gain the ramp is heading for; written by the main loop, read by write().
    std::atomic<uint64_t> target_multiplier_{Q32_ONE};
    /// Q32 gain being applied; only touched under mutex_.
    uint64_t current_multiplier_{Q32_ONE};
};

}  // namespace sendspin_cli
