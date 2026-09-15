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

#include "pulse_sink.h"

#include "log.h"
#include "pcm_volume.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// Longest write() sleep before re-asking the server for room; bounds a missed notification.
constexpr int WRITE_SLICE_MS = 10;

/// The application name the server shows in its mixer.
constexpr const char* PULSE_APP_NAME = "sendspin-cli";

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// PulseAudio format per PROBE_BIT_DEPTHS entry; signed 8-bit goes out as sign-flipped U8.
constexpr std::array<pa_sample_format_t, PROBE_BIT_DEPTHS.size()> PROBE_FORMATS{
    PA_SAMPLE_U8, PA_SAMPLE_S16LE, PA_SAMPLE_S24LE, PA_SAMPLE_S32LE};

/// How this backend spells PROBE_BIT_DEPTHS, in the same order: PulseAudio's own names.
constexpr std::array<const char*, PROBE_BIT_DEPTHS.size()> PROBE_FORMAT_NAMES{"u8", "s16le",
                                                                              "s24le", "s32le"};

/// Maps the stream's bit depth onto the PulseAudio sample format it is carried in.
bool pulse_format_for(uint8_t bits_per_sample, pa_sample_format_t& format) {
    for (size_t i = 0; i < PROBE_BIT_DEPTHS.size(); ++i) {
        if (PROBE_BIT_DEPTHS[i] == bits_per_sample) {
            format = PROBE_FORMATS[i];
            return true;
        }
    }
    return false;
}

/// Holds the libpulse mainloop lock for a scope. Every pa_* call on a live context needs it.
class MainloopLock {
public:
    explicit MainloopLock(pa_threaded_mainloop* loop) : loop_(loop) {
        pa_threaded_mainloop_lock(this->loop_);
    }
    ~MainloopLock() {
        pa_threaded_mainloop_unlock(this->loop_);
    }

    MainloopLock(const MainloopLock&) = delete;
    MainloopLock& operator=(const MainloopLock&) = delete;

private:
    pa_threaded_mainloop* loop_;
};

/// State shared by an asynchronous server query and its waiter; `done` is read unlocked.
struct PulseQuery {
    PulseConnection* conn{nullptr};
    std::atomic<bool> done{false};
    bool failed{false};

    /// Called from the query's own callback, on the mainloop thread, exactly once.
    void finish(bool ok) {
        this->failed = !ok;
        this->done.store(true, std::memory_order_release);
        this->conn->notify();
    }
};

/// One sink as the server describes it.
struct PulseSinkInfo {
    std::string name;
    std::string description;
    pa_sample_spec spec{};
};

struct SinkListQuery : PulseQuery {
    std::vector<PulseSinkInfo> sinks;
};

struct ServerQuery : PulseQuery {
    std::string default_sink;
};

void sink_info_cb(pa_context* /*context*/, const pa_sink_info* info, int eol, void* userdata) {
    auto* query = static_cast<SinkListQuery*>(userdata);
    if (eol < 0) {
        query->finish(false);
        return;
    }
    if (eol > 0) {
        query->finish(true);
        return;
    }
    PulseSinkInfo entry;
    entry.name = (info->name != nullptr) ? info->name : "";
    entry.description = (info->description != nullptr) ? info->description : "";
    entry.spec = info->sample_spec;
    query->sinks.push_back(std::move(entry));
}

void server_info_cb(pa_context* /*context*/, const pa_server_info* info, void* userdata) {
    auto* query = static_cast<ServerQuery*>(userdata);
    if (info != nullptr && info->default_sink_name != nullptr) {
        query->default_sink = info->default_sink_name;
    }
    query->finish(info != nullptr);
}

/// Waits for one query, cancelling it on timeout so a late callback cannot touch a dead `query`.
/// @return true if the query completed and the server answered it.
bool await_query(PulseConnection& conn, PulseQuery& query, pa_operation* op) {
    if (op == nullptr) {
        return false;
    }
    const bool answered =
        conn.wait_for([&query] { return query.done.load(std::memory_order_acquire); });
    {
        const MainloopLock lock(conn.mainloop());
        if (!answered) {
            pa_operation_cancel(op);
        }
        pa_operation_unref(op);
    }
    return answered && !query.failed;
}

/// Asks the server for every sink it has. The mainloop lock must NOT be held.
bool list_sinks(PulseConnection& conn, std::vector<PulseSinkInfo>& out) {
    SinkListQuery query;
    query.conn = &conn;
    pa_operation* op = nullptr;
    {
        const MainloopLock lock(conn.mainloop());
        op = pa_context_get_sink_info_list(conn.context(), sink_info_cb, &query);
    }
    if (!await_query(conn, query, op)) {
        return false;
    }
    out = std::move(query.sinks);
    return true;
}

/// Asks the server which sink it calls default. Empty when it will not say. Lock must NOT be held.
std::string default_sink_name(PulseConnection& conn) {
    ServerQuery query;
    query.conn = &conn;
    pa_operation* op = nullptr;
    {
        const MainloopLock lock(conn.mainloop());
        op = pa_context_get_server_info(conn.context(), server_info_cb, &query);
    }
    if (!await_query(conn, query, op)) {
        return "";
    }
    return query.default_sink;
}

}  // namespace


PulseConnection::~PulseConnection() {
    this->disconnect();
}

void PulseConnection::state_cb(pa_context* context, void* userdata) {
    auto* self = static_cast<PulseConnection*>(userdata);
    self->state_.store(pa_context_get_state(context), std::memory_order_release);
    self->notify();
}

void PulseConnection::notify() {
    // Taken briefly so a waiter between its predicate check and sleep cannot miss this.
    const std::lock_guard<std::mutex> lock(this->wait_mutex_);
    this->wait_cv_.notify_all();
}

bool PulseConnection::connect(std::string& error, int timeout_ms) {
    this->disconnect();

    this->loop_ = pa_threaded_mainloop_new();
    if (this->loop_ == nullptr) {
        error = "cannot create a PulseAudio mainloop";
        return false;
    }
    this->context_ = pa_context_new(pa_threaded_mainloop_get_api(this->loop_), PULSE_APP_NAME);
    if (this->context_ == nullptr) {
        error = "cannot create a PulseAudio context";
        this->disconnect();
        return false;
    }

    // Before the mainloop starts, so no state change goes unrecorded.
    this->state_.store(PA_CONTEXT_UNCONNECTED, std::memory_order_release);
    pa_context_set_state_callback(this->context_, &PulseConnection::state_cb, this);

    if (pa_context_connect(this->context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        error = std::string("cannot reach a PulseAudio server: ") +
                pa_strerror(pa_context_errno(this->context_));
        this->disconnect();
        return false;
    }
    if (pa_threaded_mainloop_start(this->loop_) < 0) {
        error = "cannot start the PulseAudio mainloop thread";
        this->disconnect();
        return false;
    }

    const bool settled = this->wait_for(
        [this] {
            const pa_context_state_t state = this->state_.load(std::memory_order_acquire);
            return state == PA_CONTEXT_READY || PA_CONTEXT_IS_GOOD(state) == 0;
        },
        timeout_ms);
    if (!settled) {
        error = "the PulseAudio server did not answer within " + std::to_string(timeout_ms) + " ms";
        this->disconnect();
        return false;
    }
    if (!this->ready()) {
        // The errno is read under the mainloop lock, because the mainloop thread owns the context.
        std::string reason;
        {
            const MainloopLock lock(this->loop_);
            reason = pa_strerror(pa_context_errno(this->context_));
        }
        error = "cannot reach a PulseAudio server: " + reason;
        this->disconnect();
        return false;
    }
    return true;
}

void PulseConnection::disconnect() {
    if (this->loop_ != nullptr) {
        // Stop the mainloop first: disconnecting while a callback may run is unsafe.
        pa_threaded_mainloop_stop(this->loop_);
    }
    if (this->context_ != nullptr) {
        pa_context_set_state_callback(this->context_, nullptr, nullptr);
        pa_context_disconnect(this->context_);
        pa_context_unref(this->context_);
        this->context_ = nullptr;
    }
    if (this->loop_ != nullptr) {
        pa_threaded_mainloop_free(this->loop_);
        this->loop_ = nullptr;
    }
    this->state_.store(PA_CONTEXT_UNCONNECTED, std::memory_order_release);
}

bool PulseConnection::ready() const {
    return this->state_.load(std::memory_order_acquire) == PA_CONTEXT_READY;
}

std::string PulseConnection::server_name() const {
    if (this->context_ == nullptr || this->loop_ == nullptr) {
        return "(no server)";
    }
    const MainloopLock lock(this->loop_);
    const char* server = pa_context_get_server(this->context_);
    return (server != nullptr) ? server : "(no server)";
}


PulseAudioSink::PulseAudioSink(std::string device, uint32_t buffer_ms)
    : device_(std::move(device)), buffer_ms_(buffer_ms) {
    std::string error;
    if (!this->conn_.connect(error)) {
        // Degrade to discarding; configure() reconnects.
        cli_log(LogLevel::ERROR, "pulse: %s", error.c_str());
    }
}

PulseAudioSink::~PulseAudioSink() {
    // A sink destroyed without stop() must still free its stream before conn_ goes.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->close_stream_();
}

std::string PulseAudioSink::name() const {
    return this->device_.empty() ? "pulse" : "pulse:" + this->device_;
}

SinkCapabilities PulseAudioSink::capabilities() const {
    return SinkCapabilities::permissive();
}

bool PulseAudioSink::probe(const std::string& device, std::string& error) {
    PulseConnection conn;
    if (!conn.connect(error)) {
        error += " -- run with -l to see what this host has, or -o alsa:pulse to reach the same "
                 "server through ALSA's plugin PCM";
        return false;
    }
    if (device.empty()) {
        return true;
    }

    std::vector<PulseSinkInfo> sinks;
    if (!list_sinks(conn, sinks)) {
        error = "cannot list the sinks on " + conn.server_name();
        return false;
    }
    const bool found = std::any_of(sinks.begin(), sinks.end(),
                                   [&device](const PulseSinkInfo& s) { return s.name == device; });
    if (!found) {
        error = "-o pulse:" + device + ": " + conn.server_name() +
                " has no sink by that name -- run with -l to list them";
        return false;
    }
    return true;
}

void PulseAudioSink::list_devices(std::FILE* out) {
    PulseConnection conn;
    std::string error;
    if (!conn.connect(error)) {
        std::fprintf(out, "  (%s)\n", error.c_str());
        return;
    }

    std::vector<PulseSinkInfo> sinks;
    if (!list_sinks(conn, sinks)) {
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
        char spec[PA_SAMPLE_SPEC_SNPRINT_MAX];
        pa_sample_spec_snprint(spec, sizeof(spec), &sink.spec);
        std::fprintf(out, "      running at %s\n", spec);
    }

    std::fprintf(out, "\n  Every sink above accepts all of:\n");
    print_sink_capabilities(out, SinkCapabilities::permissive(), PROBE_FORMAT_NAMES);
}

bool PulseAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    if (this->stopping_.load()) {
        cli_log(LogLevel::DEBUG, "pulse: ignoring a stream start during shutdown");
        return false;
    }

    // Before anything can fail: recovery reopens at this format.
    this->last_format_ = {sample_rate, channels, bits_per_sample};

    this->close_stream_();
    if (!this->conn_.ready()) {
        std::string error;
        if (!this->conn_.connect(error)) {
            cli_log(LogLevel::ERROR, "pulse: %s", error.c_str());
            this->failed_.store(true);
            return false;
        }
    }
    if (!this->open_stream_(sample_rate, channels, bits_per_sample, PULSE_TIMEOUT_MS)) {
        this->failed_.store(true);
        return false;
    }
    // Refill the recovery budget only once a stream is really running.
    this->recovery_.reset();
    return true;
}

size_t PulseAudioSink::write(const uint8_t* data, size_t length, uint32_t timeout_ms) {
    if (data == nullptr || length == 0) {
        return 0;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(this->mutex_);

    if (!this->stream_alive_() && !this->reopen_in_place_()) {
        // Discard rather than return 0 forever, which would spin the sync task.
        if (!this->failed_.exchange(true)) {
            cli_log(LogLevel::ERROR,
                    "pulse: '%s' is not playing -- discarding audio until a stream reconfigures it",
                    this->name().c_str());
        }
        // Frame-aligned via last_format_, since close_stream_() zeroed bytes_per_frame_.
        const size_t frame = (this->bytes_per_frame_ != 0)
                                 ? this->bytes_per_frame_
                                 : static_cast<size_t>(this->last_format_.channels) *
                                       (static_cast<size_t>(this->last_format_.bit_depth) / 8U);
        return (frame == 0) ? length : length - (length % frame);
    }

    const size_t bytes_per_frame = this->bytes_per_frame_;
    const uint64_t generation = this->stream_generation_;
    const size_t usable = length - (length % bytes_per_frame);
    if (usable == 0) {
        return 0;
    }
    const size_t frames_total = usable / bytes_per_frame;

    // Scale the whole buffer up front; commit the ramp only by frames written.
    const uint64_t start = this->current_multiplier_;
    const uint64_t target = this->target_multiplier_.load(std::memory_order_relaxed);
    const uint8_t* src = this->stage_(data, frames_total, start, target);

    size_t frames_done = 0;
    bool first_pass = true;
    while (frames_done < frames_total) {
        if (this->stopping_.load()) {
            break;
        }
        // Always attempt one pass, so a zero timeout still moves whatever already fits.
        if (!first_pass && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        first_pass = false;

        size_t written = 0;
        bool broken = false;
        {
            const MainloopLock ml(this->conn_.mainloop());
            const size_t writable = pa_stream_writable_size(this->stream_);
            if (writable == static_cast<size_t>(-1)) {
                broken = true;
            } else {
                // Whole frames only, so the server's queue never holds half a frame.
                const size_t room = writable - (writable % bytes_per_frame);
                const size_t chunk = std::min(room, (frames_total - frames_done) * bytes_per_frame);
                if (chunk > 0) {
                    if (pa_stream_write(this->stream_, src + (frames_done * bytes_per_frame), chunk,
                                        nullptr, 0, PA_SEEK_RELATIVE) < 0) {
                        broken = true;
                    } else {
                        written = chunk;
                    }
                }
            }
        }
        if (broken) {
            // Recorded only; the next write() spends a recovery attempt on it.
            this->stream_failed_.store(true);
            break;
        }
        if (written > 0) {
            frames_done += written / bytes_per_frame;
            continue;
        }

        // Sliced wait, no predicate: writable size needs the mainloop lock, so recheck below.
        const auto slice =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(WRITE_SLICE_MS);
        this->space_available_.wait_until(lock, std::min(slice, deadline));
        if (this->stream_ == nullptr || this->stream_generation_ != generation) {
            // The stream changed while unlocked; report only what landed.
            break;
        }
    }

    this->current_multiplier_ = ramped_gain(start, target, this->ramp_step_, frames_done);

    int64_t finish_us = 0;
    bool have_timing = false;
    if (frames_done > 0 && this->stream_ != nullptr) {
        // pa_stream_get_latency() is the server's own measure of time until queued audio plays.
        const MainloopLock ml(this->conn_.mainloop());
        pa_usec_t latency_us = 0;
        int negative = 0;
        if (pa_stream_get_latency(this->stream_, &latency_us, &negative) == 0 && negative == 0) {
            finish_us = now_us() + static_cast<int64_t>(latency_us);
            have_timing = true;
        }
    }

    lock.unlock();
    // Outside the lock, so a callback that touches the sink cannot deadlock.
    if (have_timing && this->on_frames_played) {
        this->on_frames_played(static_cast<uint32_t>(frames_done), finish_us);
    }
    return frames_done * bytes_per_frame;
}

void PulseAudioSink::clear() {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    // Safe only because write() shares mutex_; PortAudioSink::clear() must not do the same.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    // A flush ends a parked write()'s stream.
    ++this->stream_generation_;
    this->space_available_.notify_all();

    if (this->stream_ == nullptr) {
        return;
    }
    const MainloopLock ml(this->conn_.mainloop());
    // Fire and forget: the flush is ordered ahead of later writes.
    pa_operation* op = pa_stream_flush(this->stream_, nullptr, nullptr);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
}

void PulseAudioSink::stop() {
    // Before the mutex, so a waiting write() bails out.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    // Before the early return, so a write() after shutdown attempts nothing.
    this->last_format_ = {};
    if (this->stream_ == nullptr) {
        return;
    }
    this->close_stream_();
    // The connection outlives stop(); the destructor releases it.
    cli_log(LogLevel::INFO, "pulse: '%s' closed", this->name().c_str());
}

void PulseAudioSink::poll(int64_t now_ms) {
    // Unlocked fast path: nothing to do on almost every tick.
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

    const StreamFormat format = this->last_format_;
    this->close_stream_();

    // Reconnect on the main loop, bounded by PULSE_RECOVERY_TIMEOUT_MS.
    std::string error;
    if (!this->conn_.connect(error, PULSE_RECOVERY_TIMEOUT_MS)) {
        this->report_failed_recovery_(error);
        return;
    }
    if (!this->open_stream_(format.sample_rate, format.channels, format.bit_depth,
                            PULSE_RECOVERY_TIMEOUT_MS)) {
        this->report_failed_recovery_("the server would not take the stream");
        return;
    }
    if (this->stopping_.load()) {
        // stop() can land during the reconnect above; release the stream now.
        this->recovery_.rescan_done(true);  // shutting down; there is nothing left to retry for
        this->close_stream_();
        return;
    }
    this->recovery_.rescan_done(true);
    cli_log(LogLevel::INFO, "pulse: '%s' is back after reconnecting to %s", this->name().c_str(),
            this->conn_.server_name().c_str());
}

void PulseAudioSink::set_volume(uint8_t volume) {
    this->volume_.store(volume > 100 ? 100 : volume);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "pulse: volume now %u", this->volume_.load());
}

void PulseAudioSink::set_muted(bool muted) {
    this->muted_.store(muted);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "pulse: %s", muted ? "muted" : "unmuted");
}

void PulseAudioSink::stream_state_cb(pa_stream* stream, void* userdata) {
    auto* self = static_cast<PulseAudioSink*>(userdata);
    const pa_stream_state_t state = pa_stream_get_state(stream);
    self->stream_state_.store(static_cast<int>(state));
    if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
        // Recorded only; write() notices and spends the attempt.
        self->stream_failed_.store(true);
        self->space_available_.notify_all();
    }
    self->conn_.notify();
}

void PulseAudioSink::stream_write_cb(pa_stream* /*stream*/, size_t /*nbytes*/, void* userdata) {
    auto* self = static_cast<PulseAudioSink*>(userdata);
    // Notified without the sink's mutex; a missed wakeup costs one WRITE_SLICE_MS.
    self->space_available_.notify_all();
}

void PulseAudioSink::stream_underflow_cb(pa_stream* /*stream*/, void* userdata) {
    auto* self = static_cast<PulseAudioSink*>(userdata);
    // Debug: track boundaries and seeks drain the queue legitimately.
    cli_log(LogLevel::DEBUG, "pulse: '%s' ran dry", self->name().c_str());
}

bool PulseAudioSink::open_stream_(uint32_t sample_rate, uint8_t channels,
                                  uint8_t bits_per_sample, int timeout_ms) {
    pa_sample_format_t format = PA_SAMPLE_INVALID;
    if (!pulse_format_for(bits_per_sample, format)) {
        cli_log(LogLevel::ERROR, "pulse: unsupported bit depth %u", bits_per_sample);
        return false;
    }
    if (channels == 0 || sample_rate == 0) {
        cli_log(LogLevel::ERROR, "pulse: refusing stream with %u ch at %u Hz", channels,
                sample_rate);
        return false;
    }
    if (!this->conn_.ready()) {
        cli_log(LogLevel::ERROR, "pulse: no server connection to open '%s' on",
                this->name().c_str());
        return false;
    }

    pa_sample_spec spec{};
    spec.format = format;
    spec.rate = sample_rate;
    spec.channels = channels;
    if (pa_sample_spec_valid(&spec) == 0) {
        cli_log(LogLevel::ERROR, "pulse: the server cannot carry %u Hz, %u ch, %u-bit", sample_rate,
                channels, bits_per_sample);
        return false;
    }

    // Bump the generation so a write() parked on the previous stream cannot feed this one.
    ++this->stream_generation_;
    this->rate_ = sample_rate;
    this->channels_ = channels;
    this->bits_ = bits_per_sample;
    this->bytes_per_frame_ =
        static_cast<size_t>(channels) * (static_cast<size_t>(bits_per_sample) / 8U);
    this->ramp_step_ = volume_ramp_step(sample_rate);
    // Open at the target gain, never ramping up to a restored volume.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);
    this->stream_failed_.store(false);
    this->stream_state_.store(PA_STREAM_UNCONNECTED);

    // --buffer-ms becomes tlength, the only buffer in this path; the server sizes the rest.
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(
        pa_usec_to_bytes(static_cast<pa_usec_t>(this->buffer_ms_) * 1000, &spec));
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = static_cast<uint32_t>(-1);

    const auto flags = static_cast<pa_stream_flags_t>(
        PA_STREAM_ADJUST_LATENCY | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE);
    const char* device = this->device_.empty() ? nullptr : this->device_.c_str();

    {
        const MainloopLock ml(this->conn_.mainloop());
        this->stream_ = pa_stream_new(this->conn_.context(), PULSE_APP_NAME, &spec, nullptr);
        if (this->stream_ == nullptr) {
            cli_log(LogLevel::ERROR, "pulse: cannot create a stream: %s",
                    pa_strerror(pa_context_errno(this->conn_.context())));
            return false;
        }
        pa_stream_set_state_callback(this->stream_, &PulseAudioSink::stream_state_cb, this);
        pa_stream_set_write_callback(this->stream_, &PulseAudioSink::stream_write_cb, this);
        pa_stream_set_underflow_callback(this->stream_, &PulseAudioSink::stream_underflow_cb, this);

        if (pa_stream_connect_playback(this->stream_, device, &attr, flags, nullptr, nullptr) < 0) {
            cli_log(LogLevel::ERROR, "pulse: cannot play on '%s': %s", this->name().c_str(),
                    pa_strerror(pa_context_errno(this->conn_.context())));
            pa_stream_set_state_callback(this->stream_, nullptr, nullptr);
            pa_stream_set_write_callback(this->stream_, nullptr, nullptr);
            pa_stream_set_underflow_callback(this->stream_, nullptr, nullptr);
            pa_stream_unref(this->stream_);
            this->stream_ = nullptr;
            return false;
        }
    }

    const bool settled = this->conn_.wait_for(
        [this] {
            const auto state = static_cast<pa_stream_state_t>(this->stream_state_.load());
            return state == PA_STREAM_READY || PA_STREAM_IS_GOOD(state) == 0;
        },
        timeout_ms);
    const auto state = static_cast<pa_stream_state_t>(this->stream_state_.load());
    pa_buffer_attr actual_attr = attr;
    if (state == PA_STREAM_READY) {
        const MainloopLock ml(this->conn_.mainloop());
        const pa_buffer_attr* actual = pa_stream_get_buffer_attr(this->stream_);
        if (actual != nullptr) {
            actual_attr = *actual;
        }
    }
    const uint32_t granted = actual_attr.tlength;
    if (!settled || state != PA_STREAM_READY) {
        cli_log(LogLevel::ERROR, "pulse: '%s' would not start at %u Hz, %u ch, %u-bit -- %s",
                this->name().c_str(), sample_rate, channels, bits_per_sample,
                settled ? "the server refused it" : "the server did not answer");
        this->close_stream_();
        return false;
    }

    // Log what the server granted, so an ignored --buffer-ms says what it became.
    if (granted != attr.tlength) {
        cli_log(LogLevel::DEBUG,
                "pulse: --buffer-ms %u asked the server for %u bytes of queue and got %u (%llu ms; "
                "maxlength %u, prebuf %u, minreq %u)",
                this->buffer_ms_, attr.tlength, granted,
                static_cast<unsigned long long>(pa_bytes_to_usec(granted, &spec) / 1000),
                actual_attr.maxlength, actual_attr.prebuf, actual_attr.minreq);
    }

    this->failed_.store(false);
    cli_log(LogLevel::INFO,
            "pulse: '%s' open on %s at %u Hz, %u ch, %u-bit (%zu bytes/frame, %u-byte server "
            "queue)",
            this->name().c_str(), this->conn_.server_name().c_str(), sample_rate, channels,
            bits_per_sample, this->bytes_per_frame_, granted);
    return true;
}

void PulseAudioSink::close_stream_() {
    if (this->stream_ != nullptr && this->conn_.mainloop() == nullptr) {
        // Unreachable today: every path closes the stream before the connection.
        cli_log(LogLevel::ERROR,
                "pulse: '%s' outlived its mainloop -- the stream cannot be released",
                this->name().c_str());
    }
    if (this->stream_ != nullptr && this->conn_.mainloop() != nullptr) {
        const MainloopLock ml(this->conn_.mainloop());
        // Cleared before the disconnect, whose own state change must not reach a dying stream.
        pa_stream_set_state_callback(this->stream_, nullptr, nullptr);
        pa_stream_set_write_callback(this->stream_, nullptr, nullptr);
        pa_stream_set_underflow_callback(this->stream_, nullptr, nullptr);
        pa_stream_disconnect(this->stream_);
        pa_stream_unref(this->stream_);
    }
    this->stream_ = nullptr;

    ++this->stream_generation_;
    this->rate_ = 0;
    this->channels_ = 0;
    this->bits_ = 0;
    this->bytes_per_frame_ = 0;
    this->ramp_step_ = 0;
    this->stream_failed_.store(false);
    this->stream_state_.store(PA_STREAM_UNCONNECTED);
    // Wake a write() parked on the old stream.
    this->space_available_.notify_all();
    // Never clear stopping_ here: a mid-stream configure() would un-latch a shutdown.
}

bool PulseAudioSink::reopen_in_place_() {
    if (this->stopping_.load() || this->last_format_.sample_rate == 0) {
        return false;
    }
    if (!this->recovery_.reopen_due()) {
        return false;
    }
    if (!this->conn_.ready()) {
        // Context down: reconnecting waits on the server, so escalate to poll().
        this->recovery_.reopen_done(false);
        return false;
    }

    const StreamFormat format = this->last_format_;
    this->close_stream_();
    if (!this->open_stream_(format.sample_rate, format.channels, format.bit_depth,
                            PULSE_RECOVERY_TIMEOUT_MS)) {
        this->recovery_.reopen_done(false);  // open_stream_() has already said why, once
        return false;
    }
    this->recovery_.reopen_done(true);

    if (this->stopping_.load()) {
        // stop() can land during the open above.
        this->close_stream_();
        return false;
    }
    cli_log(LogLevel::INFO, "pulse: '%s' recovered without waiting for the next stream",
            this->name().c_str());
    return true;
}

void PulseAudioSink::report_failed_recovery_(const std::string& reason) {
    // Failure buys another attempt after a longer delay.
    this->recovery_.rescan_done(false);
    // Quiet while attempts remain, loud once the budget is gone.
    const bool retrying = this->recovery_.pending();
    cli_log(retrying ? LogLevel::DEBUG : LogLevel::WARN, "pulse: '%s' is not back: %s%s",
            this->name().c_str(), reason.c_str(),
            retrying ? " -- trying again shortly" : " -- discarding until the next stream");
}

bool PulseAudioSink::stream_alive_() const {
    return this->stream_ != nullptr && this->bytes_per_frame_ != 0 && this->conn_.ready() &&
           !this->stream_failed_.load();
}

const uint8_t* PulseAudioSink::stage_(const uint8_t* data, size_t frames, uint64_t start,
                                      uint64_t target) {
    const bool scaling = (start != target) || (target != Q32_ONE);
    // 8-bit always needs the copy to flip the sign bit.
    const bool converting = this->bits_ == 8;
    if (!scaling && !converting) {
        return data;
    }

    const size_t bytes = frames * this->bytes_per_frame_;
    this->scratch_.assign(data, data + bytes);
    if (scaling) {
        apply_volume_ramp(this->scratch_.data(), bytes, this->bits_ / 8U, this->channels_, start,
                          target, this->ramp_step_);
    }
    if (converting) {
        for (uint8_t& sample : this->scratch_) {
            sample = static_cast<uint8_t>(sample ^ 0x80U);
        }
    }
    return this->scratch_.data();
}

void PulseAudioSink::update_target_multiplier_() {
    this->target_multiplier_.store(q32_gain_for(this->volume_.load(), this->muted_.load()),
                                   std::memory_order_relaxed);
}

}  // namespace sendspin_cli
