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

/// AudioSink over CoreAudio's AUHAL output unit, with render-callback DAC-time sync feedback.

#pragma once

#include "audio_sink.h"
#include "pcm_ring.h"
#include "pcm_volume.h"
#include "sink_recovery.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace sendspin_cli {

/// An AudioSink that plays through CoreAudio: `-o coreaudio:` an index, a device name, or empty.
/// The render callback reads format fields unlocked: change them only while no unit is running.
class CoreAudioSink final : public AudioSink {
public:
    /// @param buffer_ms Ring size request; the device-latency and minimum-ring floors win.
    CoreAudioSink(std::string device, uint32_t buffer_ms);
    ~CoreAudioSink() override;

    std::string name() const override;
    bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) override;
    size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) override;
    void clear() override;
    void stop() override;
    void set_volume(uint8_t volume) override;
    void set_muted(bool muted) override;

    /// Follows a moved system default, and retries the reopen once a dead device has
    /// escalated; blocks while either runs.
    void poll(int64_t now_ms) override;

    /// What the resolved device's AUHAL unit will take; opens a scratch unit to ask.
    SinkCapabilities capabilities() const override;

    /// Checks that `device` names exactly one output device on this host; no format is tested.
    static bool probe(const std::string& device, std::string& error);

    /// Prints this host's CoreAudio output devices.
    static void list_devices(std::FILE* out);

private:
    static OSStatus render_callback(void* user_data, AudioUnitRenderActionFlags* flags,
                                    const AudioTimeStamp* timestamp, UInt32 bus, UInt32 frames,
                                    AudioBufferList* data);

    /// Property listener for device death and default-output changes; runs off a HAL thread.
    /// Only ever flags work for poll() and write(): it must not take mutex_.
    static OSStatus property_listener(AudioObjectID object, UInt32 count,
                                      const AudioObjectPropertyAddress* addresses, void* user_data);

    /// Opens, initializes and starts an AUHAL unit. Caller holds mutex_ and has closed any
    /// previous unit.
    bool open_unit_(AudioDeviceID device, uint32_t sample_rate, uint8_t channels,
                    uint8_t bits_per_sample);
    /// Stops, uninitializes and disposes the unit and forgets the format. Caller holds mutex_.
    /// Must not touch stopping_, or a format change would un-latch a shutdown.
    void close_unit_();
    /// Restarts the open unit from an empty ring. Caller holds mutex_; unit_ is not null.
    /// @return true if the unit is running again.
    bool restart_unit_();
    /// The one in-place reopen a dead unit gets, from write(). Caller holds mutex_.
    /// @return true if a unit is running again.
    bool reopen_in_place_();
    /// Reopens on the system default's new device, for a bare -o coreaudio. Not a recovery: a
    /// move is an ordinary event, so it must not spend the budget an outage needs.
    void follow_default_();
    /// Adds the frames still in the ring to the outage gap: the player has counted them, and the
    /// lost unit recovery is about to close will never report them. Only for those closes --
    /// stop(), configure() and clear() end the stream the gap belonged to. Caller holds mutex_.
    void discard_ring_tail_();
    /// True while the render callback is still being driven. Caller holds mutex_.
    /// Liveness only: a lost device does not stop the callback, so this stays true until the
    /// unit does. Anything touching the ring's consumer side must ask this, not unit_alive_().
    bool callback_running_() const;
    /// True while the open unit is still worth feeding: running, on a device that has not died.
    /// Caller holds mutex_.
    bool unit_alive_() const;
    /// Ring size in bytes. Caller holds mutex_ and the format fields are set.
    size_t ring_capacity_(double device_latency_s) const;
    void update_target_multiplier_();

    /// Starts listening for the open device's death, and for default-output moves when following
    /// the default. Caller holds mutex_; listeners are removed by close_unit_().
    /// Removal does not wait an in-flight listener out, unlike PortAudio's stream close, so a
    /// notification can still land on the atomics just after the sink is destroyed.
    void add_listeners_(AudioDeviceID device);
    void remove_listeners_();

    /// The device as -o spelled it, resolved per stream.
    std::string device_;
    /// Ring size to aim for, in milliseconds, before the floors in ring_capacity_() apply.
    uint32_t buffer_ms_;

    /// Serialises unit_, the format fields, and the ring buffer's producer side.
    std::mutex mutex_;
    /// Signalled by the render callback once it has drained a buffer's worth of the ring.
    std::condition_variable space_available_;
    PcmRingBuffer ring_;
    AudioUnit unit_{nullptr};
    /// True between AudioOutputUnitStart() and the matching stop. Guarded by mutex_.
    bool running_{false};
    AudioDeviceID device_id_{kAudioObjectUnknown};
    uint32_t rate_{0};
    uint8_t channels_{0};
    uint8_t bits_{0};
    /// Read by the render callback unlocked; see the class comment.
    size_t bytes_per_frame_{0};
    /// The rate the unit's input scope really took. Read by the render callback.
    double stream_rate_{0.0};
    /// Per-frame gain increment from volume_ramp_step(); 0 means no ramp. Read by the callback.
    uint64_t ramp_step_{0};
    /// Mach ticks per microsecond, from mach_timebase_info(). Read by the render callback.
    double host_ticks_per_us_{0.0};
    /// Output path latency the render timestamp does not carry, in microseconds. Read by the
    /// render callback.
    int64_t output_latency_us_{0};

    /// Bumped on open, close, restart or flush; a write() that waited compares it on waking.
    uint64_t stream_generation_{0};

    /// Set by the property listener when the open device dies. Latches until the next open;
    /// read unlocked by write().
    std::atomic<bool> device_lost_{false};
    /// Set by the property listener when the system default output moves; poll() clears it.
    std::atomic<bool> default_moved_{false};
    /// Set by the property listener when a device is added or removed anywhere on the host.
    /// Kept until a rescan is owed, so a replug during the backoff is not waited out.
    std::atomic<bool> devices_changed_{false};
    /// Whether each listener is registered, so each is removed exactly once. Guarded by mutex_.
    bool listening_alive_{false};
    bool listening_default_{false};
    bool listening_devices_{false};
    /// The device the death listener is registered on. Guarded by mutex_.
    AudioDeviceID listening_device_{kAudioObjectUnknown};

    /// Set before stop() takes the mutex so a blocked write() bails out; latches.
    std::atomic<bool> stopping_{false};
    /// Latches so write() complains once about a missing unit; cleared by a good configure().
    std::atomic<bool> failed_{false};

    /// Format last asked for, even if its open failed; recovery reopens at it. Guarded by mutex_.
    StreamFormat last_format_{};
    /// Guarded by mutex_, except SinkRecovery::pending().
    SinkRecovery recovery_;
    /// The outage gap once write() has handed it on, for the callback to retire lock-free with
    /// its next report. recovery_ cannot be read there; see OutageGapHandoff.
    OutageGapHandoff gap_handoff_;

    std::atomic<uint8_t> volume_{DEFAULT_SINK_VOLUME};
    std::atomic<bool> muted_{false};
    /// Q32 gain the ramp is heading for; written by the main loop, read by the callback.
    std::atomic<uint64_t> target_multiplier_{Q32_ONE};
    /// Q32 gain being applied; only the callback advances it. clear() must not touch it.
    std::uint64_t current_multiplier_{Q32_ONE};
};

}  // namespace sendspin_cli
