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

/// Backend-agnostic destination for the PCM the sendspin player role decodes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sendspin_cli {

/// Sample rates a device is probed for, ascending.
inline constexpr std::array<uint32_t, 8> PROBE_RATES{22050, 32000, 44100,  48000,
                                                     88200, 96000, 176400, 192000};

/// Bit depths a device is probed for: the packed little-endian depths the decoders emit.
inline constexpr std::array<uint8_t, 4> PROBE_BIT_DEPTHS{8, 16, 24, 32};

/// Channel counts a device is probed for: mono through 7.1.
inline constexpr std::array<uint8_t, 5> PROBE_CHANNELS{1, 2, 4, 6, 8};

/// What a sink's device will take; the three axes are independent, not combinations.
struct SinkCapabilities {
    std::vector<uint32_t> rates;      ///< ascending, a subset of PROBE_RATES
    std::vector<uint8_t> bit_depths;  ///< ascending, a subset of PROBE_BIT_DEPTHS
    std::vector<uint8_t> channels;    ///< ascending, a subset of PROBE_CHANNELS

    /// All three ladders in full: for device-less sinks and devices that cannot be probed.
    static SinkCapabilities permissive();
};

/// Gain until a server sets one; full, since a server may never send a volume.
inline constexpr uint8_t DEFAULT_SINK_VOLUME = 100;

/// Where a sink's current gain came from, for `status`.
enum class VolumeSource {
    SinkDefault,  ///< nothing has set it: DEFAULT_SINK_VOLUME, unmuted
    Restored,     ///< read back from the state store at startup
    Server,       ///< a server sent a volume or mute command this run
};

/// The parameters configure() takes, as one value.
struct StreamFormat {
    uint32_t sample_rate{0};
    uint8_t channels{0};
    uint8_t bit_depth{0};
};

/// Destination for decoded PCM, one implementation per audio backend.
/// write() runs on the sync task's thread and must be safe against every other method.
class AudioSink {
public:
    virtual ~AudioSink() = default;

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    /// Backend name as accepted by -o, e.g. "null".
    virtual std::string name() const = 0;

    /// Opens or reconfigures the device for a stream's format.
    /// @return true if the device accepted the format.
    virtual bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) = 0;

    /// Consumes PCM on the sync task's thread, blocking up to `timeout_ms` for room.
    /// @return Bytes consumed; must be a whole number of frames, or sync drifts.
    virtual size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) = 0;

    /// Drops buffered audio without releasing the device.
    virtual void clear() {}

    /// Releases the device. Called once, during shutdown.
    virtual void stop() {}

    /// Applies playback volume, 0-100.
    virtual void set_volume(uint8_t /*volume*/) {}

    /// Silences output regardless of volume.
    virtual void set_muted(bool /*muted*/) {}

    /// Main-loop slice for work write()'s thread must not do; must stay cheap on an ordinary tick.
    /// @param now_ms Monotonic milliseconds; derive timing from this, not from counting calls.
    virtual void poll(int64_t /*now_ms*/) {}

    /// What the device will take, probed once before start().
    /// An unprobeable device must answer SinkCapabilities::permissive(), never an empty set.
    virtual SinkCapabilities capabilities() const {
        return SinkCapabilities::permissive();
    }

    /// Reports frames that reached the DAC, for sync feedback; optional for instant sinks.
    /// Assign before start() and never after: backends read it from their audio thread.
    std::function<void(uint32_t frames, int64_t timestamp)> on_frames_played;

protected:
    AudioSink() = default;
};

/// Which backend a -o spec named.
enum class SinkBackend {
    Null,       ///< discard everything; needs no device
    Stdout,     ///< raw interleaved PCM on stdout; needs no device
    Alsa,       ///< an ALSA PCM, named by DeviceSpec::device
    CoreAudio,  ///< a CoreAudio device by index or name, or this host's default if empty
    PortAudio,  ///< a PortAudio device by index or name, or this host's default if empty
    Pulse,      ///< a PulseAudio sink by name, or the server's own default if empty
    PipeWire,   ///< a PipeWire node by name, or the graph's own default routing if empty
};

/// A -o spec resolved into a backend and the device to hand it.
struct DeviceSpec {
    SinkBackend backend{SinkBackend::Null};
    /// Backend-specific device name; empty for device-less sinks or a backend's default.
    std::string device;
};

/// Resolves a -o spec without opening anything: a bare backend name, `<backend>:<device>` split
/// on the first colon, or else an ALSA PCM name. Bare backend names shadow same-named ALSA PCMs.
/// @return true if `spec` named something this build can play through.
bool resolve_device_spec(const std::string& spec, DeviceSpec& out, std::string& error);

/// The backend prefixes this build has, e.g. "null, stdout, alsa".
std::string audio_backend_list();

/// True if a bare `-o <pcm>` reaches the ALSA PCM of that name rather than a shadowing backend.
bool alsa_pcm_is_reachable(const std::string& pcm);

/// Prints one capability set as the three indented lines -l shows under a device.
/// @param depth_names This backend's spelling of PROBE_BIT_DEPTHS, in the same order.
void print_sink_capabilities(std::FILE* out, const SinkCapabilities& caps,
                             const std::array<const char*, PROBE_BIT_DEPTHS.size()>& depth_names);

/// Builds the sink named by -o.
/// @param buffer_ms Already range-checked by the parser.
/// @return The sink, or nullptr with `error` set.
std::unique_ptr<AudioSink> make_audio_sink(const std::string& device, uint32_t buffer_ms,
                                           std::string& error);

/// Prints the device specs make_audio_sink() accepts, for -l.
void print_audio_devices(std::FILE* out);

}  // namespace sendspin_cli
