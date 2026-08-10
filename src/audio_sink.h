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

/// @file audio_sink.h
/// @brief Backend-agnostic destination for the PCM the sendspin player role decodes

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace sendspin_cli {

/// @brief Destination for decoded PCM frames coming out of the sendspin player role.
///
/// One implementation per audio backend: NullAudioSink (the device-less null/stdout pair),
/// AlsaAudioSink and PortAudioSink, the last two compiled in only where their library is
/// available.
///
/// THREAD SAFETY: write() is called on the sendspin sync task's background thread.
/// Every other method is called on the main loop thread. An implementation must
/// therefore make write() safe against concurrent configure(), clear(), stop(),
/// set_volume() and set_muted() calls.
class AudioSink {
public:
    virtual ~AudioSink() = default;

    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    /// @brief Backend name as accepted by -o, e.g. "null". Used in logs and by -l.
    virtual std::string name() const = 0;

    /// @brief Opens/reconfigures the device for a stream's format.
    ///
    /// Called from PlayerRoleListener::on_stream_start(), once the server has told us
    /// the stream parameters.
    /// @return true if the device accepted the format.
    virtual bool configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) = 0;

    /// @brief Consumes decoded PCM. Called on the sync task's background thread.
    ///
    /// May block for up to timeout_ms waiting for room. Partial writes are allowed but
    /// MUST be a whole number of PCM frames (a multiple of channels * bytes-per-sample):
    /// the sendspin sync task derives its playtime estimate from the returned count, so a
    /// mid-frame value drifts sync and starts the next write mid-frame.
    /// @return Number of bytes consumed.
    virtual size_t write(const uint8_t* data, size_t length, uint32_t timeout_ms) = 0;

    /// @brief Drops buffered audio without releasing the device (stream end, flush).
    virtual void clear() {}

    /// @brief Releases the device. Called once, during shutdown.
    virtual void stop() {}

    /// @brief Applies playback volume, 0-100. A backend with no volume control may ignore it.
    virtual void set_volume(uint8_t /*volume*/) {}

    /// @brief Silences output regardless of volume.
    virtual void set_muted(bool /*muted*/) {}

    /// @brief Reports frames that have actually reached the DAC, for sync feedback.
    ///
    /// Wired to PlayerRole::notify_audio_played() by PlayerListener. A backend that knows
    /// its own playout timing (a PortAudio callback, an ALSA delay query) should invoke
    /// this; a sink that consumes instantly can leave it unset, since the sync task also
    /// counts frames from write()'s return value.
    ///
    /// Must be assigned before SendspinClient::start_server(), and never afterwards: a
    /// backend may read it from its own audio thread, so reassigning it on a running
    /// player is a data race.
    std::function<void(uint32_t frames, int64_t timestamp)> on_frames_played;

protected:
    AudioSink() = default;
};

/// @brief Which backend a -o spec named.
enum class SinkBackend {
    Null,       ///< discard everything; needs no device
    Stdout,     ///< raw interleaved PCM on stdout; needs no device
    Alsa,       ///< an ALSA PCM, named by DeviceSpec::device
    PortAudio,  ///< a PortAudio device by index or name, or this host's default if empty
};

/// @brief A -o spec resolved into a backend and the device to hand it.
struct DeviceSpec {
    SinkBackend backend{SinkBackend::Null};
    /// Backend-specific device name. Empty for the device-less sinks, and for a backend whose
    /// device is optional -- PortAudio reads an empty one as this host's default output.
    std::string device;
};

/// @brief Resolves a -o spec, without opening anything.
///
/// The rule, in this order:
///  1. The whole string names a backend on its own: a reserved device-less name (`null`,
///     `stdout`, `-`), or a backend whose device is optional (`portaudio`). A backend that
///     requires a device (`alsa`) is rejected here rather than handed an empty one.
///  2. The string contains a colon and the text before the **first** colon names a
///     backend -- first colon only, because ALSA device names carry their own, so
///     `alsa:hw:2,0` is the ALSA backend playing `hw:2,0`.
///  3. Otherwise it is an ALSA PCM name, which is squeezelite's `-o` model. `hw:2,0` and
///     `default` keep working with no prefix at all. This stays ALSA-only on purpose:
///     PortAudio *does* have an enumerable device list, so reaching one of its devices
///     without the prefix would make the same `-o` mean different things per host.
///
/// Separate from make_audio_sink() because this half is pure string work: it can be
/// tested, and reasoned about, without a sound card in the machine.
/// @param error Set to a human-readable reason when the return value is false.
/// @return true if `spec` named something this build can play through.
bool resolve_device_spec(const std::string& spec, DeviceSpec& out, std::string& error);

/// @brief The backend prefixes this build has, e.g. "null, stdout, alsa". For diagnostics.
std::string audio_backend_list();

/// @brief Builds the sink named by -o.
///
/// Resolves the spec (see resolve_device_spec()) and opens what it names. This is the
/// registry every backend hooks into: an entry in the table resolve_device_spec() reads,
/// and its construction here.
/// @param device Device/backend spec as given to -o.
/// @param error Set to a human-readable reason when the return value is nullptr.
/// @return The sink, or nullptr if `device` is not recognized.
std::unique_ptr<AudioSink> make_audio_sink(const std::string& device, std::string& error);

/// @brief Prints the device specs that make_audio_sink() accepts. Backs the -l flag.
void print_audio_devices(std::FILE* out);

}  // namespace sendspin_cli
