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

#include "null_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace sendspin_cli {

namespace {

constexpr double TONE_HZ = 440.0;
constexpr double TONE_AMPLITUDE = 0.25;
constexpr double TWO_PI = 6.283185307179586;
constexpr int64_t US_PER_S = 1000000;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

NullAudioSource::NullAudioSource(NullSourceSignal signal) : signal_(signal) {}

std::string NullAudioSource::name() const {
    return this->signal_ == NullSourceSignal::Tone ? "tone" : "null";
}

bool NullAudioSource::negotiate(StreamFormat& format, std::string& error) {
    const bool depth_ok = std::find(CAPTURE_BIT_DEPTHS.begin(), CAPTURE_BIT_DEPTHS.end(),
                                    format.bit_depth) != CAPTURE_BIT_DEPTHS.end();
    if (!depth_ok || format.channels == 0 || format.sample_rate == 0) {
        error = "the " + this->name() + " input cannot produce " +
                std::to_string(format.sample_rate) + " Hz / " +
                std::to_string(static_cast<unsigned>(format.channels)) + " ch / " +
                std::to_string(static_cast<unsigned>(format.bit_depth)) + "-bit";
        return false;
    }
    return true;
}

bool NullAudioSource::open(const StreamFormat& format) {
    this->format_ = format;
    this->bytes_per_frame_ =
        static_cast<size_t>(format.channels) * (static_cast<size_t>(format.bit_depth) / 8U);
    this->start_us_ = now_us();
    this->frames_ = 0;
    return this->bytes_per_frame_ != 0 && format.sample_rate != 0;
}

int NullAudioSource::read(uint8_t* data, size_t length, uint32_t timeout_ms,
                          int64_t& capture_time_us) {
    if (this->bytes_per_frame_ == 0) {
        return -1;
    }
    const uint64_t wanted = length / this->bytes_per_frame_;
    const int64_t rate = this->format_.sample_rate;
    const auto time_of = [this, rate](uint64_t frame) {
        return this->start_us_ + static_cast<int64_t>(frame) * US_PER_S / rate;
    };

    // Wait for the whole read to have "happened", as a device would.
    const int64_t wait_us = std::min<int64_t>(time_of(this->frames_ + wanted) - now_us(),
                                              static_cast<int64_t>(timeout_ms) * 1000);
    if (wait_us > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
    }
    const int64_t elapsed_frames = (now_us() - this->start_us_) * rate / US_PER_S;
    const int64_t due = elapsed_frames - static_cast<int64_t>(this->frames_);
    if (due <= 0) {
        return 0;
    }
    const uint64_t frames = std::min<uint64_t>(wanted, static_cast<uint64_t>(due));

    capture_time_us = time_of(this->frames_);
    std::memset(data, 0, frames * this->bytes_per_frame_);
    if (this->signal_ == NullSourceSignal::Tone) {
        const size_t bytes_per_sample = this->format_.bit_depth / 8U;
        const double full_scale = std::ldexp(1.0, this->format_.bit_depth - 1) - 1.0;
        uint8_t* cursor = data;
        for (uint64_t i = 0; i < frames; ++i) {
            const double phase = TWO_PI * TONE_HZ * static_cast<double>(this->frames_ + i) /
                                 static_cast<double>(rate);
            const auto sample =
                static_cast<int32_t>(std::lround(std::sin(phase) * TONE_AMPLITUDE * full_scale));
            for (uint8_t channel = 0; channel < this->format_.channels; ++channel) {
                for (size_t byte = 0; byte < bytes_per_sample; ++byte) {
                    *cursor++ = static_cast<uint8_t>(static_cast<uint32_t>(sample) >> (8U * byte));
                }
            }
        }
    }

    this->frames_ += frames;
    return static_cast<int>(frames * this->bytes_per_frame_);
}

void NullAudioSource::close() {
    this->bytes_per_frame_ = 0;
}

}  // namespace sendspin_cli
