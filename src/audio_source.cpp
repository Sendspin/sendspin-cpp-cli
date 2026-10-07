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

#include "audio_source.h"

#include "log.h"
#include "null_source.h"
#include "sink_recovery.h"

#ifdef SENDSPIN_CLI_HAVE_ALSA
#include "alsa_source.h"
#endif

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// Audio per read(); matches the source role's default chunk.
constexpr uint32_t READ_MS = 20;

/// Longest one read() may block, bounding how long a stop waits for the thread.
constexpr uint32_t READ_TIMEOUT_MS = 100;

/// Output backends --input knows by name but cannot capture through.
constexpr const char* UNSUPPORTED_BACKENDS[] = {"coreaudio", "portaudio", "pulse", "pipewire"};

}  // namespace

SourceCapture::SourceCapture(AudioSource& source, StreamFormat format, Writer writer)
    : source_(source), format_(format), writer_(std::move(writer)) {}

SourceCapture::~SourceCapture() {
    this->on_streaming_stopped();
}

void SourceCapture::on_streaming_started() {
    if (this->thread_.joinable()) {
        return;
    }
    log_line(LogLevel::INFO, LOG_TAG, "Input stream opened: capturing from '%s'",
             this->source_.name().c_str());
    this->stopping_ = false;
    this->thread_ = std::thread([this] { this->run_(); });
}

void SourceCapture::on_streaming_stopped() {
    if (!this->thread_.joinable()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(this->stop_mutex_);
        this->stopping_ = true;
    }
    this->stop_cv_.notify_all();
    this->thread_.join();
    log_line(LogLevel::INFO, LOG_TAG, "Input stream closed: released '%s'",
             this->source_.name().c_str());
}

bool SourceCapture::wait_(int64_t delay_ms) {
    std::unique_lock<std::mutex> lock(this->stop_mutex_);
    return !this->stop_cv_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                                    [this] { return this->stopping_; });
}

void SourceCapture::run_() {
    const size_t bytes_per_frame = static_cast<size_t>(this->format_.channels) *
                                   (static_cast<size_t>(this->format_.bit_depth) / 8U);
    const size_t frames = static_cast<size_t>(this->format_.sample_rate) * READ_MS / 1000U;
    std::vector<uint8_t> buffer(std::max<size_t>(frames, 1) * bytes_per_frame);

    bool open = false;
    int64_t retry_ms = SINK_RESCAN_DELAY_MS;
    while (this->wait_(0)) {
        if (!open) {
            open = this->source_.open(this->format_);
            if (!open) {
                log_line(LogLevel::WARN, LOG_TAG,
                         "Input device '%s' is unavailable -- retrying in %lld ms",
                         this->source_.name().c_str(), static_cast<long long>(retry_ms));
                if (!this->wait_(retry_ms)) {
                    break;
                }
                retry_ms = std::min(retry_ms * 2, SINK_RESCAN_MAX_DELAY_MS);
                continue;
            }
            retry_ms = SINK_RESCAN_DELAY_MS;
        }

        int64_t capture_time_us = 0;
        const int bytes =
            this->source_.read(buffer.data(), buffer.size(), READ_TIMEOUT_MS, capture_time_us);
        if (bytes < 0) {
            log_line(LogLevel::WARN, LOG_TAG, "Input device '%s' was lost mid-stream -- reopening",
                     this->source_.name().c_str());
            this->source_.close();
            open = false;
            // Paced even when the reopen works, so a device that opens but cannot read never spins.
            if (!this->wait_(SINK_RESCAN_DELAY_MS)) {
                break;
            }
            continue;
        }
        if (bytes > 0) {
            // A refused write is the role's drop policy at work, not ours to retry.
            this->writer_(buffer.data(), static_cast<size_t>(bytes), capture_time_us);
        }
    }

    if (open) {
        this->source_.close();
    }
}

std::string input_backend_list() {
#ifdef SENDSPIN_CLI_HAVE_ALSA
    return "null, tone, alsa";
#else
    return "null, tone";
#endif
}

bool resolve_input_spec(const std::string& spec, InputSpec& out, std::string& error) {
    if (spec.empty()) {
        error = "empty input device -- run with -l to list what this build has";
        return false;
    }

    // Split on the first colon: ALSA names carry their own.
    const size_t colon = spec.find(':');
    const bool has_device = colon != std::string::npos;
    const std::string prefix = spec.substr(0, colon);
    const std::string rest = has_device ? spec.substr(colon + 1) : std::string();

    if (prefix == "null" || prefix == "tone") {
        if (has_device) {
            error = "the " + prefix + " input takes no device, so --input '" + spec +
                    "' means nothing -- use --input " + prefix + " on its own";
            return false;
        }
        out = {prefix == "tone" ? SourceBackend::Tone : SourceBackend::Null, ""};
        return true;
    }

    if (prefix == "alsa") {
#ifdef SENDSPIN_CLI_HAVE_ALSA
        if (rest.empty()) {
            error = "--input '" + spec +
                    "' names no device -- write --input alsa:<device>, or -l to list them";
            return false;
        }
        out = {SourceBackend::Alsa, rest};
        return true;
#else
        error = "the ALSA backend is not in this build, so --input takes only: " +
                input_backend_list();
        return false;
#endif
    }

    for (const char* name : UNSUPPORTED_BACKENDS) {
        if (prefix != name) {
            continue;
        }
        error = "capturing through the " + prefix +
                " backend is not supported yet -- --input takes: " + input_backend_list();
#ifdef SENDSPIN_CLI_HAVE_ALSA
        if (prefix == "pulse" || prefix == "pipewire") {
            error += ". ALSA's plugin PCM of that name still works: --input alsa:" + prefix;
        }
#endif
        return false;
    }

#ifdef SENDSPIN_CLI_HAVE_ALSA
    out = {SourceBackend::Alsa, spec};
    return true;
#else
    error = "unknown input device '" + spec + "' -- this build has: " + input_backend_list() +
            " (run with -l)";
    return false;
#endif
}

bool input_pcm_is_reachable(const std::string& pcm) {
    InputSpec spec;
    std::string error;
    return resolve_input_spec(pcm, spec, error) && spec.backend == SourceBackend::Alsa &&
           spec.device == pcm;
}

std::unique_ptr<AudioSource> make_audio_source(const std::string& spec, StreamFormat& format,
                                               std::string& error) {
    InputSpec resolved;
    if (!resolve_input_spec(spec, resolved, error)) {
        return nullptr;
    }

    std::unique_ptr<AudioSource> source;
    switch (resolved.backend) {
        case SourceBackend::Null:
            source = std::make_unique<NullAudioSource>(NullSourceSignal::Silence);
            break;
        case SourceBackend::Tone:
            source = std::make_unique<NullAudioSource>(NullSourceSignal::Tone);
            break;
        case SourceBackend::Alsa:
#ifdef SENDSPIN_CLI_HAVE_ALSA
            source = std::make_unique<AlsaAudioSource>(resolved.device);
#endif
            break;
    }
    if (!source) {
        error = "internal error: input device '" + spec +
                "' resolved to a backend this build cannot construct";
        return nullptr;
    }
    // Negotiated now so a typo or a busy card fails at startup, not at the first stream.
    if (!source->negotiate(format, error)) {
        return nullptr;
    }
    return source;
}

void print_capture_devices(std::FILE* out) {
    std::fprintf(out, "\nInput devices (--input):\n");
    std::fprintf(out, "  null      silence; needs no sound card at all\n");
    std::fprintf(out, "  tone      a 440 Hz test tone, for checking the path to a server\n");
    std::fprintf(out,
                 "\n--input reads its argument the way -o does: one of the names above, or\n"
                 "<backend>:<device> split on the first colon, where <backend> is one of:\n"
                 "%s.\n",
                 input_backend_list().c_str());
#ifdef SENDSPIN_CLI_HAVE_ALSA
    std::fprintf(out, "Anything else is an ALSA PCM name, so --input hw:1,0 and --input default\n"
                      "work with no prefix at all.\n");
    std::fprintf(out, "\nALSA capture PCMs on this host (any of these names can follow --input):\n");
    AlsaAudioSource::list_devices(out);
    std::fprintf(out, "\nA capture stream runs at one format for the whole run: 48000 Hz, 2\n"
                      "channels, S16_LE where the PCM takes it, else the nearest it does take. A\n"
                      "plug-style PCM -- default, plughw: -- converts, so it takes the preferred\n"
                      "format whatever the hardware behind it captures.\n");
#else
    std::fprintf(out, "\nThis build has no ALSA backend, so there is no sound card to capture\n"
                      "from here.\n");
#endif
}

}  // namespace sendspin_cli
