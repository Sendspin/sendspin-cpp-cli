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

#include "pulse_source.h"

#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// The stream name the server shows in its mixer.
constexpr const char* PULSE_STREAM_NAME = "sendspin-cli input";

/// Fragment asked of the server; one source-role chunk.
constexpr pa_usec_t FRAGMENT_US = 20000;

constexpr int64_t US_PER_S = 1000000;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// PulseAudio's packed little-endian format for a capture bit depth.
pa_sample_format_t pulse_format_for(uint8_t bit_depth) {
    switch (bit_depth) {
        case 16:
            return PA_SAMPLE_S16LE;
        case 24:
            return PA_SAMPLE_S24LE;
        case 32:
            return PA_SAMPLE_S32LE;
        default:
            return PA_SAMPLE_INVALID;
    }
}

/// The source name PulseAudio resolves to the default sink's monitor.
constexpr const char* DEFAULT_MONITOR = "@DEFAULT_MONITOR@";

/// One source as the server describes it.
struct PulseSourceInfo {
    std::string name;
    std::string description;
    std::string monitor_of_sink;  ///< empty unless this is a sink's monitor
    pa_sample_spec spec{};
};

struct SourceListQuery : PulseQuery {
    std::vector<PulseSourceInfo> sources;
};

struct DefaultSourceQuery : PulseQuery {
    std::string name;
};

void source_info_cb(pa_context* /*context*/, const pa_source_info* info, int eol, void* userdata) {
    auto* query = static_cast<SourceListQuery*>(userdata);
    if (eol != 0) {
        query->finish(eol > 0);
        return;
    }
    PulseSourceInfo entry;
    entry.name = (info->name != nullptr) ? info->name : "";
    entry.description = (info->description != nullptr) ? info->description : "";
    if (info->monitor_of_sink != PA_INVALID_INDEX && info->monitor_of_sink_name != nullptr) {
        entry.monitor_of_sink = info->monitor_of_sink_name;
    }
    entry.spec = info->sample_spec;
    query->sources.push_back(std::move(entry));
}

void server_info_cb(pa_context* /*context*/, const pa_server_info* info, void* userdata) {
    auto* query = static_cast<DefaultSourceQuery*>(userdata);
    if (info != nullptr && info->default_source_name != nullptr) {
        query->name = info->default_source_name;
    }
    query->finish(info != nullptr);
}

/// Asks the server for every source it has, monitors included. The mainloop lock must NOT be held.
bool list_sources(PulseConnection& conn, std::vector<PulseSourceInfo>& out,
                  int timeout_ms = PULSE_TIMEOUT_MS) {
    SourceListQuery query;
    query.conn = &conn;
    pa_operation* op = nullptr;
    {
        const MainloopLock lock(conn.mainloop());
        op = pa_context_get_source_info_list(conn.context(), source_info_cb, &query);
    }
    if (!await_query(conn, query, op, timeout_ms)) {
        return false;
    }
    out = std::move(query.sources);
    return true;
}

/// The source the server calls default, or empty. The mainloop lock must NOT be held.
std::string default_source_name(PulseConnection& conn) {
    DefaultSourceQuery query;
    query.conn = &conn;
    pa_operation* op = nullptr;
    {
        const MainloopLock lock(conn.mainloop());
        op = pa_context_get_server_info(conn.context(), server_info_cb, &query);
    }
    return await_query(conn, query, op) ? query.name : "";
}

}  // namespace

PulseAudioSource::PulseAudioSource(std::string device, bool loopback)
    : device_(std::move(device)), loopback_(loopback) {}

PulseAudioSource::~PulseAudioSource() {
    this->close();
}

std::string PulseAudioSource::name() const {
    if (this->loopback_) {
        return this->device_.empty() ? "loopback" : "loopback:" + this->device_;
    }
    return this->device_.empty() ? "pulse" : "pulse:" + this->device_;
}

std::optional<std::string> PulseAudioSource::looped_sink() const {
    return this->looped_sink_;
}

bool PulseAudioSource::server_answers(std::string& error) {
    PulseConnection conn;
    return conn.connect(error);
}

void PulseAudioSource::list_devices(std::FILE* out) {
    PulseConnection conn;
    std::string error;
    if (!conn.connect(error)) {
        std::fprintf(out, "  (%s)\n", error.c_str());
        return;
    }

    std::vector<PulseSourceInfo> sources;
    if (!list_sources(conn, sources)) {
        std::fprintf(out, "  (cannot list the sources on %s)\n", conn.server_name().c_str());
        return;
    }
    if (sources.empty()) {
        std::fprintf(out, "  (%s has no sources)\n", conn.server_name().c_str());
        return;
    }

    const std::string fallback = default_source_name(conn);
    for (const PulseSourceInfo& source : sources) {
        std::fprintf(out, "  %s%s\n", source.name.c_str(),
                     (source.name == fallback) ? "  (server default)" : "");
        std::fprintf(out, "      %s\n", source.description.c_str());
        char spec[PA_SAMPLE_SPEC_SNPRINT_MAX];
        pa_sample_spec_snprint(spec, sizeof(spec), &source.spec);
        std::fprintf(out, "      running at %s\n", spec);
    }
}

void PulseAudioSource::list_sinks(std::FILE* out) {
    PulseConnection conn;
    std::string error;
    if (!conn.connect(error)) {
        std::fprintf(out, "  (%s)\n", error.c_str());
        return;
    }

    std::vector<PulseSinkInfo> sinks;
    if (!sendspin_cli::list_sinks(conn, sinks)) {
        std::fprintf(out, "  (cannot list the sinks on %s)\n", conn.server_name().c_str());
        return;
    }
    if (sinks.empty()) {
        std::fprintf(out, "  (%s has no sinks)\n", conn.server_name().c_str());
        return;
    }

    const std::string fallback = default_sink_name(conn);
    for (const PulseSinkInfo& sink : sinks) {
        std::fprintf(out, "  %s%s\n", sink.name.c_str(),
                     (sink.name == fallback) ? "  (server default)" : "");
        std::fprintf(out, "      %s\n", sink.description.c_str());
    }
}

bool PulseAudioSource::resolve_source_(std::string& error, int timeout_ms) {
    this->record_from_.clear();
    this->looped_sink_.reset();
    this->looped_sink_name_.clear();
    if (this->device_.empty()) {
        if (this->loopback_) {
            this->record_from_ = DEFAULT_MONITOR;
            this->looped_sink_ = "";
            this->looped_sink_name_ = default_sink_name(this->conn_, timeout_ms);
        }
        return true;
    }

    std::vector<PulseSourceInfo> sources;
    if (!list_sources(this->conn_, sources, timeout_ms)) {
        error = "cannot list the sources on " + this->conn_.server_name();
        return false;
    }
    // loopback: takes a sink or a sink's monitor, never an input.
    const auto source =
        std::find_if(sources.begin(), sources.end(), [this](const PulseSourceInfo& info) {
            return info.name == this->device_ &&
                   (!this->loopback_ || !info.monitor_of_sink.empty());
        });
    if (source != sources.end()) {
        this->record_from_ = source->name;
        if (!source->monitor_of_sink.empty()) {
            this->looped_sink_ = source->monitor_of_sink;
        }
        return true;
    }

    std::vector<PulseSinkInfo> sinks;
    if (!sendspin_cli::list_sinks(this->conn_, sinks, timeout_ms)) {
        error = "cannot list the sinks on " + this->conn_.server_name();
        return false;
    }
    const auto sink = std::find_if(sinks.begin(), sinks.end(), [this](const PulseSinkInfo& info) {
        return info.name == this->device_;
    });
    if (sink == sinks.end() || sink->monitor_source.empty()) {
        error = "--input " + this->name() + ": " + this->conn_.server_name() +
                (this->loopback_ ? " has no sink" : " has no source or sink") + " by that name";
        return false;
    }
    this->record_from_ = sink->monitor_source;
    this->looped_sink_ = sink->name;
    return true;
}

bool PulseAudioSource::negotiate(StreamFormat& format, std::string& error) {
    settle_server_capture_format(format);
    if (!this->open_(format, PULSE_TIMEOUT_MS, error)) {
        error += " -- run with -l to list this host's capture devices";
        return false;
    }
    this->close();
    return true;
}

bool PulseAudioSource::open(const StreamFormat& format) {
    std::string error;
    if (!this->open_(format, PULSE_RECOVERY_TIMEOUT_MS, error)) {
        cli_log(LogLevel::ERROR, "pulse: %s", error.c_str());
        return false;
    }
    std::string captures;
    if (this->looped_sink_ && !this->looped_sink_->empty()) {
        captures = " -- capturing the monitor of sink '" + *this->looped_sink_ + "'";
    } else if (this->looped_sink_) {
        captures = " -- capturing the monitor of the default sink";
        if (!this->looped_sink_name_.empty()) {
            captures += " '" + this->looped_sink_name_ + "'";
        }
    }
    cli_log(LogLevel::INFO, "pulse: input '%s' open on %s at %u Hz, %u ch, %u-bit%s",
            this->name().c_str(), this->conn_.server_name().c_str(), format.sample_rate,
            format.channels, format.bit_depth, captures.c_str());
    return true;
}

bool PulseAudioSource::open_(const StreamFormat& format, int timeout_ms, std::string& error) {
    this->close();

    const std::string device = "PulseAudio input '" + this->name() + "'";
    const std::string wanted = std::to_string(format.sample_rate) + " Hz / " +
                               std::to_string(static_cast<unsigned>(format.channels)) + " ch / " +
                               std::to_string(static_cast<unsigned>(format.bit_depth)) + "-bit";
    pa_sample_spec spec{};
    spec.format = pulse_format_for(format.bit_depth);
    spec.rate = format.sample_rate;
    spec.channels = format.channels;
    if (pa_sample_spec_valid(&spec) == 0) {
        error = device + " cannot capture " + wanted;
        return false;
    }
    if (!this->conn_.ready() && !this->conn_.connect(error, timeout_ms)) {
        return false;
    }
    // pipewire-pulse records from the default when the named source is missing.
    if (!this->resolve_source_(error, timeout_ms)) {
        return false;
    }

    this->rate_ = format.sample_rate;
    this->bytes_per_frame_ = pa_frame_size(&spec);

    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = static_cast<uint32_t>(pa_usec_to_bytes(FRAGMENT_US, &spec));

    int flags =
        PA_STREAM_ADJUST_LATENCY | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE;
    if (!this->device_.empty()) {
        // Unplugging the source ends the stream, so it is reopened rather than moved elsewhere.
        flags |= PA_STREAM_DONT_MOVE;
    }
    const char* source = this->record_from_.empty() ? nullptr : this->record_from_.c_str();

    int err = -1;
    {
        const MainloopLock ml(this->conn_.mainloop());
        this->stream_ = pa_stream_new(this->conn_.context(), PULSE_STREAM_NAME, &spec, nullptr);
        if (this->stream_ != nullptr) {
            pa_stream_set_state_callback(this->stream_, &PulseAudioSource::stream_state_cb, this);
            pa_stream_set_read_callback(this->stream_, &PulseAudioSource::stream_read_cb, this);
            err = pa_stream_connect_record(this->stream_, source, &attr,
                                           static_cast<pa_stream_flags_t>(flags));
        }
        if (err < 0) {
            error = "cannot open " + device + ": " +
                    pa_strerror(pa_context_errno(this->conn_.context()));
        }
    }
    if (err < 0) {
        this->close();
        return false;
    }

    const bool settled = this->conn_.wait_for(
        [this] {
            const auto state = static_cast<pa_stream_state_t>(this->stream_state_.load());
            return state == PA_STREAM_READY || PA_STREAM_IS_GOOD(state) == 0;
        },
        timeout_ms);
    if (!settled || this->stream_state_.load() != PA_STREAM_READY) {
        error =
            device + " would not start at " + wanted + ": " +
            (settled ? "the server refused it"
                     : "the server did not answer within " + std::to_string(timeout_ms) + " ms");
        this->close();
        return false;
    }
    return true;
}

void PulseAudioSource::stream_state_cb(pa_stream* stream, void* userdata) {
    auto* self = static_cast<PulseAudioSource*>(userdata);
    const pa_stream_state_t state = pa_stream_get_state(stream);
    self->stream_state_.store(static_cast<int>(state));
    if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
        self->lost_.store(true);
    }
    self->conn_.notify();
}

void PulseAudioSource::stream_read_cb(pa_stream* /*stream*/, size_t /*nbytes*/, void* userdata) {
    auto* self = static_cast<PulseAudioSource*>(userdata);
    self->readable_.store(true);
    self->conn_.notify();
}

int PulseAudioSource::drain_(uint8_t* data, size_t length, int64_t& capture_time_us) {
    if (this->lost_.load() || !this->conn_.ready()) {
        return -1;
    }
    const MainloopLock ml(this->conn_.mainloop());

    // The server's measure of how old the sample at the read index is.
    pa_usec_t latency_us = 0;
    int negative = 0;
    const bool timed =
        pa_stream_get_latency(this->stream_, &latency_us, &negative) == 0 && negative == 0;
    // The read index only moves with a whole fragment, so the part already returned is added back.
    const int64_t first_us = now_us() - static_cast<int64_t>(latency_us) +
                             static_cast<int64_t>(this->fragment_offset_ / this->bytes_per_frame_) *
                                 US_PER_S / this->rate_;

    size_t done = 0;
    while (length - done >= this->bytes_per_frame_) {
        const void* fragment = nullptr;
        size_t size = 0;
        if (pa_stream_peek(this->stream_, &fragment, &size) < 0) {
            return -1;
        }
        if (size == 0) {
            break;
        }
        if (fragment == nullptr) {
            // A hole: skipped, and the read ends here so no timestamp spans it.
            pa_stream_drop(this->stream_);
            break;
        }
        size_t chunk = std::min(size - this->fragment_offset_, length - done);
        chunk -= chunk % this->bytes_per_frame_;
        std::memcpy(data + done, static_cast<const uint8_t*>(fragment) + this->fragment_offset_,
                    chunk);
        done += chunk;
        this->fragment_offset_ += chunk;
        if (size - this->fragment_offset_ < this->bytes_per_frame_) {
            pa_stream_drop(this->stream_);
            this->fragment_offset_ = 0;
        }
    }
    if (done > 0) {
        capture_time_us = timed ? first_us : 0;
    }
    return static_cast<int>(done);
}

int PulseAudioSource::read(uint8_t* data, size_t length, uint32_t timeout_ms,
                           int64_t& capture_time_us) {
    if (this->stream_ == nullptr) {
        return -1;
    }
    // Cleared before the server is asked, so a fragment landing in between still ends the wait.
    this->readable_.store(false);
    int bytes = this->drain_(data, length, capture_time_us);
    if (bytes == 0 &&
        this->conn_.wait_for(
            [this] { return this->readable_.load() || this->lost_.load() || !this->conn_.ready(); },
            static_cast<int>(timeout_ms))) {
        bytes = this->drain_(data, length, capture_time_us);
    }
    return bytes;
}

void PulseAudioSource::close() {
    if (this->stream_ != nullptr && this->conn_.mainloop() != nullptr) {
        const MainloopLock ml(this->conn_.mainloop());
        // Cleared before the disconnect, whose own state change must not reach a dying stream.
        pa_stream_set_state_callback(this->stream_, nullptr, nullptr);
        pa_stream_set_read_callback(this->stream_, nullptr, nullptr);
        pa_stream_disconnect(this->stream_);
        pa_stream_unref(this->stream_);
    }
    this->stream_ = nullptr;
    this->rate_ = 0;
    this->bytes_per_frame_ = 0;
    this->fragment_offset_ = 0;
    this->readable_.store(false);
    this->lost_.store(false);
    this->stream_state_.store(PA_STREAM_UNCONNECTED);
}

}  // namespace sendspin_cli
