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

#include "portaudio_source.h"

#include "log.h"

#ifdef SENDSPIN_CLI_HAVE_COREAUDIO
#include "coreaudio_permission.h"
#endif

#include <sendspin/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;
using sendspin::SendspinClient;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// Capture ring, in milliseconds; the ALSA source's size.
constexpr uint32_t RING_MS = 200;

/// Ring floor in device buffers: the callback delivers a whole buffer per wakeup.
constexpr double RING_LATENCY_MULTIPLE = 3.0;

/// How long a running stream may go without a callback before read() calls the device lost.
constexpr int64_t STALL_US = 2000000;

constexpr int64_t US_PER_S = 1000000;

PaStreamParameters input_params(PaDeviceIndex device, const PaDeviceInfo* info, uint8_t channels,
                                PaSampleFormat format) {
    PaStreamParameters params = {};
    params.device = device;
    params.channelCount = channels;
    params.sampleFormat = format;
    params.suggestedLatency = info->defaultHighInputLatency;
    params.hostApiSpecificStreamInfo = nullptr;
    return params;
}

}  // namespace

PortAudioSource::PortAudioSource(std::string device) : device_(std::move(device)) {}

PortAudioSource::~PortAudioSource() {
    this->close();
}

std::string PortAudioSource::name() const {
    return this->device_.empty() ? "portaudio" : "portaudio:" + this->device_;
}

void PortAudioSource::list_devices(std::FILE* out) {
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

    const PaDeviceIndex fallback = Pa_GetDefaultInputDevice();
    int listed = 0;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!pa_has_channels(info, PaDirection::Input)) {
            continue;
        }
        if (listed == 0) {
            std::fprintf(out, "  idx  name                                   host API     "
                              " in ch  default rate\n");
        }
        std::fprintf(out, "  %3d  %-38s %-12s %2d ch  %6.0f Hz%s\n", static_cast<int>(i),
                     info->name, pa_host_api_label(info->hostApi), info->maxInputChannels,
                     info->defaultSampleRate, (i == fallback) ? "  (system default)" : "");
        ++listed;
    }

    if (listed == 0) {
        std::fprintf(out, "  (this host has no PortAudio input devices)\n");
    }
}

bool PortAudioSource::negotiate(StreamFormat& format, std::string& error) {
    if (!this->pa_.ok()) {
        error = std::string("cannot initialise PortAudio: ") + this->pa_.error();
        return false;
    }
    PaDeviceIndex device = paNoDevice;
    if (!resolve_pa_device(this->device_, PaDirection::Input, device, error)) {
        return false;
    }
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (info == nullptr || info->maxInputChannels <= 0) {
        error = "PortAudio input device " + std::to_string(device) +
                " went away before it could be asked what it captures -- run with -l to list "
                "this host's input devices";
        return false;
    }
    const std::string label = std::string("PortAudio input device '") + info->name + "'";

#ifdef SENDSPIN_CLI_HAVE_COREAUDIO
    if (microphone_prompt_pending()) {
        // macOS aborts the process, uncatchably, when the app hosting the shell may not prompt.
        cli_log(LogLevel::INFO,
                "portaudio: asking macOS for microphone access -- answer its prompt. If "
                "sendspin-cli aborts here instead, the app hosting this shell cannot ask for the "
                "microphone: start it from Terminal.app");
    }
    // Without it the stream opens, starts and delivers silence.
    if (!request_microphone_access(error)) {
        return false;
    }
#endif

    const uint8_t wanted = (format.channels != 0) ? format.channels : uint8_t{2};
    const auto channels = static_cast<uint8_t>(std::min<int>(wanted, info->maxInputChannels));

    std::vector<uint32_t> rates{(format.sample_rate != 0) ? format.sample_rate : 48000U};
    const auto native_rate = static_cast<uint32_t>(std::lround(info->defaultSampleRate));
    if (native_rate != 0 && native_rate != rates.front()) {
        rates.push_back(native_rate);
    }
    std::vector<uint8_t> depths{format.bit_depth};
    std::copy_if(CAPTURE_BIT_DEPTHS.begin(), CAPTURE_BIT_DEPTHS.end(), std::back_inserter(depths),
                 [&format](uint8_t depth) { return depth != format.bit_depth; });

    for (const uint32_t rate : rates) {
        for (const uint8_t depth : depths) {
            PaSampleFormat sample_format = 0;
            if (std::find(CAPTURE_BIT_DEPTHS.begin(), CAPTURE_BIT_DEPTHS.end(), depth) ==
                    CAPTURE_BIT_DEPTHS.end() ||
                !pa_format_for(depth, sample_format)) {
                continue;
            }
            const PaStreamParameters params = input_params(device, info, channels, sample_format);
            PaError err = paNoError;
            {
                const std::lock_guard<std::mutex> pa_lock(pa_api_mutex());
                err = Pa_IsFormatSupported(&params, nullptr, static_cast<double>(rate));
            }
            if (err == paDeviceUnavailable) {
                error = label + " is in use by something else";
                return false;
            }
            if (err == paFormatIsSupported) {
                format = {rate, channels, depth};
                return true;
            }
        }
    }

    error = label + " captures nothing sendspin-cli can send (16, 24 or 32-bit PCM at " +
            std::to_string(rates.front()) +
            " Hz or its default rate) -- run with -l to list others";
    return false;
}

bool PortAudioSource::open(const StreamFormat& format) {
    this->close();
    if (!this->pa_.ok()) {
        cli_log(LogLevel::ERROR, "portaudio: cannot initialise PortAudio: %s", this->pa_.error());
        return false;
    }

    // Resolved per open, against the device list PortAudio built at startup.
    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, PaDirection::Input, device, error)) {
        cli_log(LogLevel::ERROR, "portaudio: %s", error.c_str());
        return false;
    }
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    PaSampleFormat sample_format = 0;
    if (info == nullptr || !pa_format_for(format.bit_depth, sample_format) ||
        format.channels == 0 || format.sample_rate == 0) {
        cli_log(LogLevel::ERROR, "portaudio: refusing capture at %u Hz / %u ch / %u-bit",
                format.sample_rate, format.channels, format.bit_depth);
        return false;
    }
    if (format.channels > info->maxInputChannels) {
        cli_log(LogLevel::ERROR, "portaudio: input '%s' has %d channels, so it cannot capture %u",
                info->name, info->maxInputChannels, format.channels);
        return false;
    }

    // Everything the callback reads is set before the stream starts.
    this->rate_ = format.sample_rate;
    this->bytes_per_frame_ =
        static_cast<size_t>(format.channels) * (static_cast<size_t>(format.bit_depth) / 8U);
    this->frames_captured_ = 0;
    this->frames_read_ = 0;
    this->epoch_us_.store(0);
    this->last_callback_us_.store(0);
    this->frames_dropped_.store(0);

    const PaStreamParameters params = input_params(device, info, format.channels, sample_format);
    std::unique_lock<std::mutex> pa_lock(pa_api_mutex());
    PaError err =
        Pa_OpenStream(&this->stream_, &params, nullptr, format.sample_rate,
                      paFramesPerBufferUnspecified, paNoFlag, &PortAudioSource::pa_callback, this);
    if (err != paNoError) {
        pa_lock.unlock();
        cli_log(LogLevel::ERROR,
                "portaudio: input '%s' would not open at %u Hz / %u ch / %u-bit: %s", info->name,
                format.sample_rate, format.channels, format.bit_depth, Pa_GetErrorText(err));
        this->stream_ = nullptr;
        this->close();
        return false;
    }

    double latency_s = 0.0;
    const PaStreamInfo* stream_info = Pa_GetStreamInfo(this->stream_);
    if (stream_info != nullptr) {
        latency_s = stream_info->inputLatency;
    }
    const size_t ring_frames =
        std::max(static_cast<size_t>(format.sample_rate) * RING_MS / 1000U,
                 static_cast<size_t>(latency_s * format.sample_rate * RING_LATENCY_MULTIPLE));
    // The spare byte the ring keeps to tell full from empty.
    this->ring_.reset((ring_frames * this->bytes_per_frame_) + 1);

    this->opened_us_ = SendspinClient::now_us();
    err = Pa_StartStream(this->stream_);
    pa_lock.unlock();
    if (err != paNoError) {
        cli_log(LogLevel::ERROR, "portaudio: input '%s' would not start: %s", info->name,
                Pa_GetErrorText(err));
        this->close();
        return false;
    }

    cli_log(LogLevel::INFO, "portaudio: input '%s' (%s) open at %u Hz, %u ch, %u-bit", info->name,
            this->name().c_str(), format.sample_rate, format.channels, format.bit_depth);
    return true;
}

int PortAudioSource::read(uint8_t* data, size_t length, uint32_t timeout_ms,
                          int64_t& capture_time_us) {
    if (this->stream_ == nullptr) {
        return -1;
    }

    {
        std::unique_lock<std::mutex> lock(this->wait_mutex_);
        this->data_ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
            return this->ring_.available() >= this->bytes_per_frame_;
        });
    }

    if (this->ring_.available() < this->bytes_per_frame_) {
        if (Pa_IsStreamActive(this->stream_) != 1) {
            cli_log(LogLevel::ERROR, "portaudio: input '%s' stopped", this->name().c_str());
            return -1;
        }
        // Some host APIs keep a stream on a vanished device active but never call back again.
        const int64_t last_us = this->last_callback_us_.load();
        if (SendspinClient::now_us() - (last_us != 0 ? last_us : this->opened_us_) > STALL_US) {
            cli_log(LogLevel::ERROR, "portaudio: input '%s' stopped delivering audio",
                    this->name().c_str());
            return -1;
        }
        return 0;
    }

    const uint64_t dropped = this->frames_dropped_.exchange(0);
    if (dropped != 0) {
        cli_log(LogLevel::DEBUG, "portaudio: input overran, dropping %llu frames",
                static_cast<unsigned long long>(dropped));
    }

    size_t bytes = std::min(length, this->ring_.available());
    bytes -= bytes % this->bytes_per_frame_;
    if (bytes == 0) {
        return 0;
    }
    // Loaded before the read: the callback publishes it before the frames it covers.
    const int64_t epoch_us = this->epoch_us_.load();
    this->ring_.read(data, bytes);
    capture_time_us = (epoch_us == 0) ? 0
                                      : epoch_us + static_cast<int64_t>(this->frames_read_) *
                                                       US_PER_S / this->rate_;
    this->frames_read_ += bytes / this->bytes_per_frame_;
    return static_cast<int>(bytes);
}

void PortAudioSource::close() {
    if (this->stream_ != nullptr) {
        const std::lock_guard<std::mutex> pa_lock(pa_api_mutex());
        // Both wait the callback out, so nothing below races it.
        Pa_AbortStream(this->stream_);
        Pa_CloseStream(this->stream_);
        this->stream_ = nullptr;
    }
    this->rate_ = 0;
    this->bytes_per_frame_ = 0;
    this->ring_.reset(0);
}

int PortAudioSource::pa_callback(const void* input, void* /*output*/, unsigned long frame_count,
                                 const PaStreamCallbackTimeInfo* time_info,
                                 PaStreamCallbackFlags /*status_flags*/, void* user_data) {
    const int64_t entered_us = SendspinClient::now_us();

    auto* self = static_cast<PortAudioSource*>(user_data);
    self->last_callback_us_.store(entered_us);
    const size_t bytes_per_frame = self->bytes_per_frame_;
    if (input == nullptr || bytes_per_frame == 0) {
        return paContinue;
    }

    // inputBufferAdcTime is on the stream clock; its offset from currentTime carries onto ours.
    int64_t epoch_us = 0;
    if (time_info != nullptr && time_info->inputBufferAdcTime > 0.0 &&
        time_info->currentTime > 0.0) {
        const int64_t first_us =
            entered_us +
            std::llround((time_info->inputBufferAdcTime - time_info->currentTime) * 1e6);
        epoch_us = first_us - static_cast<int64_t>(self->frames_captured_) * US_PER_S / self->rate_;
    }
    // Before the frames go in, so read() never times a frame against an epoch older than its own.
    self->epoch_us_.store(epoch_us);

    const size_t captured = static_cast<size_t>(frame_count) * bytes_per_frame;
    // Whole frames only: read() counts frames by integer division.
    size_t room = self->ring_.free_space();
    room -= room % bytes_per_frame;
    const size_t written =
        self->ring_.write(static_cast<const uint8_t*>(input), std::min(captured, room));
    self->frames_captured_ += written / bytes_per_frame;
    if (written < captured) {
        self->frames_dropped_.fetch_add((captured - written) / bytes_per_frame,
                                        std::memory_order_relaxed);
    }

    // Notified without the mutex, keeping the callback lock-free; a missed wakeup is bounded.
    self->data_ready_.notify_one();
    return paContinue;
}

}  // namespace sendspin_cli
