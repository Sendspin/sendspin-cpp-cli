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

#include "pipewire_source.h"

#include "log.h"

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <utility>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

constexpr const char* PIPEWIRE_APP_NAME = "sendspin-cli";

/// The name the capture stream's node carries in the graph, apart from the playback stream's.
constexpr const char* PIPEWIRE_NODE_NAME = "sendspin-cli-input";

/// Quantum asked of the graph; one source-role chunk.
constexpr uint32_t QUANTUM_MS = 20;

/// Ring size; whole quanta are dropped once a stalled reader has let it fill.
constexpr uint32_t RING_MS = 1000;

constexpr int64_t US_PER_S = 1000000;

/// SPA's packed little-endian format for a capture bit depth.
spa_audio_format spa_format_for(uint8_t bit_depth) {
    switch (bit_depth) {
        case 16:
            return SPA_AUDIO_FORMAT_S16_LE;
        case 24:
            return SPA_AUDIO_FORMAT_S24_LE;
        case 32:
            return SPA_AUDIO_FORMAT_S32_LE;
        default:
            return SPA_AUDIO_FORMAT_UNKNOWN;
    }
}

/// Lists the graph's capture nodes, virtual ones included.
bool list_source_nodes(std::vector<PipeWireNode>& out, std::string& error) {
    return pipewire_list_nodes({"Audio/Source", "Audio/Source/Virtual"}, out, error);
}

}  // namespace

PipeWireAudioSource::PipeWireAudioSource(std::string device) : device_(std::move(device)) {}

PipeWireAudioSource::~PipeWireAudioSource() {
    this->close();
}

std::string PipeWireAudioSource::name() const {
    return this->device_.empty() ? "pipewire" : "pipewire:" + this->device_;
}

void PipeWireAudioSource::list_devices(std::FILE* out) {
    const PipeWireGuard guard;

    std::vector<PipeWireNode> nodes;
    std::string error;
    if (!list_source_nodes(nodes, error)) {
        std::fprintf(out, "  (%s)\n", error.c_str());
        return;
    }
    if (nodes.empty()) {
        std::fprintf(out, "  (this graph has no audio source nodes)\n");
        return;
    }
    for (const PipeWireNode& node : nodes) {
        std::fprintf(out, "  %s\n", node.name.c_str());
        if (!node.description.empty()) {
            std::fprintf(out, "      %s\n", node.description.c_str());
        }
    }
}

bool PipeWireAudioSource::find_node_(std::string& error) const {
    std::vector<PipeWireNode> nodes;
    if (!list_source_nodes(nodes, error)) {
        return false;
    }
    const bool found =
        this->device_.empty() ||
        std::any_of(nodes.begin(), nodes.end(),
                    [this](const PipeWireNode& node) { return node.name == this->device_; });
    if (!found) {
        error = "--input " + this->name() + ": this graph has no audio source node by that name";
    }
    return found;
}

bool PipeWireAudioSource::negotiate(StreamFormat& format, std::string& error) {
    if (!this->find_node_(error)) {
        error += " -- run with -l to list this host's capture devices";
        return false;
    }
    settle_server_capture_format(format);
    return true;
}

bool PipeWireAudioSource::open(const StreamFormat& format) {
    this->close();

    const spa_audio_format spa_format = spa_format_for(format.bit_depth);
    if (spa_format == SPA_AUDIO_FORMAT_UNKNOWN || format.channels == 0 || format.sample_rate == 0) {
        cli_log(LogLevel::ERROR, "pipewire: input cannot capture %u Hz / %u ch / %u-bit",
                format.sample_rate, format.channels, format.bit_depth);
        return false;
    }
    // A named node that has gone would otherwise be swapped for the default by the session manager.
    std::string error;
    if (!this->device_.empty() && !this->find_node_(error)) {
        cli_log(LogLevel::ERROR, "pipewire: %s", error.c_str());
        return false;
    }

    this->loop_ = pw_thread_loop_new("sendspin-pw-in", nullptr);
    if (this->loop_ == nullptr || pw_thread_loop_start(this->loop_) < 0) {
        cli_log(LogLevel::ERROR, "pipewire: cannot start a loop for input '%s'",
                this->name().c_str());
        this->close();
        return false;
    }

    this->rate_ = format.sample_rate;
    this->bytes_per_frame_ =
        static_cast<size_t>(format.channels) * (static_cast<size_t>(format.bit_depth) / 8U);
    // The spare byte the ring keeps to tell full from empty.
    this->ring_.reset((static_cast<size_t>(format.sample_rate) * RING_MS / 1000U *
                       this->bytes_per_frame_) +
                      1);

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_APP_NAME,
        PIPEWIRE_APP_NAME, PW_KEY_NODE_NAME, PIPEWIRE_NODE_NAME, nullptr);
    if (props == nullptr) {
        cli_log(LogLevel::ERROR, "pipewire: cannot describe the input stream to the graph");
        this->close();
        return false;
    }
    if (!this->device_.empty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, this->device_.c_str());
        // Unplugging the node ends the stream, so it is reopened rather than moved elsewhere.
        pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");
    }
    const std::string latency = std::to_string(format.sample_rate * QUANTUM_MS / 1000U) + "/" +
                                std::to_string(format.sample_rate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency.c_str());

    uint8_t pod_storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    spa_audio_info_raw info{};
    info.format = spa_format;
    info.rate = format.sample_rate;
    info.channels = format.channels;
    if (format.channels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (format.channels == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    // Static: pw_stream_new_simple() keeps the pointer. Zeroed then assigned: the struct grows.
    static constexpr pw_stream_events events = [] {
        pw_stream_events table{};
        table.version = PW_VERSION_STREAM_EVENTS;
        table.state_changed = &PipeWireAudioSource::stream_state_cb;
        table.process = &PipeWireAudioSource::stream_process_cb;
        return table;
    }();

    int err = -ENOMEM;
    {
        const LoopLock lock(this->loop_);
        // pw_stream_new_simple() consumes props even on failure.
        this->stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(this->loop_),
                                             PIPEWIRE_APP_NAME, props, &events, this);
        if (this->stream_ != nullptr) {
            const auto flags = static_cast<pw_stream_flags>(
                PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
            err = pw_stream_connect(this->stream_, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1);
        }
        if (err >= 0) {
            // PAUSED counts: the graph has taken the stream and starts it when a source is linked.
            timespec deadline{};
            pw_thread_loop_get_time(this->loop_, &deadline,
                                    static_cast<int64_t>(PIPEWIRE_RECOVERY_TIMEOUT_MS) * 1000 * 1000);
            while (true) {
                const auto state = static_cast<pw_stream_state>(this->stream_state_.load());
                if (state == PW_STREAM_STATE_STREAMING || state == PW_STREAM_STATE_PAUSED ||
                    state == PW_STREAM_STATE_ERROR) {
                    break;
                }
                if (pw_thread_loop_timed_wait_full(this->loop_, &deadline) != 0) {
                    break;
                }
            }
        }
    }

    const auto state = static_cast<pw_stream_state>(this->stream_state_.load());
    if (err < 0) {
        cli_log(LogLevel::ERROR, "pipewire: cannot capture from '%s': %s", this->name().c_str(),
                spa_strerror(err));
    } else if (state == PW_STREAM_STATE_ERROR) {
        err = -EIO;  // stream_state_cb() has said why
    } else if (state != PW_STREAM_STATE_STREAMING && state != PW_STREAM_STATE_PAUSED) {
        err = -ETIMEDOUT;
        cli_log(LogLevel::ERROR, "pipewire: input '%s' would not start at %u Hz, %u ch, %u-bit",
                this->name().c_str(), format.sample_rate, format.channels, format.bit_depth);
    }
    if (err < 0) {
        this->close();
        return false;
    }

    cli_log(LogLevel::INFO, "pipewire: input '%s' open at %u Hz, %u ch, %u-bit",
            this->name().c_str(), format.sample_rate, format.channels, format.bit_depth);
    return true;
}

void PipeWireAudioSource::stream_state_cb(void* userdata, enum pw_stream_state /*old*/,
                                          enum pw_stream_state state, const char* error) {
    auto* self = static_cast<PipeWireAudioSource*>(userdata);
    self->stream_state_.store(static_cast<int>(state));
    if (state == PW_STREAM_STATE_ERROR) {
        cli_log(LogLevel::WARN, "pipewire: input '%s' failed: %s", self->name().c_str(),
                (error != nullptr) ? error : "(no reason given)");
    }
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        self->lost_.store(true);
        self->data_ready_.notify_all();
    }
    pw_thread_loop_signal(self->loop_, false);
}

void PipeWireAudioSource::stream_process_cb(void* userdata) {
    auto* self = static_cast<PipeWireAudioSource*>(userdata);

    // Safe unlocked: pw_stream_disconnect() waits for this callback to return.
    pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream_);
    if (buffer == nullptr) {
        return;
    }
    const spa_data& data = buffer->buffer->datas[0];
    const size_t stride = self->bytes_per_frame_;
    if (data.data != nullptr && stride != 0) {
        const uint32_t offset = std::min(data.chunk->offset, data.maxsize);
        const size_t frames = std::min(data.chunk->size, data.maxsize - offset) / stride;
        // All or nothing, so the frames in the ring stay contiguous within a quantum.
        if (frames != 0 && self->ring_.free_space() >= frames * stride) {
            pw_time time{};
            if (pw_stream_get_time_n(self->stream_, &time, sizeof(time)) == 0 &&
                time.rate.denom != 0) {
                // `delay` is the newest frame's age; the first is older by this buffer and by
                // what the converter still holds.
                const double age_s = (static_cast<double>(time.delay) *
                                      static_cast<double>(time.rate.num) /
                                      static_cast<double>(time.rate.denom)) +
                                     (static_cast<double>(time.buffered + frames) /
                                      static_cast<double>(self->rate_));
                // `now` is CLOCK_MONOTONIC, which is what steady_clock reads here.
                const int64_t first_us = (time.now / 1000) - static_cast<int64_t>(age_s * 1e6);
                self->origin_us_.store(first_us - static_cast<int64_t>(self->frames_written_) *
                                                      US_PER_S / self->rate_);
            }
            self->ring_.write(static_cast<const uint8_t*>(data.data) + offset, frames * stride);
            self->frames_written_ += frames;
        }
    }
    pw_stream_queue_buffer(self->stream_, buffer);

    // Notified without the mutex; a missed wakeup costs at most one quantum.
    self->data_ready_.notify_one();
}

int PipeWireAudioSource::read(uint8_t* data, size_t length, uint32_t timeout_ms,
                              int64_t& capture_time_us) {
    if (this->stream_ == nullptr) {
        return -1;
    }
    {
        std::unique_lock<std::mutex> lock(this->wait_mutex_);
        this->data_ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
            return this->lost_.load() || this->ring_.available() >= this->bytes_per_frame_;
        });
    }
    if (this->lost_.load()) {
        return -1;
    }

    size_t bytes = std::min(this->ring_.available(), length);
    bytes -= bytes % this->bytes_per_frame_;
    if (bytes == 0) {
        return 0;
    }
    this->ring_.read(data, bytes);

    const int64_t origin_us = this->origin_us_.load();
    capture_time_us =
        (origin_us == 0)
            ? 0
            : origin_us + static_cast<int64_t>(this->frames_read_) * US_PER_S / this->rate_;
    this->frames_read_ += bytes / this->bytes_per_frame_;
    return static_cast<int>(bytes);
}

void PipeWireAudioSource::close() {
    if (this->stream_ != nullptr) {
        const LoopLock lock(this->loop_);
        // Disconnect before destroy, under the loop lock: disconnect waits out process().
        pw_stream_disconnect(this->stream_);
        pw_stream_destroy(this->stream_);
        this->stream_ = nullptr;
    }
    // The loop goes too: a restarted daemon took the old connection with it.
    if (this->loop_ != nullptr) {
        pw_thread_loop_destroy(this->loop_);
        this->loop_ = nullptr;
    }

    this->ring_.reset(0);
    this->rate_ = 0;
    this->bytes_per_frame_ = 0;
    this->frames_written_ = 0;
    this->frames_read_ = 0;
    this->origin_us_.store(0);
    this->lost_.store(false);
    this->stream_state_.store(PW_STREAM_STATE_UNCONNECTED);
}

}  // namespace sendspin_cli
