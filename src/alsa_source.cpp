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

#include "alsa_source.h"

#include "alsa_sink.h"
#include "log.h"

#include <chrono>
#include <utility>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// Capture ring and wakeup period; the period is one source-role chunk.
constexpr unsigned int BUFFER_TIME_US = 200000;
constexpr unsigned int PERIOD_TIME_US = 20000;

constexpr int64_t US_PER_S = 1000000;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// ALSA's packed little-endian format for a capture bit depth.
snd_pcm_format_t alsa_format_for(uint8_t bit_depth) {
    switch (bit_depth) {
        case 16:
            return SND_PCM_FORMAT_S16_LE;
        case 24:
            return SND_PCM_FORMAT_S24_3LE;
        case 32:
            return SND_PCM_FORMAT_S32_LE;
        default:
            return SND_PCM_FORMAT_UNKNOWN;
    }
}

}  // namespace

AlsaAudioSource::AlsaAudioSource(std::string device) : device_(std::move(device)) {}

AlsaAudioSource::~AlsaAudioSource() {
    this->close();
}

std::string AlsaAudioSource::name() const {
    return this->device_;
}

void AlsaAudioSource::list_devices(std::FILE* out) {
    list_alsa_pcms(out, SND_PCM_STREAM_CAPTURE, &input_pcm_is_reachable);
}

bool AlsaAudioSource::negotiate(StreamFormat& format, std::string& error) {
    install_alsa_error_handler();

    const std::string device = "ALSA input device '" + this->device_ + "'";
    snd_pcm_t* pcm = nullptr;
    // NONBLOCK, so a busy exclusive device fails instead of blocking startup.
    int err = snd_pcm_open(&pcm, this->device_.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) {
        error = "cannot open " + device + ": " + snd_strerror(err) +
                " -- run with -l to list this host's capture devices";
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(pcm, hw) < 0 ||
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        snd_pcm_close(pcm);
        error = device + " has no interleaved capture configuration -- run with -l to list others";
        return false;
    }

    // Each axis is fixed before the next is tested, so the result is one real configuration.
    std::vector<uint8_t> depths{format.bit_depth};
    depths.insert(depths.end(), CAPTURE_BIT_DEPTHS.begin(), CAPTURE_BIT_DEPTHS.end());
    uint8_t depth = 0;
    for (const uint8_t candidate : depths) {
        const snd_pcm_format_t alsa_format = alsa_format_for(candidate);
        if (alsa_format != SND_PCM_FORMAT_UNKNOWN &&
            snd_pcm_hw_params_set_format(pcm, hw, alsa_format) == 0) {
            depth = candidate;
            break;
        }
    }

    uint8_t channels = 0;
    for (const uint8_t candidate : {format.channels, uint8_t{2}, uint8_t{1}}) {
        if (depth != 0 && candidate != 0 &&
            snd_pcm_hw_params_set_channels(pcm, hw, candidate) == 0) {
            channels = candidate;
            break;
        }
    }
    if (depth != 0 && channels == 0) {
        // A multichannel-only interface: take the device's nearest count.
        unsigned int nearest = 2;
        if (snd_pcm_hw_params_set_channels_near(pcm, hw, &nearest) == 0 && nearest <= UINT8_MAX) {
            channels = static_cast<uint8_t>(nearest);
        }
    }

    unsigned int rate = 0;
    if (channels != 0) {
        for (const unsigned int candidate : {format.sample_rate, 48000U, 44100U}) {
            if (candidate != 0 && snd_pcm_hw_params_set_rate(pcm, hw, candidate, 0) == 0) {
                rate = candidate;
                break;
            }
        }
        if (rate == 0) {
            // Off the usual ladder, e.g. a 16 kHz microphone: take the device's nearest.
            unsigned int nearest = format.sample_rate;
            if (snd_pcm_hw_params_set_rate_near(pcm, hw, &nearest, nullptr) == 0) {
                rate = nearest;
            }
        }
    }
    snd_pcm_close(pcm);

    if (rate == 0) {
        error = device + " captures nothing sendspin-cli can send (16, 24 or 32-bit PCM) -- its "
                         "plughw: form converts; run with -l to list them";
        return false;
    }
    format = {rate, channels, depth};
    return true;
}

bool AlsaAudioSource::open(const StreamFormat& format) {
    install_alsa_error_handler();
    this->close();

    // NONBLOCK and left that way: a blocking open or read could not be interrupted by a stop.
    int err =
        snd_pcm_open(&this->pcm_, this->device_.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa: cannot open input '%s': %s", this->device_.c_str(),
                snd_strerror(err));
        this->pcm_ = nullptr;
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    unsigned int buffer_time = BUFFER_TIME_US;
    unsigned int period_time = PERIOD_TIME_US;
    err = snd_pcm_hw_params_any(this->pcm_, hw);
    if (err >= 0) {
        err = snd_pcm_hw_params_set_access(this->pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    }
    if (err >= 0) {
        err = snd_pcm_hw_params_set_format(this->pcm_, hw, alsa_format_for(format.bit_depth));
    }
    if (err >= 0) {
        err = snd_pcm_hw_params_set_channels(this->pcm_, hw, format.channels);
    }
    if (err >= 0) {
        // Exact: the role announced this rate, and nothing here resamples.
        err = snd_pcm_hw_params_set_rate(this->pcm_, hw, format.sample_rate, 0);
    }
    if (err >= 0) {
        err = snd_pcm_hw_params_set_buffer_time_near(this->pcm_, hw, &buffer_time, nullptr);
    }
    if (err >= 0) {
        err = snd_pcm_hw_params_set_period_time_near(this->pcm_, hw, &period_time, nullptr);
    }
    if (err >= 0) {
        err = snd_pcm_hw_params(this->pcm_, hw);
    }
    if (err >= 0) {
        err = snd_pcm_prepare(this->pcm_);
    }
    if (err >= 0) {
        // Capture does not start on its own, and snd_pcm_wait() never wakes on a stopped PCM.
        err = snd_pcm_start(this->pcm_);
    }
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa: input '%s' rejected %u Hz / %u ch / %u-bit: %s",
                this->device_.c_str(), format.sample_rate, format.channels, format.bit_depth,
                snd_strerror(err));
        this->close();
        return false;
    }

    this->rate_ = format.sample_rate;
    this->bytes_per_frame_ =
        static_cast<size_t>(format.channels) * (static_cast<size_t>(format.bit_depth) / 8U);
    cli_log(LogLevel::INFO, "alsa: input '%s' open at %u Hz, %u ch, %u-bit", this->device_.c_str(),
            format.sample_rate, format.channels, format.bit_depth);
    return true;
}

bool AlsaAudioSource::recover_(int err) {
    if (err == -EINTR || err == -EAGAIN) {
        return true;
    }
    // Not snd_pcm_recover(): its resume loop is unbounded and would hold up a stop.
    int recovered = snd_pcm_prepare(this->pcm_);
    if (recovered >= 0) {
        recovered = snd_pcm_start(this->pcm_);
    }
    if (recovered < 0) {
        cli_log(LogLevel::ERROR, "alsa: input '%s' is gone (%s)", this->device_.c_str(),
                snd_strerror(err));
        return false;
    }
    cli_log(LogLevel::DEBUG, "alsa: input recovered from %s", snd_strerror(err));
    return true;
}

int AlsaAudioSource::read(uint8_t* data, size_t length, uint32_t timeout_ms,
                          int64_t& capture_time_us) {
    if (this->pcm_ == nullptr) {
        return -1;
    }

    const int ready = snd_pcm_wait(this->pcm_, static_cast<int>(timeout_ms));
    if (ready == 0) {
        return 0;
    }
    if (ready < 0) {
        return this->recover_(ready) ? 0 : -1;
    }

    // Frames queued ahead of this read: the oldest is the first sample returned.
    snd_pcm_sframes_t delay = 0;
    const int64_t now = now_us();
    const bool have_delay = snd_pcm_delay(this->pcm_, &delay) == 0 && delay >= 0;

    const snd_pcm_sframes_t frames =
        snd_pcm_readi(this->pcm_, data, length / this->bytes_per_frame_);
    if (frames < 0) {
        return this->recover_(static_cast<int>(frames)) ? 0 : -1;
    }

    capture_time_us = have_delay ? now - static_cast<int64_t>(delay) * US_PER_S / this->rate_ : 0;
    return static_cast<int>(static_cast<size_t>(frames) * this->bytes_per_frame_);
}

void AlsaAudioSource::close() {
    if (this->pcm_ == nullptr) {
        return;
    }
    snd_pcm_close(this->pcm_);
    this->pcm_ = nullptr;
    this->bytes_per_frame_ = 0;
}

}  // namespace sendspin_cli
