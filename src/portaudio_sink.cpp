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

#include "portaudio_sink.h"

#include "log.h"
#include "pcm_volume.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// Ring floor in device buffers: the callback takes a whole buffer per wakeup.
constexpr double RING_LATENCY_MULTIPLE = 3.0;

/// Absolute floor on the ring, for a device that reports no latency at all.
constexpr size_t MIN_RING_FRAMES = 1024;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// PortAudio format per PROBE_BIT_DEPTHS entry; 24-bit is packed paInt24.
constexpr std::array<PaSampleFormat, PROBE_BIT_DEPTHS.size()> PROBE_FORMATS{paInt8, paInt16,
                                                                            paInt24, paInt32};
constexpr std::array<const char*, PROBE_BIT_DEPTHS.size()> PROBE_FORMAT_NAMES{"paInt8", "paInt16",
                                                                              "paInt24", "paInt32"};

/// Maps the stream's bit depth onto the interleaved little-endian PCM format PortAudio wants.
bool pa_format_for(uint8_t bits_per_sample, PaSampleFormat& format) {
    for (size_t i = 0; i < PROBE_BIT_DEPTHS.size(); ++i) {
        if (PROBE_BIT_DEPTHS[i] == bits_per_sample) {
            format = PROBE_FORMATS[i];
            return true;
        }
    }
    return false;
}

/// True if `value` is a non-empty run of decimal digits.
bool is_device_index(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    return value.find_first_not_of("0123456789") == std::string::npos;
}

bool iequals(const std::string& value, const char* other) {
    if (other == nullptr) {
        return false;
    }
    const size_t len = std::strlen(other);
    if (value.size() != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        // Through unsigned char: tolower() is undefined for negative chars.
        const int a = std::tolower(static_cast<unsigned char>(value[i]));
        const int b = std::tolower(static_cast<unsigned char>(other[i]));
        if (a != b) {
            return false;
        }
    }
    return true;
}

/// True if this device can be played out of at all.
bool is_output_device(const PaDeviceInfo* info) {
    return info != nullptr && info->maxOutputChannels > 0;
}

/// Resolves a PortAudio device spec: empty is the default, digits an index, else a unique name.
/// PortAudio must be initialized.
bool resolve_pa_device(const std::string& device, PaDeviceIndex& out, std::string& error) {
    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        error = std::string("cannot enumerate PortAudio devices: ") + Pa_GetErrorText(count);
        return false;
    }
    if (count == 0) {
        error = "PortAudio found no audio devices on this host at all";
        return false;
    }

    if (device.empty()) {
        const PaDeviceIndex fallback = Pa_GetDefaultOutputDevice();
        if (fallback == paNoDevice) {
            error = "this host has no PortAudio output device at all -- run with -l to see what "
                    "it does have";
            return false;
        }
        out = fallback;
        return true;
    }

    if (is_device_index(device)) {
        // strtoull saturates rather than wrapping into a plausible index.
        const unsigned long long value = std::strtoull(device.c_str(), nullptr, 10);
        if (value >= static_cast<unsigned long long>(count)) {
            // Report the index range, not the count: indices include input-only devices.
            error = "-o portaudio:" + device + ": no device at that index -- indices run 0-" +
                    std::to_string(count - 1) + " here, and -l lists the ones -o can reach";
            return false;
        }
        const auto index = static_cast<PaDeviceIndex>(value);
        if (!is_output_device(Pa_GetDeviceInfo(index))) {
            error = "-o portaudio:" + device +
                    ": that device has no output channels -- run with -l, which lists only the "
                    "ones -o can reach";
            return false;
        }
        out = index;
        return true;
    }

    std::vector<PaDeviceIndex> matches;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (is_output_device(info) && iequals(device, info->name)) {
            matches.push_back(i);
        }
    }
    if (matches.empty()) {
        error = "-o portaudio:" + device +
                ": no output device by that name -- run with -l to list them";
        return false;
    }
    if (matches.size() > 1) {
        std::string indices;
        for (const PaDeviceIndex index : matches) {
            if (!indices.empty()) {
                indices += ", ";
            }
            indices += std::to_string(index);
        }
        error = "-o portaudio:" + device + ": " + std::to_string(matches.size()) +
                " output devices share that name (indices " + indices +
                ") -- name the one you mean by index instead";
        return false;
    }

    out = matches.front();
    return true;
}

/// Asks whether `device` takes this exact format; on ALSA hosts each call opens the PCM.
PaError pa_accepts(PaDeviceIndex device, const PaDeviceInfo* info, uint32_t rate,
                   PaSampleFormat format, int channels) {
    PaStreamParameters params = {};
    params.device = device;
    params.channelCount = channels;
    params.sampleFormat = format;
    // The latency open_stream_() asks for.
    params.suggestedLatency = info->defaultHighOutputLatency;
    params.hostApiSpecificStreamInfo = nullptr;
    return Pa_IsFormatSupported(nullptr, &params, static_cast<double>(rate));
}

/// Why a probe could not describe a device, mirroring AlsaAudioSink's statuses.
enum class ProbeStatus {
    Ok,
    Busy,      ///< paDeviceUnavailable: the device is there, something else holds it
    NoDevice,  ///< the index does not resolve, or has no output channels
};

struct ProbeResult {
    ProbeStatus status{ProbeStatus::Ok};
    /// Empty on anything but Ok.
    SinkCapabilities caps;
};

/// Asks one device what it will take, without opening a stream; axes are independent.
ProbeResult probe_capabilities(PaDeviceIndex device) {
    ProbeResult result;
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (info == nullptr || info->maxOutputChannels <= 0) {
        result.status = ProbeStatus::NoDevice;
        return result;
    }

    // One rate x depth pass at the channel count we would use: each call may open the device.
    const int probe_channels = std::min(2, info->maxOutputChannels);
    bool accepted[PROBE_RATES.size()][PROBE_BIT_DEPTHS.size()] = {};
    for (size_t r = 0; r < PROBE_RATES.size(); ++r) {
        for (size_t d = 0; d < PROBE_BIT_DEPTHS.size(); ++d) {
            const PaError err =
                pa_accepts(device, info, PROBE_RATES[r], PROBE_FORMATS[d], probe_channels);
            if (err == paDeviceUnavailable) {
                // Held elsewhere: stop and report "in use", not "supports nothing".
                result.status = ProbeStatus::Busy;
                result.caps = {};
                return result;
            }
            accepted[r][d] = (err == paFormatIsSupported);
        }
    }

    for (size_t r = 0; r < PROBE_RATES.size(); ++r) {
        if (std::any_of(std::begin(accepted[r]), std::end(accepted[r]),
                        [](bool ok) { return ok; })) {
            result.caps.rates.push_back(PROBE_RATES[r]);
        }
    }
    for (size_t d = 0; d < PROBE_BIT_DEPTHS.size(); ++d) {
        for (size_t r = 0; r < PROBE_RATES.size(); ++r) {
            if (accepted[r][d]) {
                result.caps.bit_depths.push_back(PROBE_BIT_DEPTHS[d]);
                break;
            }
        }
    }

    // The channel axis reuses a depth the grid accepted.
    if (result.caps.bit_depths.empty()) {
        return result;
    }
    PaSampleFormat probe_format = PROBE_FORMATS[0];
    for (size_t d = 0; d < PROBE_BIT_DEPTHS.size(); ++d) {
        if (PROBE_BIT_DEPTHS[d] == result.caps.bit_depths.front()) {
            probe_format = PROBE_FORMATS[d];
        }
    }
    const auto probe_rate = static_cast<uint32_t>(info->defaultSampleRate);
    for (const uint8_t count : PROBE_CHANNELS) {
        if (count > info->maxOutputChannels) {
            break;  // PROBE_CHANNELS ascends, so nothing after this fits either
        }
        if (pa_accepts(device, info, probe_rate, probe_format, count) == paFormatIsSupported) {
            result.caps.channels.push_back(count);
        }
    }
    return result;
}

/// Prints what one device will take, indented under it in -l.
void print_device_capabilities(std::FILE* out, PaDeviceIndex device) {
    const ProbeResult result = probe_capabilities(device);
    switch (result.status) {
        case ProbeStatus::Busy:
            std::fprintf(out, "      (in use -- capabilities unknown)\n");
            return;
        case ProbeStatus::NoDevice:
            std::fprintf(out, "      (cannot query: the device went away)\n");
            return;
        case ProbeStatus::Ok:
            break;
    }
    print_sink_capabilities(out, result.caps, PROBE_FORMAT_NAMES);
}

}  // namespace


PortAudioGuard::PortAudioGuard() : err_(Pa_Initialize()) {}

PortAudioGuard::~PortAudioGuard() {
    if (this->err_ == paNoError) {
        Pa_Terminate();
    }
}

bool PortAudioGuard::ok() const {
    return this->err_ == paNoError;
}

const char* PortAudioGuard::error() const {
    return Pa_GetErrorText(this->err_);
}

bool PortAudioGuard::reinitialize() {
    if (this->err_ == paNoError) {
        Pa_Terminate();
    }
    // Overwritten so ok() and the destructor track the live state.
    this->err_ = Pa_Initialize();
    return this->err_ == paNoError;
}


PortAudioSink::PortAudioSink(std::string device, uint32_t buffer_ms)
    : device_(std::move(device)), buffer_ms_(buffer_ms) {
    if (!this->pa_.ok()) {
        // Degrade to discarding; configure() will fail until PortAudio comes back.
        cli_log(LogLevel::ERROR, "portaudio: cannot initialise PortAudio: %s", this->pa_.error());
    }
}

PortAudioSink::~PortAudioSink() {
    // A sink destroyed without stop() must still close its stream before pa_ terminates.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->close_stream_();
}

std::string PortAudioSink::name() const {
    return this->device_.empty() ? "portaudio" : "portaudio:" + this->device_;
}

bool PortAudioSink::probe(const std::string& device, std::string& error) {
    const PortAudioGuard pa;
    if (!pa.ok()) {
        error = std::string("cannot initialise PortAudio: ") + pa.error();
        return false;
    }

    PaDeviceIndex index = paNoDevice;
    return resolve_pa_device(device, index, error);
}

SinkCapabilities PortAudioSink::capabilities() const {
    // Describes the default device at startup; a later default switch shows up as a refusal.
    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, device, error)) {
        cli_log(LogLevel::DEBUG, "portaudio: %s -- advertising everything sendspin-cli can emit",
                error.c_str());
        return SinkCapabilities::permissive();
    }

    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    // A named local: name() returns by value, so its c_str() would dangle below.
    const std::string fallback_name = this->name();
    const char* device_name = (info != nullptr) ? info->name : fallback_name.c_str();
    ProbeResult result = probe_capabilities(device);
    if (result.status != ProbeStatus::Ok) {
        cli_log(LogLevel::DEBUG,
                "portaudio: could not probe '%s' -- advertising everything sendspin-cli "
                "can emit",
                device_name);
        return SinkCapabilities::permissive();
    }
    cli_log(LogLevel::DEBUG, "portaudio: capabilities probed from '%s'", device_name);
    return result.caps;
}

void PortAudioSink::list_devices(std::FILE* out) {
    const PortAudioGuard pa;
    if (!pa.ok()) {
        std::fprintf(out, "  (cannot initialise PortAudio: %s)\n", pa.error());
        return;
    }

    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        std::fprintf(out, "  (cannot enumerate PortAudio devices: %s)\n", Pa_GetErrorText(count));
        return;
    }

    const PaDeviceIndex fallback = Pa_GetDefaultOutputDevice();
    int listed = 0;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!is_output_device(info)) {
            continue;  // input-only: -o cannot reach it, so listing it would mislead
        }
        // Header inside the loop, so a host with no output devices gets only the note below.
        if (listed == 0) {
            std::fprintf(out, "  idx  name                                   host API     "
                              "out ch  default rate\n");
        }
        const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
        std::fprintf(out, "  %3d  %-38s %-12s %2d ch  %6.0f Hz%s\n", static_cast<int>(i),
                     info->name, (host != nullptr) ? host->name : "(unknown host API)",
                     info->maxOutputChannels, info->defaultSampleRate,
                     (i == fallback) ? "  (system default)" : "");
        print_device_capabilities(out, i);
        ++listed;
    }

    if (listed == 0) {
        std::fprintf(out, "  (this host has no PortAudio output devices)\n");
    }
}

bool PortAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    if (this->stopping_.load()) {
        // A stream start racing shutdown; never clear stopping_ here.
        cli_log(LogLevel::DEBUG, "portaudio: ignoring a stream start during shutdown");
        return false;
    }

    // Before anything can fail: recovery reopens at this format.
    this->last_format_ = {sample_rate, channels, bits_per_sample};

    // Resolved per stream, so a bare -o portaudio follows the host's default.
    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, device, error)) {
        cli_log(LogLevel::ERROR, "portaudio: %s", error.c_str());
        this->failed_.store(true);
        return false;
    }

    if (this->stream_ != nullptr && this->device_index_ == device && this->rate_ == sample_rate &&
        this->channels_ == channels && this->bits_ == bits_per_sample) {
        // Same device and format: restart from an empty ring instead of reopening.
        if (this->restart_stream_()) {
            this->recovery_.reset();
            cli_log(LogLevel::DEBUG, "portaudio: reusing the open stream at %u Hz, %u ch, %u-bit",
                    sample_rate, channels, bits_per_sample);
            return true;
        }
        cli_log(LogLevel::WARN, "portaudio: could not restart the stream -- reopening");
    }

    this->close_stream_();
    if (!this->open_stream_(device, sample_rate, channels, bits_per_sample)) {
        this->failed_.store(true);
        return false;
    }
    // Refill the recovery budget only once a stream is really running.
    this->recovery_.reset();
    return true;
}

size_t PortAudioSink::write(const uint8_t* data, size_t length, uint32_t timeout_ms) {
    if (data == nullptr || length == 0) {
        return 0;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(this->mutex_);

    if ((!this->stream_alive_() || this->bytes_per_frame_ == 0) && !this->reopen_in_place_()) {
        // Discard rather than return 0 forever, which would spin the sync task.
        if (!this->failed_.exchange(true)) {
            cli_log(LogLevel::ERROR,
                    "portaudio: '%s' is not playing -- discarding audio until a stream "
                    "reconfigures it",
                    this->name().c_str());
        }
        // Frame-aligned via last_format_ when a failed reopen has zeroed bytes_per_frame_.
        const size_t frame = (this->bytes_per_frame_ != 0)
                                 ? this->bytes_per_frame_
                                 : static_cast<size_t>(this->last_format_.channels) *
                                       (static_cast<size_t>(this->last_format_.bit_depth) / 8U);
        return (frame == 0) ? length : length - (length % frame);
    }

    const size_t bytes_per_frame = this->bytes_per_frame_;
    // Re-checked after every wait: waiting drops the mutex.
    const uint64_t generation = this->stream_generation_;
    const size_t usable = length - (length % bytes_per_frame);
    if (usable == 0) {
        return 0;
    }

    size_t done = 0;
    while (done < usable) {
        if (this->stopping_.load()) {
            break;
        }

        const size_t remaining = usable - done;
        size_t room = this->ring_.free_space();
        if (room < remaining) {
            // Whole frames only: the callback counts frames by integer division.
            room -= room % bytes_per_frame;
        }
        if (room > 0) {
            const size_t written = this->ring_.write(data + done, std::min(remaining, room));
            if (written == 0) {
                break;  // the ring reported room and then took none; nothing to gain by spinning
            }
            done += written;
            continue;
        }

        if (!this->space_available_.wait_until(lock, deadline, [this, bytes_per_frame, generation] {
                return this->ring_.free_space() >= bytes_per_frame || this->stopping_.load() ||
                       this->stream_ == nullptr || this->stream_generation_ != generation;
            })) {
            break;  // out of time; the caller gets a short write and comes back
        }
        if (this->stream_ == nullptr || this->stream_generation_ != generation) {
            // The stream changed while unlocked; report only what landed.
            break;
        }
    }

    return done;
}

void PortAudioSink::clear() {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    // A flush ends a parked write()'s stream.
    ++this->stream_generation_;

    // Do not snap current_multiplier_: the callback keeps running through a flush.

    if (this->stream_alive_()) {
        // The consumer owns read_pos_, so it drains on its next read.
        this->ring_.request_clear();
        return;
    }
    // No consumer running, so drop now rather than leave a clear pending for the next stream.
    this->ring_.drop();
}

void PortAudioSink::stop() {
    // Before the mutex, so a blocked write() bails out.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    // Before the early return, so a write() after shutdown attempts nothing.
    this->last_format_ = {};
    if (this->stream_ == nullptr) {
        return;
    }
    this->close_stream_();
    cli_log(LogLevel::INFO, "portaudio: '%s' closed", this->name().c_str());
}

void PortAudioSink::poll(int64_t now_ms) {
    // Unlocked fast path, and checked before rescan_due() so shutdown never burns the rescan.
    if (!this->recovery_.pending() || this->stopping_.load()) {
        return;
    }

    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->last_format_.sample_rate == 0) {
        return;  // nothing was ever configured, so there is nothing to reopen at
    }
    if (!this->recovery_.rescan_due(now_ms)) {
        return;
    }
    // Reported up front and always as recovered: a device-list rebuild is one-shot.
    this->recovery_.rescan_done(true);

    const StreamFormat format = this->last_format_;
    // Close first: Pa_Terminate() with a stream open is undefined and invalidates every index.
    this->close_stream_();

    if (!this->pa_.reinitialize()) {
        // PortAudio is down, so the sink is inert until it comes back.
        cli_log(LogLevel::ERROR, "portaudio: could not restart PortAudio to look for '%s': %s",
                this->name().c_str(), this->pa_.error());
        return;
    }

    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, device, error)) {
        cli_log(LogLevel::WARN,
                "portaudio: '%s' is still gone after a device rescan -- discarding until the "
                "next stream (%s)",
                this->name().c_str(), error.c_str());
        return;
    }
    if (!this->open_stream_(device, format.sample_rate, format.channels, format.bit_depth)) {
        return;  // open_stream_() has already said why, once
    }
    if (this->stopping_.load()) {
        // stop() can land during the slow cycle above; release the device now.
        this->close_stream_();
        return;
    }
    // Name the device found: a rescan can renumber indices.
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    cli_log(LogLevel::INFO, "portaudio: '%s' is back after a device rescan, on '%s'",
            this->name().c_str(), (info != nullptr) ? info->name : "(unknown device)");
}

void PortAudioSink::set_volume(uint8_t volume) {
    this->volume_.store(volume > 100 ? 100 : volume);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "portaudio: volume now %u", this->volume_.load());
}

void PortAudioSink::set_muted(bool muted) {
    this->muted_.store(muted);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "portaudio: %s", muted ? "muted" : "unmuted");
}

bool PortAudioSink::open_stream_(PaDeviceIndex device, uint32_t sample_rate, uint8_t channels,
                                 uint8_t bits_per_sample) {
    PaSampleFormat format = 0;
    if (!pa_format_for(bits_per_sample, format)) {
        cli_log(LogLevel::ERROR, "portaudio: unsupported bit depth %u", bits_per_sample);
        return false;
    }
    if (channels == 0 || sample_rate == 0) {
        cli_log(LogLevel::ERROR, "portaudio: refusing stream with %u ch at %u Hz", channels,
                sample_rate);
        return false;
    }

    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (info == nullptr) {
        cli_log(LogLevel::ERROR, "portaudio: device %d disappeared before it could be opened",
                static_cast<int>(device));
        return false;
    }
    if (channels > info->maxOutputChannels) {
        cli_log(LogLevel::ERROR, "portaudio: '%s' has %d output channels, so it cannot play %u",
                info->name, info->maxOutputChannels, channels);
        return false;
    }

    // Format fields and generation change before Pa_StartStream(), while the callback cannot run.
    ++this->stream_generation_;
    this->device_index_ = device;
    this->rate_ = sample_rate;
    this->channels_ = channels;
    this->bits_ = bits_per_sample;
    this->bytes_per_frame_ =
        static_cast<size_t>(channels) * (static_cast<size_t>(bits_per_sample) / 8U);
    this->stream_rate_ = static_cast<double>(sample_rate);
    this->ramp_step_ = volume_ramp_step(sample_rate);
    // Open at the target gain, never ramping up to a restored volume.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    PaStreamParameters output_params = {};
    output_params.device = device;
    output_params.channelCount = channels;
    output_params.sampleFormat = format;
    // High latency, as upstream does: a network player values buffer over reaction time.
    output_params.suggestedLatency = info->defaultHighOutputLatency;
    output_params.hostApiSpecificStreamInfo = nullptr;

    PaError err =
        Pa_OpenStream(&this->stream_, nullptr, &output_params, sample_rate,
                      paFramesPerBufferUnspecified, paNoFlag, &PortAudioSink::pa_callback, this);
    if (err != paNoError) {
        cli_log(LogLevel::ERROR, "portaudio: '%s' would not open at %u Hz / %u ch / %u-bit: %s",
                info->name, sample_rate, channels, bits_per_sample, Pa_GetErrorText(err));
        this->stream_ = nullptr;
        this->close_stream_();
        return false;
    }

    // What PortAudio really gave us, written before Pa_StartStream().
    double device_latency_s = 0.0;
    const PaStreamInfo* stream_info = Pa_GetStreamInfo(this->stream_);
    if (stream_info != nullptr) {
        if (stream_info->sampleRate > 0.0) {
            this->stream_rate_ = stream_info->sampleRate;
        }
        device_latency_s = stream_info->outputLatency;
    }
    const size_t capacity = this->ring_capacity_(device_latency_s);
    this->ring_.reset(capacity);

    err = Pa_StartStream(this->stream_);
    if (err != paNoError) {
        cli_log(LogLevel::ERROR, "portaudio: '%s' would not start: %s", info->name,
                Pa_GetErrorText(err));
        this->close_stream_();
        return false;
    }

    this->failed_.store(false);
    cli_log(LogLevel::INFO,
            "portaudio: '%s' (%s) open at %u Hz, %u ch, %u-bit (%zu bytes/frame, "
            "%zu-byte ring, %.1f ms device latency)",
            info->name, this->name().c_str(), sample_rate, channels, bits_per_sample,
            this->bytes_per_frame_, capacity, device_latency_s * 1000.0);
    return true;
}

void PortAudioSink::close_stream_() {
    if (this->stream_ != nullptr) {
        // Abort, not stop: stopping waits for the buffer to play out. Both wait out the callback.
        Pa_AbortStream(this->stream_);
        Pa_CloseStream(this->stream_);
        this->stream_ = nullptr;
    }

    ++this->stream_generation_;
    this->device_index_ = paNoDevice;
    this->rate_ = 0;
    this->channels_ = 0;
    this->bits_ = 0;
    this->bytes_per_frame_ = 0;
    this->stream_rate_ = 0.0;
    this->ramp_step_ = 0;
    this->ring_.reset(0);
    // Wake a write() parked on the old stream.
    this->space_available_.notify_all();
    // Never clear stopping_ here: a mid-stream configure() would un-latch a shutdown.
}

bool PortAudioSink::restart_stream_() {
    PaError err = Pa_AbortStream(this->stream_);
    if (err != paNoError && err != paStreamIsStopped) {
        cli_log(LogLevel::DEBUG, "portaudio: cannot abort the stream: %s", Pa_GetErrorText(err));
        return false;
    }

    // A new stream: a write() parked on the old one holds dropped audio.
    ++this->stream_generation_;

    // The callback has stopped, so dropping the ring from this side is safe here.
    this->ring_.drop();
    // Snap the gain too, so the next stream starts at its target.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    err = Pa_StartStream(this->stream_);
    if (err != paNoError) {
        cli_log(LogLevel::DEBUG, "portaudio: cannot restart the stream: %s", Pa_GetErrorText(err));
        return false;
    }

    this->failed_.store(false);
    return true;
}

bool PortAudioSink::reopen_in_place_() {
    if (this->stream_ == nullptr || this->stopping_.load() || this->last_format_.sample_rate == 0) {
        // Only a stream that ran and died: a refused format has a null stream_ and needs no rescan.
        return false;
    }
    if (!this->recovery_.reopen_due()) {
        return false;
    }

    // Resolve before closing, so a failure leaves write() its frame size.
    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, device, error)) {
        cli_log(LogLevel::WARN, "portaudio: cannot reopen '%s': %s", this->name().c_str(),
                error.c_str());
        this->recovery_.reopen_done(false);
        return false;
    }

    const StreamFormat format = this->last_format_;
    this->close_stream_();
    if (!this->open_stream_(device, format.sample_rate, format.channels, format.bit_depth)) {
        this->recovery_.reopen_done(false);  // open_stream_() has already said why, once
        return false;
    }
    this->recovery_.reopen_done(true);

    if (this->stopping_.load()) {
        // stop() can land during Pa_OpenStream(); release the device now.
        this->close_stream_();
        return false;
    }

    cli_log(LogLevel::INFO, "portaudio: '%s' recovered without waiting for the next stream",
            this->name().c_str());
    return true;
}

bool PortAudioSink::stream_alive_() const {
    if (this->stream_ == nullptr) {
        return false;
    }
    // Inactive means the device went away; true only while pa_callback() never returns paComplete.
    return Pa_IsStreamActive(this->stream_) == 1;
}

size_t PortAudioSink::ring_capacity_(double device_latency_s) const {
    const auto frames_by_time =
        static_cast<size_t>((static_cast<int64_t>(this->rate_) * this->buffer_ms_) / 1000);
    const auto frames_by_latency =
        static_cast<size_t>(device_latency_s * this->stream_rate_ * RING_LATENCY_MULTIPLE);
    const size_t frames = std::max({frames_by_time, frames_by_latency, MIN_RING_FRAMES});
    // Log which floor overrode --buffer-ms.
    if (frames > frames_by_time) {
        cli_log(LogLevel::DEBUG,
                "portaudio: --buffer-ms %u is %zu frames at %u Hz, below the %s floor of "
                "%zu frames -- using the floor",
                this->buffer_ms_, frames_by_time, this->rate_,
                (frames_by_latency >= MIN_RING_FRAMES) ? "device-latency" : "minimum-ring", frames);
    }
    // The spare byte the ring keeps to tell full from empty, so `frames` really do fit.
    return (frames * this->bytes_per_frame_) + 1;
}

void PortAudioSink::update_target_multiplier_() {
    // Only the target moves; the callback advances current_multiplier_.
    this->target_multiplier_.store(q32_gain_for(this->volume_.load(), this->muted_.load()),
                                   std::memory_order_relaxed);
}

int PortAudioSink::pa_callback(const void* /*input*/, void* output, unsigned long frame_count,
                               const PaStreamCallbackTimeInfo* time_info,
                               PaStreamCallbackFlags /*status_flags*/, void* user_data) {
    const int64_t entered_us = now_us();

    auto* self = static_cast<PortAudioSink*>(user_data);
    auto* out = static_cast<uint8_t*>(output);
    const size_t bytes_requested = frame_count * self->bytes_per_frame_;

    const size_t bytes_read = self->ring_.read(out, bytes_requested);

    // The unity fast path also requires current == target, or it would skip a ramp.
    const uint64_t target = self->target_multiplier_.load(std::memory_order_relaxed);
    const uint64_t current = self->current_multiplier_;
    if (current != target || target != Q32_ONE) {
        // Advanced by every frame handed over, zero-filled tail included: silence is played too.
        self->current_multiplier_ =
            apply_volume_ramp(out, bytes_requested, self->bits_ / 8U, self->channels_, current,
                              target, self->ramp_step_);
    }

    // Notified without the mutex, keeping the callback lock-free; a missed wakeup is bounded.
    self->space_available_.notify_one();

    // outputBufferDacTime plus this buffer's duration is when its last frame leaves the DAC.
    if (bytes_read > 0 && self->on_frames_played) {
        const auto frames_played = static_cast<uint32_t>(bytes_read / self->bytes_per_frame_);
        const double dac_offset_s = (time_info->outputBufferDacTime - time_info->currentTime) +
                                    (static_cast<double>(frames_played) / self->stream_rate_);
        const int64_t finish_us =
            entered_us + static_cast<int64_t>(std::llround(dac_offset_s * 1e6));
        self->on_frames_played(frames_played, finish_us);
    }

    return paContinue;
}

}  // namespace sendspin_cli
