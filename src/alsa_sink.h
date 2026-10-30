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

/// AudioSink over ALSA snd_pcm, with snd_pcm_delay()-based sync feedback.

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

/// How a 0-100 volume maps onto a hardware mixer control's dB range.
enum class MixerCurve {
    kSpec,   ///< The spec's perceived-loudness curve: amplitude (v/100)^1.5, 30*log10 dB.
    kCubic,  ///< Amplitude (v/100)^3, the steeper curve Snapcast uses for hardware volume.
             ///< Its inverse, 100*10^(dB/60), is alsamixer's slider position, so the two track
             ///< each other on a control with more than 24 dB of range.
};

/// Parses a curve name ("spec", "cubic"). @return false when unrecognized.
bool parse_mixer_curve(const std::string& name, MixerCurve& curve);

/// An AudioSink that plays through an ALSA PCM, e.g. `default` or `hw:2,0`.
/// Every snd_pcm_* call holds device_mutex_. write() never reopens a dead device; poll() does.
class AlsaAudioSink final : public AudioSink {
public:
    /// @param buffer_ms Ring size request, already range-checked; ALSA rounds it to the card.
    /// @param mixer_spec `card:control` naming the hardware mixer to drive, e.g. `1:Digital`;
    ///                   empty keeps the software volume. Only the ALSA backend takes one.
    /// @param curve How volume maps onto the mixer's dB range; ignored without a mixer.
    AlsaAudioSink(std::string device, uint32_t buffer_ms, std::string mixer_spec = {},
                  MixerCurve curve = MixerCurve::kSpec);
    ~AlsaAudioSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void release() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// Reopens a device that died mid-stream once SinkRecovery's delay is up.
    void poll(int64_t now_ms) override;

    /// What this PCM will take; opens the device briefly.
    SinkCapabilities capabilities() const override;

    /// Checks that `device` names a playback PCM this host can open; a busy device passes.
    static bool probe(const std::string& device, std::string& error);

    /// Checks that `mixer_spec` names a playback mixer control this host can open.
    static bool probe_mixer(const std::string& mixer_spec, std::string& error);

    /// Reads the mixer's current volume and mute; -1 volume when the mixer cannot be read.
    /// @param muted Set from the control's playback switch; false without one.
    /// @param curve How volume maps onto the mixer's dB range.
    static int read_mixer_volume(const std::string& mixer_spec, bool& muted,
                                 MixerCurve curve = MixerCurve::kSpec);

    /// Prints the host's playback PCMs, as `aplay -L` does.
    static void list_devices(std::FILE* out);

private:
    /// Opens the device for a format. Caller holds device_mutex_.
    bool open_device_(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
    /// Closes the device if open. Caller holds device_mutex_. Idempotent.
    void close_device_();
    /// Recovers a PCM error in place or retires a lost device. Caller holds device_mutex_.
    /// @return true if the operation can be retried; on false pcm_ may already be closed.
    bool recover_(int err);
    /// Retires a lost device and arms poll() to get it back. Caller holds device_mutex_.
    /// @return false always.
    bool handle_device_loss_();
    /// Sets target_multiplier_ from volume_ and muted_ for the software-gain path.
    void update_target_multiplier_();

    /// Opens the mixer for `mixer_spec`; caller closes *handle even on a null element.
    static snd_mixer_elem_t* open_mixer_(const std::string& mixer_spec, snd_mixer_t** handle);
    /// The dB range for playback volume, or false when the control has none.
    static bool playback_dB_range_(snd_mixer_elem_t* elem, long& min, long& max);
    /// A control with one dB range for both channels; false when no channel reports one.
    static bool playback_dB_(snd_mixer_elem_t* elem, long& dB);
    /// The playback switch state of the first channel that has one; false when none does.
    static bool playback_switch_(snd_mixer_elem_t* elem);

    /// Opens the persistent mixer handle and element; false when the spec has no usable control.
    bool mixer_open_();
    /// Closes the persistent mixer handle. Idempotent.
    void mixer_close_();
    /// Records the control's current dB and switch as our own, so a later event reads as an echo.
    void sync_mixer_reported_();
    /// Writes `dB` to the persistent element, recording the read-back as our own value.
    /// @return false when there is no control, or ALSA refused the write.
    bool write_mixer_dB_(long dB);
    /// @return true when the control took the mute; false when it has no switch, leaving the
    ///         caller on software mute.
    bool set_mixer_mute_(bool muted);
    /// Begins a short stepped write toward `target_dB`; false when the first step was refused.
    bool start_mixer_ramp_(long target_dB);
    /// Writes one ramp step; a no-op outside a ramp.
    /// @return false when the step was refused, which abandons the ramp where it stands.
    bool step_mixer_ramp_();
    /// Records the mixer's current volume and mute, unless it is an echo of our own write.
    void report_mixer_change_();
    /// Steps any ramp and handles a pending control event. Main-loop only.
    void poll_mixer_();
    /// Q32 software gain to pair with the mixer: unity when it carries the level, 0 when silencing.
    uint64_t software_gain_() const;

    std::string device_;
    /// Ring size to ask ALSA for, in milliseconds; the period is this over PERIODS_PER_BUFFER.
    uint32_t buffer_ms_;
    /// `card:control` for hardware volume, empty for software volume.
    std::string mixer_spec_;
    /// Curve mapping volume onto the mixer's dB range; kSpec unless configured otherwise.
    MixerCurve mixer_curve_{MixerCurve::kSpec};

    /// Serialises every snd_pcm_* call and the fields describing the open stream.
    std::mutex device_mutex_;
    snd_pcm_t* pcm_{nullptr};
    uint32_t rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    size_t bytes_per_frame_{0};
    /// The format last announced; survives close_device_() so poll() can reopen at it.
    StreamFormat last_format_{};
    /// Scratch for volume-scaled samples, reused across writes.
    std::vector<uint8_t> scaled_;

    /// Set before stop() takes the mutex, so an in-flight write() bails out promptly.
    std::atomic<bool> stopping_{false};
    /// Latches once the device is unusable, so write() discards instead of spinning.
    std::atomic<bool> failed_{false};

    /// Guarded by device_mutex_, except SinkRecovery::pending().
    SinkRecovery recovery_;

    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
    /// Q32 gain the ramp is heading for; written from the main loop without device_mutex_.
    std::atomic<uint64_t> target_multiplier_{Q32_ONE};
    /// Q32 gain being applied, walked toward target_multiplier_; guarded by device_mutex_.
    std::uint64_t current_multiplier_{Q32_ONE};

    /// The persistent mixer handle and element; both null when --alsa-mixer opened nothing.
    /// Touched only from the main loop after construction, so no lock guards them.
    snd_mixer_t* mixer_{nullptr};
    snd_mixer_elem_t* mixer_elem_{nullptr};
    /// The control's playback dB range; valid only while mixer_elem_ is set.
    long mixer_dB_min_{0};
    long mixer_dB_max_{0};
    /// True when the control has a playback switch, so mute is hardware rather than software.
    bool mixer_has_switch_{false};
    /// True while the control is carrying the level. A refused volume write clears it, handing the
    /// level to the software gain; a write that succeeds sets it again. Tracked apart from the
    /// mute so a failed switch write does not also drop the level into software, scaling it twice.
    bool mixer_carries_volume_{false};
    /// True while the switch is carrying the mute. Only meaningful with a switch; a control
    /// without one, or one whose switch write was refused, leaves the mute to the software gain.
    bool mixer_carries_mute_{false};
    /// The switch state we last set or read; an event holding it is our own echo.
    bool mixer_reported_muted_{false};
    /// dB the control was last set to by us or read back; an event holding it is our own echo.
    long mixer_reported_dB_{0};
    /// A stepped write in progress: where it started, where it ends, and how far along it is.
    bool mixer_ramping_{false};
    long mixer_ramp_from_dB_{0};
    long mixer_ramp_target_dB_{0};
    int mixer_ramp_step_{0};
    int64_t mixer_ramp_next_ms_{0};
};

/// Maps a 0-100 volume onto a hardware mixer's dB range on the given curve.
/// The spec curve is amplitude (v/100)^1.5, i.e. 30*log10(v/100) dB below max; the cubic curve
/// is twice that. Clamped to the control's floor.
long mixer_dB_for_volume(long min, long max, uint8_t volume,
                         MixerCurve curve = MixerCurve::kSpec);

/// The inverse of mixer_dB_for_volume(): the 0-100 volume a mixer dB reading stands for.
uint8_t volume_for_mixer_dB(long min, long max, long dB, MixerCurve curve = MixerCurve::kSpec);

/// Routes libasound's diagnostics to our logger; call before any snd_* that can fail.
void install_alsa_error_handler();

/// Prints the host's PCMs for one direction, each with its description and capabilities.
/// @param reachable Whether the flag being listed for reaches a PCM by its bare name.
void list_alsa_pcms(std::FILE* out, snd_pcm_stream_t stream,
                    bool (*reachable)(const std::string& pcm));

}  // namespace sendspin_cli
