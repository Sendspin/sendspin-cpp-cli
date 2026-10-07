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

/// Backend-agnostic capture device feeding the sendspin source role.

#pragma once

#include "audio_sink.h"

#include <sendspin/source_role.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sendspin_cli {

/// Bit depths the source role takes, in the order a capture device is asked for them.
inline constexpr std::array<uint8_t, 3> CAPTURE_BIT_DEPTHS{16, 24, 32};

/// Source of captured PCM, one implementation per audio backend.
/// open() and read() run on the capture thread only.
class AudioSource {
public:
    virtual ~AudioSource() = default;

    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;

    /// The device as --input reaches it, e.g. "hw:1,0".
    virtual std::string name() const = 0;

    /// Narrows `format` to the nearest one the device captures, opening it briefly.
    /// @return false with `error` set if the device cannot be opened or offers nothing usable.
    virtual bool negotiate(StreamFormat& format, std::string& error) = 0;

    /// Opens the device for capture at a format negotiate() returned.
    virtual bool open(const StreamFormat& format) = 0;

    /// Blocks up to `timeout_ms` for captured PCM.
    /// @param capture_time_us Steady-clock time of the first sample returned, or 0 if unknown.
    /// @return Bytes read, a whole number of frames; 0 on timeout; negative once the device is
    /// lost.
    virtual int read(uint8_t* data, size_t length, uint32_t timeout_ms,
                     int64_t& capture_time_us) = 0;

    virtual void close() = 0;

protected:
    AudioSource() = default;
};

/// Runs an AudioSource on one capture thread while the source role's input stream is open.
/// Main loop only. Must be destroyed before the client its writer reaches.
class SourceCapture final : public sendspin::SourceRoleListener {
public:
    /// SourceRole::write_audio(), or a stand-in; called from the capture thread only.
    using Writer = std::function<bool(const uint8_t* data, size_t len, int64_t capture_time_us)>;

    /// `source` must outlive this object.
    SourceCapture(AudioSource& source, StreamFormat format, Writer writer);
    ~SourceCapture() override;

    void on_streaming_started() override;
    /// Returns once the capture thread has joined.
    void on_streaming_stopped() override;

    bool streaming() const {
        return this->thread_.joinable();
    }

    const StreamFormat& format() const {
        return this->format_;
    }

    std::string name() const {
        return this->source_.name();
    }

private:
    void run_();
    /// Sleeps up to `delay_ms`, waking early on a stop.
    /// @return false if the thread should exit.
    bool wait_(int64_t delay_ms);

    AudioSource& source_;
    StreamFormat format_;
    Writer writer_;

    std::thread thread_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    bool stopping_{false};  ///< guarded by stop_mutex_
};

/// Which backend an --input spec named.
enum class SourceBackend {
    Null,       ///< silence; needs no device
    Tone,       ///< a test tone; needs no device
    Alsa,       ///< an ALSA capture PCM, named by InputSpec::device
    Pulse,      ///< a PulseAudio source, or the server's default with no device
    PipeWire,   ///< a PipeWire source node, or the graph's default with no device
    CoreAudio,  ///< a CoreAudio device by index or name, or this host's default input if empty
};

/// An --input spec resolved into a backend and the device to hand it.
struct InputSpec {
    SourceBackend backend{SourceBackend::Null};
    std::string device;  ///< empty for the device-less sources and a backend's default
};

/// Resolves an --input spec without opening anything, by -o's rules: a bare backend name,
/// `<backend>:<device>` split on the first colon, or else an ALSA PCM name.
/// @return true if `spec` named something this build can capture from.
bool resolve_input_spec(const std::string& spec, InputSpec& out, std::string& error);

/// The backend prefixes --input has in this build, e.g. "null, tone, alsa, pulse".
std::string input_backend_list();

/// True if a bare `--input <pcm>` reaches the ALSA PCM of that name.
bool input_pcm_is_reachable(const std::string& pcm);

/// Settles `format` for a sound server, which converts: only a format no stream can carry changes.
void settle_server_capture_format(StreamFormat& format);

/// Builds the source named by --input and negotiates its capture format.
/// @param format In: the format to prefer. Out: the one the device will capture.
/// @return The source, or nullptr with `error` set.
std::unique_ptr<AudioSource> make_audio_source(const std::string& spec, StreamFormat& format,
                                               std::string& error);

/// Prints the specs make_audio_source() accepts, as -l's input section.
void print_capture_devices(std::FILE* out);

}  // namespace sendspin_cli
