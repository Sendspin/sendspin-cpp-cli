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

/// @file pipewire_sink.h
/// @brief AudioSink over libpipewire, with graph-reported playout delay as the sync feedback

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

/// @brief How long to wait for the PipeWire daemon to answer, in seconds.
///
/// Bounds the registry walk, through pw_thread_loop_timed_wait(). Three seconds because the
/// daemon is on a local socket: a handshake that has not finished by then is not going to.
inline constexpr int PIPEWIRE_TIMEOUT_S = 3;

/// @brief How long to wait for the graph to accept a stream, in milliseconds.
///
/// The same figure as PIPEWIRE_TIMEOUT_S, in the unit the stream wait needs: connecting a stream
/// is a round trip like the registry walk, and gets the same budget.
inline constexpr int PIPEWIRE_STREAM_TIMEOUT_MS = PIPEWIRE_TIMEOUT_S * 1000;

/// @brief How long a *recovery* attempt may wait on the graph, in milliseconds.
///
/// Much shorter than the budget above, because both threads that spend it are on somebody's
/// deadline: poll() runs on the main loop, which also drives the protocol client, mDNS and the
/// control socket -- AudioSink::poll() asks for a bound and three seconds is barely one -- and
/// reopen_in_place_() spends it out of a write()'s own timeout.
///
/// Half a second is generous for a daemon that is actually there. One that is not fails without
/// waiting, and one that is merely slow costs nothing here: the next configure() reconnects with
/// the full budget anyway.
inline constexpr int PIPEWIRE_RECOVERY_TIMEOUT_MS = 500;

/// @brief Floor on the ring, as a multiple of the graph's quantum.
///
/// process() asks for a whole quantum at a time, so a ring no bigger than that starves on every
/// cycle however promptly write() refills it. The same figure, for the same reason, as
/// PortAudioSink's device-latency floor.
inline constexpr size_t RING_QUANTUM_MULTIPLE = 3;

/// @brief Absolute floor on the ring, for a stream whose quantum is not known yet.
inline constexpr size_t MIN_RING_FRAMES = 1024;

/// @brief Frames the ring holds for `buffer_ms` at `rate`, once MIN_RING_FRAMES has had its say.
size_t pipewire_ring_frames(uint32_t rate, uint32_t buffer_ms);

/// @brief What a ring of `ring_frames` is worth to a graph running `quantum`-frame cycles.
struct PipeWireQuantumFit {
    /// The ring cannot hold one whole quantum, so every process() short-reads and zero-fills the
    /// remainder however promptly write() refills it. Not tight -- broken.
    bool starves{false};
    /// Holds a quantum but fewer than RING_QUANTUM_MULTIPLE of them, so a busy graph can outrun
    /// the writer.
    bool tight{false};
    /// The --buffer-ms that would clear RING_QUANTUM_MULTIPLE quanta at this rate. Zero when
    /// there is no quantum or rate to derive it from.
    uint32_t recommended_buffer_ms{0};
};

/// @brief Measures the ring against the quantum the graph turned out to be running.
///
/// Split out and pure because this arithmetic is what decides whether a host plays or crackles,
/// and it is worth testing without a graph to ask. `quantum` or `rate` of zero means nothing has
/// been observed yet, and reports neither fault.
PipeWireQuantumFit pipewire_quantum_fit(size_t ring_frames, uint32_t quantum, uint32_t rate);

/// @brief Holds one pw_init()/pw_deinit() pair for as long as it lives.
///
/// libpipewire reference-counts the pair, so a scoped guard around a one-off registry walk
/// composes with the one a live sink holds -- the same arrangement PortAudioGuard makes, and for
/// the same reason: it is what lets probe() and list_devices() be static and self-contained.
///
/// THREAD SAFETY: pw_init()/pw_deinit() are not thread-safe, so every guard must be constructed
/// and destroyed on the main loop thread. It is -- probe(), list_devices() and the sink's own
/// constructor and destructor all run there, per the AudioSink threading contract.
class PipeWireGuard {
public:
    PipeWireGuard();
    ~PipeWireGuard();

    PipeWireGuard(const PipeWireGuard&) = delete;
    PipeWireGuard& operator=(const PipeWireGuard&) = delete;
};

/// @brief An AudioSink that plays through a PipeWire graph, natively.
///
/// The device string is what followed `-o pipewire:` -- a node name as `-l` prints it, or empty
/// for wherever the graph's own default routing sends a Playback/Music stream. It becomes the
/// stream's `target.object` property, so the daemon resolves it and a node that appears after
/// startup is picked up by the next stream without anything here having to rescan.
///
/// **Why this exists beside the PulseAudio backend**, which reaches a PipeWire host perfectly
/// well through `pipewire-pulse`: nothing but a native client can select a *node* -- pipewire-pulse
/// presents sinks, which is a compatibility view of the graph rather than the graph -- and nothing
/// but a native client is free of that compatibility layer in the audio path at all. It reaches no
/// host libpulse cannot; what it buys is on the far side of the socket, not a new set of hosts.
///
/// THREAD SAFETY: this backend *pulls*, like PortAudio and unlike PulseAudio: the graph runs
/// process() on its own realtime data thread and expects a buffer filled there and then. So it
/// buffers the way PortAudioSink does -- mutex_ guards the stream and the ring's producer side,
/// and process() takes no lock, reading the ring lock-free and the format fields as plain values.
/// Those unsynchronised reads are legal only because of one ordering invariant: **everything
/// process() reads is mutated only while the stream is disconnected**, and pw_stream_disconnect()
/// does not return while the data thread is still in a callback.
///
/// loop_ is the one field a callback reads that the invariant above does not cover, because
/// state_changed() runs on the *loop* thread rather than the data thread: it is safe because that
/// thread cannot exist before start_loop_() has published the pointer, and pw_thread_loop_destroy()
/// joins it before stop_loop_() clears it -- so loop_ is fixed for the whole life of any callback
/// that could read it.
///
/// Lock order is mutex_ then the thread-loop lock, never the other way, and no PipeWire callback
/// takes mutex_. write() releases mutex_ while waiting for ring space, exactly as PortAudioSink's
/// does, so configure(), clear() and stop() are not stuck behind a parked writer;
/// stream_generation_ is what makes that safe.
///
/// Volume is applied on the way into the ring, into a scratch buffer, as AlsaAudioSink does --
/// which keeps the realtime callback down to a memcpy, and is why the ramp is committed by the
/// frames really pushed rather than the frames scaled. A volume change therefore reaches only
/// audio not yet queued, where PortAudio's callback-side scaling also reaches what is already
/// buffered. Both are correct; this one takes effect a buffer later.
///
/// Volume is deliberately *not* set on the PipeWire node. The reasons are the ones spelled out in
/// pulse_sink.h: the spec's `(volume/100)^1.5` curve is ours, stacking a graph gain on the
/// software one would square the taper, and a gain the host's mixer can move behind our back
/// would break the guarantee in audio_sink.h that a player never reports a volume the speaker is
/// not at.
class PipeWireSink final : public AudioSink {
public:
    /// @param device The node name from `-o pipewire:<node>`, or empty for the graph's default.
    /// @param buffer_ms How much audio to keep in the ring, from --buffer-ms. A request rather
    /// than a promise: the graph's quantum is the floor, and says so at `debug`.
    PipeWireSink(std::string device, uint32_t buffer_ms);
    ~PipeWireSink() override;

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
    /// needs: the socket is gone for a moment and comes back, so an immediate retry fails and one
    /// SINK_RESCAN_DELAY_MS later succeeds.
    ///
    /// Up to SINK_RESCAN_ATTEMPTS of them per configured stream, behind a delay that doubles --
    /// so a daemon that takes most of a minute to come back is still caught, and one that takes
    /// longer is left to the next configure(), which reconnects anyway.
    void poll(int64_t now_ms) override;

    /// @brief Everything this player can emit, because the graph converts.
    ///
    /// PipeWire puts an adapter in front of every stream and resamples and reformats into whatever
    /// the node runs at, so "what can I push through this -o value" really is the whole ladder --
    /// the same answer, for the same reason, that ALSA's `default` plug PCM gives.
    SinkCapabilities capabilities() const override;

    /// @brief Checks that a daemon is reachable and that it has the named node.
    ///
    /// Called from make_audio_sink() so an absent daemon or a mistyped node fails at startup,
    /// while someone is watching the terminal, rather than at the first track.
    /// @param error Set to a human-readable reason when the return value is false.
    static bool probe(const std::string& device, std::string& error);

    /// @brief Prints this graph's audio sink nodes. Backs part of -l.
    static void list_devices(std::FILE* out);

private:
    static void stream_state_cb(void* userdata, enum pw_stream_state old,
                                enum pw_stream_state state, const char* error);
    static void stream_process_cb(void* userdata);

    /// Connects a stream for a format. Caller holds mutex_ and has already closed any previous
    /// stream.
    /// @param timeout_ms How long to wait for the graph to accept it. See
    /// PIPEWIRE_RECOVERY_TIMEOUT_MS.
    bool open_stream_(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample,
                      int timeout_ms);
    /// Disconnects and destroys the stream if open, and forgets the format. Caller holds mutex_.
    /// Idempotent, and deliberately leaves stopping_ alone -- see stop().
    void close_stream_();
    /// Brings up the thread loop this sink's stream runs on. Caller holds mutex_.
    bool start_loop_();
    /// Stops and frees the thread loop, after any stream on it has gone. Caller holds mutex_.
    void stop_loop_();
    /// Makes the one in-place attempt a dead stream gets: reconnect at last_format_. Called from
    /// write(), so on the sync task's thread. Caller holds mutex_.
    /// @return true if a stream is running again.
    bool reopen_in_place_();
    /// True while the stream is connected and the graph is still driving it. Caller holds mutex_.
    bool stream_alive_() const;
    /// Ring size in bytes for the open stream's format. Caller holds mutex_, and the format
    /// fields are already set.
    size_t ring_capacity_() const;
    /// Prepares `frames` frames of `data` for the ring: scaled by a ramp from `start` toward
    /// `target`. Caller holds mutex_.
    ///
    /// Deliberately does not advance the ramp -- write() commits that by the frames it really
    /// pushed, which is not necessarily the frames scaled here.
    /// @return scratch_ when anything had to be scaled, or `data` itself when nothing did.
    const uint8_t* stage_(const uint8_t* data, size_t frames, uint64_t start, uint64_t target);
    /// Recomputes target_multiplier_ from volume_ and muted_.
    void update_target_multiplier_();

    /// Held for the sink's whole life, and declared first so it is destroyed last: the stream and
    /// its loop have to be gone before libpipewire is deinitialised under them.
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
    /// Read by the process callback. See the ordering invariant in the class docstring.
    size_t bytes_per_frame_{0};
    /// Per-frame gain increment for this stream's rate, from volume_ramp_step().
    uint64_t ramp_step_{0};
    /// Scaled copy of the caller's PCM, reused across writes so a steady stream allocates once.
    std::vector<uint8_t> scratch_;
    /// The quantum the graph last asked for, in frames. Written by the process callback and read
    /// only for the one debug line that reports it, so a stale value costs nothing.
    std::atomic<uint32_t> quantum_frames_{0};
    /// Latches once the quantum has been reported, so the line is printed per stream, not per
    /// graph cycle.
    std::atomic<bool> quantum_logged_{false};

    /// Bumped whenever the stream is opened, closed or flushed. write() captures it before
    /// waiting and compares it on waking, because waiting releases mutex_.
    uint64_t stream_generation_{0};

    /// Set before stop() takes the mutex, so a write() blocked on a full ring bails out promptly
    /// instead of making shutdown wait for the graph. Latches: nothing clears it.
    std::atomic<bool> stopping_{false};
    /// Latches when write() first finds no usable stream, purely so it complains once rather than
    /// on every buffer. Cleared by a configure() that succeeds.
    std::atomic<bool> failed_{false};
    /// Set by the stream state callback when the graph puts the stream in error, or unlinks it.
    /// Read by stream_alive_(), which is what turns a dead stream into a reconnect.
    std::atomic<bool> stream_failed_{false};
    /// The state the graph last reported, for the state wait in open_stream_().
    std::atomic<int> stream_state_{PW_STREAM_STATE_UNCONNECTED};

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
