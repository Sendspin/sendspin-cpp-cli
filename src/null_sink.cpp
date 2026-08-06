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

#include "null_sink.h"

#include "log.h"

#include <sendspin/client.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace sendspin_cli {

using sendspin::LogLevel;

namespace {

/// Chunk of zeroes used to emit silence while muted, so muting costs no allocation.
constexpr size_t SILENCE_CHUNK = 4096;
constexpr std::array<uint8_t, SILENCE_CHUNK> SILENCE{};

/// Rounds `bytes` down to a whole number of PCM frames, per the AudioSink::write()
/// contract. A frame size of 0 means no stream is configured, so nothing is aligned.
size_t align_to_frame(size_t bytes, size_t bytes_per_frame) {
    if (bytes_per_frame == 0) {
        return bytes;
    }
    return bytes - (bytes % bytes_per_frame);
}

}  // namespace

NullAudioSink::NullAudioSink(NullSinkOutput output) : output_(output) {}

std::string NullAudioSink::name() const {
    return this->output_ == NullSinkOutput::Stdout ? "stdout" : "null";
}

bool NullAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const size_t bytes_per_frame =
        static_cast<size_t>(channels) * (static_cast<size_t>(bits_per_sample) / 8U);
    this->bytes_per_frame_.store(bytes_per_frame);

    cli_log(LogLevel::INFO, "%s sink: stream %u Hz, %u ch, %u-bit (%zu bytes/frame)",
            this->name().c_str(), sample_rate, channels, bits_per_sample, bytes_per_frame);

    if (bytes_per_frame == 0) {
        cli_log(LogLevel::ERROR, "%s sink: refusing stream with a zero-byte frame",
                this->name().c_str());
        return false;
    }
    return true;
}

size_t NullAudioSink::write(const uint8_t* data, size_t length, uint32_t /*timeout_ms*/) {
    const bool to_stdout = this->output_ == NullSinkOutput::Stdout && !this->stdout_failed_.load();
    if (!to_stdout) {
        // Discarding: the whole buffer is "consumed" instantly.
        this->total_bytes_.fetch_add(length);
        return length;
    }

    size_t written = 0;
    if (this->muted_.load()) {
        // Zeroes are silence for the signed-integer PCM the player advertises.
        while (written < length) {
            const size_t chunk = std::min(SILENCE_CHUNK, length - written);
            const size_t n = std::fwrite(SILENCE.data(), 1, chunk, stdout);
            written += n;
            if (n < chunk) {
                break;
            }
        }
    } else {
        written = std::fwrite(data, 1, length, stdout);
    }

    size_t consumed = align_to_frame(written, this->bytes_per_frame_.load());
    if (written < length) {
        // A short write means stdout is gone (a closed downstream pipe). Latch it and
        // degrade to discarding, rather than short-writing on every call from here on.
        cli_log(LogLevel::ERROR,
                "stdout sink: short write (%zu of %zu bytes) -- discarding audio from here on",
                written, length);
        std::clearerr(stdout);
        this->stdout_failed_.store(true);
        consumed = length;
    }

    this->total_bytes_.fetch_add(consumed);
    return consumed;
}

void NullAudioSink::clear() {
    if (this->output_ == NullSinkOutput::Stdout && !this->stdout_failed_.load()) {
        std::fflush(stdout);
    }
    this->bytes_per_frame_.store(0);
}

void NullAudioSink::stop() {
    if (this->output_ == NullSinkOutput::Stdout && !this->stdout_failed_.load()) {
        std::fflush(stdout);
    }
    cli_log(LogLevel::INFO, "%s sink: %zu bytes consumed in total", this->name().c_str(),
            this->total_bytes_.load());
}

void NullAudioSink::set_volume(uint8_t volume) {
    this->volume_.store(volume);
    // Recorded, not applied: per-bit-depth sample scaling belongs to a real backend.
    cli_log(LogLevel::DEBUG, "%s sink: volume now %u (not applied by this sink)",
            this->name().c_str(), volume);
}

void NullAudioSink::set_muted(bool muted) {
    this->muted_.store(muted);
    cli_log(LogLevel::DEBUG, "%s sink: %s", this->name().c_str(), muted ? "muted" : "unmuted");
}

size_t NullAudioSink::total_bytes() const {
    return this->total_bytes_.load();
}

}  // namespace sendspin_cli
