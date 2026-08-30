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

/// @file alsa_sink.h
/// @brief AudioSink over ALSA snd_pcm, with snd_pcm_delay()-based sync feedback

#pragma once

#include "audio_sink.h"
#include "pcm_volume.h"
#include "sink_recovery.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

/// @brief An AudioSink that plays through an ALSA PCM device.
///
/// The device string is an ALSA PCM name exactly as `aplay -L` prints it -- `default`,
/// `pipewire`, `hw:2,0`, `plughw:1,0`, `hdmi:CARD=NVidia,DEV=0`. Anything `-o` does not
/// reserve for the device-less sinks is handed here, which is squeezelite's model.
///
/// THREAD SAFETY: an snd_pcm_t is not thread-safe, so every snd_pcm_* call is made under
/// device_mutex_. write() runs on the sync task's background thread while configure(),
/// clear() and stop() run on the main loop, so the mutex is what keeps a shutdown from
/// closing the handle mid-write.
///
/// The mutex alone would let a blocked write() stall shutdown for as long as the device
/// takes to drain, so write() never blocks unboundedly while holding it: it waits for
/// room in short slices (snd_pcm_wait) and re-checks the abort flag between them. That is
/// the write_mutex_ + abort_write_ pairing upstream's PortAudio sink uses.
///
/// **write() never reopens the device**, which is that same rule reaching recovery. A device
/// that dies mid-stream is closed where it is found and handed to poll(), because the reopen is
/// snd_pcm_open() and there is no way to bound one: the sound-server sinks make their inline
/// attempt only because PULSE_RECOVERY_TIMEOUT_MS lets them put a deadline on it, and ALSA has
/// no such knob. On a plugin PCM -- `default` on a PipeWire host, or the `alsa:pulse` route --
/// it parses config and waits on a daemon socket with no timeout at all, on the sync task's
/// thread and under device_mutex_. So AlsaAudioSink spends SinkRecovery's inline attempt
/// without making it, which is what escalates to the delayed one; see recover_(). That delay is
/// also what the case wants, a replugged DAC taking seconds to enumerate.
///
/// Volume is applied in software, through the shared Q32 fixed-point scaling in pcm_volume.h,
/// so a stream sounds the same here as through PortAudio. Scaling happens on the way *into*
/// the device, so a volume change reaches only audio not yet written -- unlike PortAudioSink,
/// which scales in its callback and so also affects what is already buffered.
///
/// A change is applied over a ramp rather than as a jump, per the spec's SHOULD. The ramp is
/// committed by the frames write() really *wrote*, not the ones it scaled: the loop can break out
/// early on a deadline, on stopping_ or on a failed recover_(), and the sync task then re-presents
/// the unconsumed tail -- so advancing by the whole buffer would leave a gain discontinuity across
/// that seam, which is the click the ramp exists to remove. Scaling a tail that is then discarded
/// costs nothing: it is scratch. This is the opposite of PortAudioSink's rule, whose callback
/// consumes everything it scales.
///
/// The ALSA hardware mixer is deliberately not used: the default device here is usually
/// PipeWire's ALSA plugin, where a hardware mixer element is either absent or controls
/// something other than this stream.
class AlsaAudioSink final : public AudioSink {
public:
    /// @param buffer_ms How much audio to keep queued in the device ring, from --buffer-ms.
    /// Already range-checked by the parser, so it is taken as given rather than re-clamped;
    /// ALSA still rounds it to something the card can do (`_near`).
    AlsaAudioSink(std::string device, uint32_t buffer_ms);
    ~AlsaAudioSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// @brief Reopens a device that died mid-stream, once the delay SinkRecovery imposes is up.
    ///
    /// Does nothing on an ordinary tick and takes no lock to establish that, like
    /// PulseAudioSink::poll(). What it does when there is work is the whole of this sink's
    /// recovery, write() having only closed the device and escalated -- a fresh
    /// snd_pcm_open() at last_format_, which re-resolves `hw:CARD=NAME` and so can find a card
    /// that has come back on a different index.
    ///
    /// Up to SINK_RESCAN_ATTEMPTS of them per configured stream, behind a delay that doubles.
    /// The outcome is reported to SinkRecovery rather than assumed, because this attempt is the
    /// repeatable kind: a device absent now may be present four seconds later, which is exactly
    /// what a replug is. Reporting success unconditionally, as PortAudioSink's one-shot device
    /// rescan does, would collapse the budget to a single attempt.
    void poll(int64_t now_ms) override;

    /// @brief What this PCM will take, probed through the same ladder -l reports.
    ///
    /// Opens the device briefly, so it follows AudioSink::capabilities()'s contract: main
    /// loop only, before the server starts. A PCM that will not open answers permissively.
    SinkCapabilities capabilities() const override;

    /// @brief Checks that `device` names a PCM this host can open, without keeping it.
    ///
    /// Called from make_audio_sink() so a typo in -o fails at startup rather than at the
    /// first stream. A device that exists but is currently busy (-EBUSY, an exclusive
    /// `hw:` PCM someone else holds) deliberately passes: that is a transient condition
    /// for configure() to report, not a reason to refuse to start.
    /// @param error Set to a human-readable reason when the return value is false.
    /// @return true if the device name resolves to a playback PCM.
    static bool probe(const std::string& device, std::string& error);

    /// @brief Prints the host's playback PCMs, as `aplay -L` does. Backs part of -l.
    static void list_devices(std::FILE* out);

private:
    /// Opens the device for a format. Caller holds device_mutex_.
    bool open_device_(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
    /// Closes the device if open. Caller holds device_mutex_. Idempotent.
    void close_device_();
    /// Handles an error from a PCM call made on a live stream. Caller holds device_mutex_.
    ///
    /// -EINTR/-EAGAIN are retried as they are; -EPIPE and -ESTRPIPE are recovered in place, by a
    /// prepare() and by a resume() respectively -- with a prepare() as the suspend fallback,
    /// which restarts the stream where the resume would have kept its position. Every other
    /// error, and a prepare() that will not take either, is taken to mean the device itself is
    /// gone -- that being the device answering rather than the ring -- and goes to
    /// handle_device_loss_().
    /// @return true if the stream was recovered and the operation can be retried. **A false
    /// return may mean the handle is now closed**, so a caller must not touch pcm_ after one
    /// without re-reading it.
    bool recover_(int err);
    /// Retires a device that is gone and arms poll() to get it back. Caller holds device_mutex_.
    ///
    /// The caller says what happened, at whatever level suits it; this only acts on it.
    /// @return false always, so recover_()'s failure paths can `return` it directly.
    bool handle_device_loss_();
    /// Recomputes target_multiplier_ from volume_ and muted_.
    void update_target_multiplier_();

    std::string device_;
    /// Ring size to ask ALSA for, in milliseconds; the period is this over PERIODS_PER_BUFFER.
    uint32_t buffer_ms_;

    /// Serialises every snd_pcm_* call and the fields describing the open stream.
    std::mutex device_mutex_;
    snd_pcm_t* pcm_{nullptr};
    uint32_t rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    size_t bytes_per_frame_{0};
    /// The format the player last announced, which outlives the handle that was open for it.
    ///
    /// rate_/channels_/bits_ above describe the *open stream* and are zeroed with it by
    /// close_device_(), so they are gone by the time a reopen needs them. This is the other
    /// question -- what the player is sending, whether or not a device is currently taking it --
    /// and it is what poll() reopens at. Set by configure() before anything can fail, for the
    /// same reason the other three sinks set theirs there.
    StreamFormat last_format_{};
    /// Scratch for volume-scaled samples, since write()'s input buffer is const. Grown to
    /// the largest write seen and reused, so steady-state playback does not allocate.
    std::vector<uint8_t> scaled_;

    /// Set before stop() takes the mutex, so an in-flight write() bails out promptly
    /// instead of making shutdown wait for the device to drain.
    std::atomic<bool> stopping_{false};
    /// Latches once the device is unusable, so write() discards instead of returning 0
    /// forever and spinning the sync task. Same degrade-don't-stall rule as NullAudioSink.
    std::atomic<bool> failed_{false};

    /// Whether to try to get a dead device back, and when. Guarded by device_mutex_ except for
    /// SinkRecovery::pending().
    SinkRecovery recovery_;

    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
    /// The Q32 gain the ramp is heading for: Q32_ONE is unity, 0 is silence. Atomic because
    /// set_volume()/set_muted() write it from the main loop without taking device_mutex_, and
    /// write() reads it on the sync task's thread.
    std::atomic<uint64_t> target_multiplier_{Q32_ONE};
    /// The Q32 gain actually being applied, which write() walks toward target_multiplier_ over
    /// VOLUME_RAMP_MS.
    ///
    /// Not atomic, and it does not need to be: every function that touches it -- write(),
    /// configure() and clear() -- holds device_mutex_, which is this sink's whole threading model.
    std::uint64_t current_multiplier_{Q32_ONE};
};

}  // namespace sendspin_cli
