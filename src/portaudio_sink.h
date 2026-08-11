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

/// @file portaudio_sink.h
/// @brief AudioSink over PortAudio, with callback DAC-time sync feedback

#pragma once

#include "audio_sink.h"
#include "pcm_volume.h"
#include "sink_recovery.h"

#include <portaudio.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief Holds one Pa_Initialize()/Pa_Terminate() pair for as long as it lives.
///
/// PortAudio reference-counts the pair, so a scoped guard around a one-off query composes
/// with the one a live sink holds: whichever guard is destroyed last is the one that really
/// terminates. That is what lets probe() and list_devices() be static and self-contained.
///
/// THREAD SAFETY: neither Pa_Initialize() nor Pa_Terminate() is thread-safe, so every guard
/// must be constructed and destroyed on the main loop thread. It is -- probe(),
/// list_devices() and the sink's own constructor and destructor all run there, per the
/// AudioSink threading contract.
class PortAudioGuard {
public:
    PortAudioGuard();
    ~PortAudioGuard();

    PortAudioGuard(const PortAudioGuard&) = delete;
    PortAudioGuard& operator=(const PortAudioGuard&) = delete;

    /// @brief True if PortAudio came up. When false, no other Pa_* call will work.
    bool ok() const;

    /// @brief Terminates PortAudio and initializes it again, so its device list is taken afresh.
    ///
    /// PortAudio enumerates devices at Pa_Initialize() and never revisits that list, so a device
    /// plugged in since is unreachable until this runs. The cost is that **every PaDeviceIndex in
    /// the process is invalidated**, and that any open stream must be closed first -- calling
    /// Pa_Terminate() with one live is undefined. It is also slow: rebuilding the list walks every
    /// host API, which can block the caller for the better part of a second.
    ///
    /// The refcounting the class docstring describes cuts both ways here: this only really
    /// terminates because the sink's guard is the sole live one once the player is running --
    /// probe() and list_devices() hold theirs only on the startup path. Anything that later
    /// reaches those at runtime would turn this into a silent no-op, and the rescan with it.
    ///
    /// Main loop only, like the constructor and for the same reason: neither call is thread-safe.
    /// @return true if PortAudio came back up. ok() and error() answer for the new state either
    /// way, so a failure leaves the guard honest and the destructor correct.
    bool reinitialize();

    /// @brief Why initialization failed. Only meaningful when ok() is false.
    const char* error() const;

private:
    PaError err_;
};

/// @brief Lock-free single-producer/single-consumer byte ring buffer.
///
/// Bridges AudioSink::write()'s push model to PortAudio's pull callback: the sync task
/// writes, the audio callback reads, and neither waits on the other. The ring itself needs no
/// lock and allocates nothing; what the callback does around it -- notifying the producer's
/// condition variable, and invoking on_frames_played -- is not strictly realtime-safe, and is
/// the same pragmatic trade upstream's reference makes.
///
/// The producer owns write_pos_ and the consumer owns read_pos_. That each index has exactly
/// one writer is the invariant the whole class rests on, and it is why request_clear() only
/// *asks* for a drain that the consumer performs on its next read() -- resetting read_pos_
/// from the producer side would break it.
///
/// Lifted from upstream's PortAudioSink (examples/common/portaudio_sink.cpp), so the two
/// implementations buffer alike.
class PortAudioRingBuffer {
public:
    /// @brief Writes up to `len` bytes. Producer side.
    /// @return Bytes actually written, which is short of `len` when the ring is nearly full.
    size_t write(const uint8_t* data, size_t len);

    /// @brief Reads up to `len` bytes, zero-filling any shortfall. Consumer side.
    ///
    /// Zeroed bytes are silence for the signed PCM the player emits, so a starved callback
    /// outputs a gap rather than whatever the device buffer last held.
    /// @return Bytes of real audio read, not counting the silence padding.
    size_t read(uint8_t* dest, size_t len);

    /// @brief Bytes available to read.
    size_t available() const;

    /// @brief Bytes that can be written before the ring is full.
    size_t free_space() const;

    /// @brief Asks the consumer to drop everything buffered, on its next read().
    ///
    /// Safe to call while the callback is running, and the only clearing operation that is.
    void request_clear();

    /// @brief Drops everything buffered, here and now.
    ///
    /// Writes both positions, so it is only safe with no reader running -- after
    /// Pa_AbortStream(), with the producer's mutex held.
    void drop();

    /// @brief Resizes the ring and drops everything in it. Same restriction as drop().
    void reset(size_t capacity);

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_{0};
    std::atomic<size_t> write_pos_{0};
    std::atomic<size_t> read_pos_{0};
    std::atomic<bool> clear_requested_{false};
};

/// @brief An AudioSink that plays through PortAudio.
///
/// The cross-platform backend, and the only one that makes noise on macOS. The device string
/// is what followed `-o portaudio:` -- an index as `-l` prints it, a device name matched in
/// full and case-insensitively, or empty for whatever this host's default output is.
///
/// The device is resolved afresh at every configure() rather than once at construction, so a
/// bare `-o portaudio` follows the host's default output as the user changes it, and a device
/// that was absent at startup is picked up at the next stream.
///
/// THREAD SAFETY: mutex_ guards stream_ and the ring buffer's producer side. The audio
/// callback deliberately takes no lock: it reads the ring lock-free, and reads the format
/// fields (bytes_per_frame_, bits_, channels_, stream_rate_, ramp_step_) as plain values, plus
/// current_multiplier_, which it also writes. Those unsynchronised reads
/// are legal only because of one ordering invariant -- **everything the callback reads is
/// mutated only while the callback provably cannot run**: either before Pa_StartStream(), which
/// is what first lets it run, or after Pa_AbortStream()/Pa_CloseStream(), neither of which
/// returns while it is still running. That covers the ring's own storage as well as the format
/// fields.
///
/// write() blocks on a condition variable rather than holding mutex_ throughout, which is
/// where this sink differs from AlsaAudioSink: waiting releases the mutex, so configure() and
/// clear() can run to completion while a write() is parked. stream_generation_ is what makes
/// that safe -- see its declaration.
///
/// **write() may itself open and close a stream**, which is the one place this class lets the
/// sync task's thread do so, and it is safe for a reason worth stating outright: the ordering
/// invariant above is about *when*, not about which thread. Pa_AbortStream()/Pa_CloseStream() do
/// not return while the callback is running, and open_stream_() writes every field the callback
/// reads before Pa_StartStream() lets it run again -- neither depends on the caller's identity.
/// What keeps two openers apart is mutex_, which write(), configure(), clear() and stop() all
/// hold; the only other Pa_* callers, probe(), list_devices() and capabilities(), run on the main
/// loop before the server starts and so cannot overlap anything. See reopen_in_place_().
///
/// It does mean mutex_ is now held across a Pa_OpenStream(), which can take a few hundred
/// milliseconds -- so a configure(), clear() or stop() on the main loop can block behind a
/// recovering write() for that long. Bounded by the same budget everything else here is: at most
/// one such open per configured stream, against the one configure() already performs anyway.
///
/// The one recovery step that is *not* allowed there is rebuilding PortAudio's device list, which
/// invalidates every device index in the process: that stays on the main loop, in poll().
///
/// Volume is applied in the callback, on PortAudio's own output buffer, mirroring upstream.
/// That needs no scratch copy -- unlike AlsaAudioSink, which scales on the way in -- at the
/// cost of a volume change also reaching audio that is already buffered. Both are correct;
/// this one just takes effect sooner.
///
/// A change is applied over a ramp rather than as a jump, per the spec's SHOULD. The callback
/// advances the ramp by every frame it *scales*, because it consumes every frame it scales -- even
/// the zero-filled tail of a short ring read, which is deliberate: the ramp is heard in wall-clock
/// time and that silence is played rather than re-presented, so an underrun spends ramp time
/// against it exactly as it spends real time. This is the opposite of AlsaAudioSink's rule, where
/// unwritten frames *are* handed back and the advance has to follow what was written.
class PortAudioSink final : public AudioSink {
public:
    /// @param buffer_ms How much audio to keep in the ring, from --buffer-ms. Already
    /// range-checked by the parser, so it is taken as given rather than re-clamped -- but it
    /// is a request, not a promise: the device-latency and minimum-ring floors still win.
    PortAudioSink(std::string device, uint32_t buffer_ms);
    ~PortAudioSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// @brief Spends this sink's one device rescan, when a dead stream has escalated to it.
    ///
    /// Does nothing at all on an ordinary tick, and takes no lock to establish that. When there
    /// is something to do it is expensive: a Pa_Terminate()/Pa_Initialize() cycle re-enumerates
    /// every host API, which blocks this thread -- and so SendspinClient::loop() beside it -- for
    /// hundreds of milliseconds to seconds, depending on the host. That is affordable only
    /// because it happens at most once per stream, only on a stream that has already died, and
    /// never sooner than SINK_RESCAN_DELAY_MS after the last one. The transport itself runs on
    /// its own thread, so what is delayed is the dispatch of messages already received; the
    /// tightest deadline on that path is the time-sync burst response, which the library gives
    /// ten seconds.
    void poll(int64_t now_ms) override;

    /// @brief What the resolved device will take, probed through the same ladder -l reports.
    ///
    /// Opens no stream, but not free either: PortAudio's ALSA host API opens and closes the
    /// PCM inside each Pa_IsFormatSupported(). Follows AudioSink::capabilities()'s contract:
    /// main loop only, before the server starts. A device that will not resolve, or that
    /// something else holds, answers permissively.
    SinkCapabilities capabilities() const override;

    /// @brief Checks that `device` names exactly one output device on this host.
    ///
    /// Called from make_audio_sink() so a bad -o fails at startup rather than at the first
    /// stream. Deliberately resolution only: no format is tested, because a device that
    /// refuses 44.1 kHz but plays 48 kHz is configure()'s business to report per stream, not
    /// a reason to refuse to start.
    /// @param error Set to a human-readable reason when the return value is false.
    /// @return true if the device spec names one usable output device.
    static bool probe(const std::string& device, std::string& error);

    /// @brief Prints this host's PortAudio output devices. Backs part of -l.
    ///
    /// Index, name, host API, output channel count and default rate, with the system default
    /// marked. Input-only devices are left out, since -o cannot reach them.
    static void list_devices(std::FILE* out);

private:
    static int pa_callback(const void* input, void* output, unsigned long frame_count,
                           const PaStreamCallbackTimeInfo* time_info,
                           PaStreamCallbackFlags status_flags, void* user_data);

    /// Opens and starts a stream on `device` for a format. Caller holds mutex_ and has
    /// already closed any previous stream.
    bool open_stream_(PaDeviceIndex device, uint32_t sample_rate, uint8_t channels,
                      uint8_t bits_per_sample);
    /// Stops and closes the stream if open, and forgets the format. Caller holds mutex_.
    /// Idempotent, and deliberately leaves stopping_ alone -- see stop().
    void close_stream_();
    /// Restarts the open stream from an empty ring, for a new stream of the same format.
    /// Caller holds mutex_ and stream_ is not null.
    /// @return true if the stream is running again.
    bool restart_stream_();
    /// Makes the one in-place reopen attempt a dead stream gets, at last_format_ and against a
    /// freshly resolved device. Called from write(), so on the sync task's thread -- see the
    /// class docstring for why that is safe. Caller holds mutex_. Does nothing, and says so
    /// cheaply, unless a format is remembered and the attempt is still in hand.
    /// @return true if a stream is running again, so the caller can carry on filling the ring.
    bool reopen_in_place_();
    /// True while the open stream is still being driven by PortAudio. Caller holds mutex_.
    bool stream_alive_() const;
    /// Ring size in bytes for the open stream's format. Caller holds mutex_, and the format
    /// fields are already set.
    size_t ring_capacity_(double device_latency_s) const;
    /// Recomputes target_multiplier_ from volume_ and muted_.
    void update_target_multiplier_();

    /// Held for the sink's whole life, and declared first so it is destroyed last: the
    /// stream has to be closed before PortAudio is terminated under it.
    PortAudioGuard pa_;

    /// The device as -o spelled it, resolved per stream rather than kept as an index.
    std::string device_;
    /// Ring size to aim for, in milliseconds, before the floors in ring_capacity_() apply.
    uint32_t buffer_ms_;

    /// Serialises stream_, the format fields, and the ring buffer's producer side.
    std::mutex mutex_;
    /// Signalled by the audio callback once it has drained a buffer's worth of the ring.
    std::condition_variable space_available_;
    PortAudioRingBuffer ring_;
    PaStream* stream_{nullptr};
    PaDeviceIndex device_index_{paNoDevice};
    uint32_t rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    /// Read by the audio callback. See the ordering invariant in the class docstring.
    size_t bytes_per_frame_{0};
    /// The rate PortAudio says it really opened, for the callback's frame-to-time maths.
    /// Read by the audio callback, under the same invariant.
    double stream_rate_{0.0};
    /// Per-frame gain increment for this stream's rate, from volume_ramp_step().
    ///
    /// Precomputed rather than derived in the callback, which would put a 64-bit division on the
    /// audio path fifty times a second for a value that only changes when the format does. Read by
    /// the audio callback, under the same invariant. 0 means "do not ramp" -- see
    /// volume_ramp_step().
    uint64_t ramp_step_{0};

    /// Bumped whenever the stream is opened, closed, restarted or flushed.
    ///
    /// write() captures it before waiting and compares it on waking. Waiting releases mutex_,
    /// so a configure() or clear() can complete in that window -- and the rest of the caller's
    /// buffer then belongs to a stream that no longer exists, or to audio the player has just
    /// asked us to drop. Without this the loop would feed those bytes to the new stream, using
    /// the frame size of the old one, and report them as consumed.
    uint64_t stream_generation_{0};

    /// Set before stop() takes the mutex, so a write() blocked on a full ring bails out
    /// promptly instead of making shutdown wait for the device. Latches: nothing clears it,
    /// which is why close_stream_() must not touch it -- a format change would otherwise
    /// un-latch a shutdown already in progress.
    std::atomic<bool> stopping_{false};
    /// Latches when write() first finds no usable stream, purely so it complains once rather
    /// than on every buffer -- writes arrive around fifty times a second. Whether to discard
    /// is stream_alive_()'s call, not this flag's. Cleared by a configure() that succeeds.
    std::atomic<bool> failed_{false};

    /// The format the sink was last *asked* to play, which outlives the stream playing it.
    ///
    /// close_stream_() zeroes rate_/channels_/bits_, so recovery would have nothing to reopen at
    /// without this. It is set from configure()'s arguments rather than from a stream that opened
    /// successfully, deliberately: a configure() whose open failed is still the format the player
    /// is about to send audio in, and recovering to the *previous* one would play that audio at
    /// the wrong frame size. Guarded by mutex_. Zeroed by stop(), which is what makes a write()
    /// after shutdown attempt nothing.
    StreamFormat last_format_{};
    /// What is left to try about a stream that has died, and when. Guarded by mutex_, bar
    /// SinkRecovery::pending() -- see it.
    SinkRecovery recovery_;

    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
    /// The Q32 gain the ramp is heading for: Q32_ONE is unity, 0 is silence. Atomic because
    /// set_volume()/set_muted() write it from the main loop and the callback reads it.
    std::atomic<uint64_t> target_multiplier_{Q32_ONE};
    /// The Q32 gain actually being applied, which the callback walks toward target_multiplier_
    /// over VOLUME_RAMP_MS.
    ///
    /// Plain rather than atomic, under the class's ordering invariant: the callback is the only
    /// thing that *advances* it, and the two places the main loop snaps it -- open_stream_() and
    /// restart_stream_() -- both run before Pa_StartStream(), where the callback provably cannot
    /// be running. clear() deliberately does not touch it; see clear().
    std::uint64_t current_multiplier_{Q32_ONE};
};

}  // namespace sendspin_cli
