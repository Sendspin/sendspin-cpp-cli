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

/// How long write() sleeps before asking the server for room again, in milliseconds.
///
/// The write callback wakes it sooner in the ordinary case; this only bounds the wait when that
/// notification is missed, which the callback's lock-free notify makes possible. Short for the
/// reason AlsaAudioSink slices its snd_pcm_wait(): a parked writer must notice stopping_.
constexpr int WRITE_SLICE_MS = 10;

/// The application name the server shows in its mixer, and one of the reasons this backend
/// exists at all -- through ALSA's plugin PCM every stream is just "ALSA plug-in".
constexpr const char* PULSE_APP_NAME = "sendspin-cli";

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// How each of PROBE_BIT_DEPTHS maps onto a PulseAudio sample format, in the same order.
///
/// 8-bit is the odd one: the player emits **signed** 8-bit and PulseAudio has no signed 8-bit
/// format at all, so it goes out as PA_SAMPLE_U8 with the sign bit flipped on the way (see
/// stage_()). Refusing 8-bit instead would leave a stream ALSA and PortAudio play and this
/// backend does not, which is a difference with no cause.
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

/// @brief Holds the libpulse mainloop lock for a scope. Every pa_* call on a live context needs it.
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

/// What one asynchronous server query has in common with the next.
///
/// `done` is atomic because the waiter reads it outside the mainloop lock; everything a callback
/// writes before finish() is published by that store and read after the matching load.
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

/// Waits for one query to complete, and cancels it if the server does not answer in time.
///
/// The cancel is what makes the deadline safe rather than merely convenient: without it a late
/// callback would write through a `userdata` pointer whose query has already gone out of scope.
/// pa_operation_cancel() guarantees the callback is not called afterwards.
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

// ============================================================================
// PulseConnection
// ============================================================================

PulseConnection::~PulseConnection() {
    this->disconnect();
}

void PulseConnection::state_cb(pa_context* context, void* userdata) {
    auto* self = static_cast<PulseConnection*>(userdata);
    self->state_.store(pa_context_get_state(context), std::memory_order_release);
    self->notify();
}

void PulseConnection::notify() {
    // Taken briefly so a waiter that has evaluated its predicate and not yet slept cannot miss
    // this wakeup. Held only for the notify; the mainloop thread never blocks on it.
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

    // Set before the mainloop is started, so no state change can happen between the connect below
    // and the wait after it without a callback recording it.
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
        // Stopped before the context is touched: pa_context_disconnect() is not safe to call while
        // the mainloop thread may be running a callback on the same context.
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

// ============================================================================
// PulseAudioSink
// ============================================================================

PulseAudioSink::PulseAudioSink(std::string device, uint32_t buffer_ms)
    : device_(std::move(device)), buffer_ms_(buffer_ms) {
    std::string error;
    if (!this->conn_.connect(error)) {
        // Reported rather than thrown: make_audio_sink() has already run probe(), so getting here
        // means the server answered once and then would not. configure() reconnects, and until it
        // does the sink degrades to discarding -- which beats taking the daemon down.
        cli_log(LogLevel::ERROR, "pulse: %s", error.c_str());
    }
}

PulseAudioSink::~PulseAudioSink() {
    // stop() is the documented shutdown path, but a sink destroyed without it must still hand the
    // stream back -- and must free it before conn_ tears the mainloop down under it.
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
        // No sink named, so a reachable server is the whole of what there is to check: which sink
        // the server calls default is its business, and it may change before the first stream.
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

    // Printed once rather than under every sink, because the answer really is the same for all of
    // them: the server converts, so what a sink accepts is not what the hardware behind it runs at.
    std::fprintf(out, "\n  Every sink above accepts all of:\n");
    print_sink_capabilities(out, SinkCapabilities::permissive(), PROBE_FORMAT_NAMES);
}

bool PulseAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    if (this->stopping_.load()) {
        cli_log(LogLevel::DEBUG, "pulse: ignoring a stream start during shutdown");
        return false;
    }

    // Remembered before anything can fail: this is the format the player is about to send audio in
    // whether or not the server takes it, so it is also the only format recovery may reopen at.
    this->last_format_ = {sample_rate, channels, bits_per_sample};

    this->close_stream_();
    if (!this->conn_.ready()) {
        // A configure() is the natural moment to pay for a reconnect: it already runs on the main
        // loop, between streams, where a wait costs nobody's audio.
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
    // Both attempts go back in hand, and only here: a stream that is really running is what a
    // budget spent on the last one was waiting for.
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
        // No stream to feed, and none to be had: swallow the audio rather than return 0 forever,
        // which would spin the sync task on a buffer it can never hand off.
        if (!this->failed_.exchange(true)) {
            cli_log(LogLevel::ERROR,
                    "pulse: '%s' is not playing -- discarding audio until a stream reconfigures it",
                    this->name().c_str());
        }
        // Frame-aligned per the write() contract wherever the frame size is still known -- from
        // the remembered format once close_stream_() has zeroed bytes_per_frame_, which is the
        // same figure it held.
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
        // Less than one whole frame: consuming it would mean returning a mid-frame count, which
        // the contract forbids.
        return 0;
    }
    const size_t frames_total = usable / bytes_per_frame;

    // Scaled once, up front, for the whole buffer -- but the ramp is committed below by the frames
    // actually *written*, because the loop can break out with frames left over and the sync task
    // re-presents that tail on its next call.
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
                // Rounded down to a frame, so no remainder byte is ever left mid-frame in the
                // server's queue -- the next write would then start half a frame late.
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
            // The server has dropped the stream. Recorded rather than recovered here: the next
            // write() sees a dead stream and spends a recovery attempt on it.
            this->stream_failed_.store(true);
            break;
        }
        if (written > 0) {
            frames_done += written / bytes_per_frame;
            continue;
        }

        // No room. Waiting releases mutex_, so configure(), clear() and stop() still run.
        this->space_available_.wait_for(
            lock, std::chrono::milliseconds(WRITE_SLICE_MS), [this, generation] {
                return this->stopping_.load() || this->stream_ == nullptr ||
                       this->stream_generation_ != generation;
            });
        if (this->stream_ == nullptr || this->stream_generation_ != generation) {
            // Closed, reopened or flushed while the mutex was released. What is left of this
            // buffer belongs to a stream that is gone, so report only what really landed.
            break;
        }
    }

    // Committed by what was really written, not by what was scaled, so the gain never runs ahead of
    // the audio. Recomputed through ramped_gain() so there is one definition of the arithmetic.
    this->current_multiplier_ = ramped_gain(start, target, this->ramp_step_, frames_done);

    int64_t finish_us = 0;
    bool have_timing = false;
    if (frames_done > 0 && this->stream_ != nullptr) {
        // Sync feedback, and the reason this backend exists: pa_stream_get_latency() is how long
        // it will be until what is queued has played out, measured by the *server* -- so the
        // frames just written finish that far into the future. Through ALSA's plugin PCM the same
        // question is answered by the plugin's own buffering instead.
        const MainloopLock ml(this->conn_.mainloop());
        pa_usec_t latency_us = 0;
        int negative = 0;
        if (pa_stream_get_latency(this->stream_, &latency_us, &negative) == 0 && negative == 0) {
            finish_us = now_us() + static_cast<int64_t>(latency_us);
            have_timing = true;
        }
    }

    lock.unlock();
    // Fired outside the lock: notify_audio_played() runs the player's own bookkeeping, and holding
    // the sink's mutex across a callback is how a future callback that touches the sink would
    // deadlock.
    if (have_timing && this->on_frames_played) {
        this->on_frames_played(static_cast<uint32_t>(frames_done), finish_us);
    }
    return frames_done * bytes_per_frame;
}

void PulseAudioSink::clear() {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    // Snapped before the early return, so a flush with no stream open still leaves the gain where
    // the next stream should start. Safe here only because this sink serialises write() through
    // the same mutex -- PortAudioSink deliberately does not do this, because its callback keeps
    // running through a flush. The discriminator is the threading model, not the audio.
    //
    // A write() parked on the wait below *can* wake and commit its own ramp over this snap, which
    // is harmless in both directions: mid-stream, carrying the ramp on is what should be heard
    // anyway, and at a stream end open_stream_() snaps it again before a sample is played.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    // A flush ends this buffer's stream as far as a parked write() is concerned: whatever it still
    // holds is audio the player has just asked us to drop.
    ++this->stream_generation_;
    this->space_available_.notify_all();

    if (this->stream_ == nullptr) {
        return;
    }
    const MainloopLock ml(this->conn_.mainloop());
    // Fire and forget: the flush is ordered on the stream ahead of anything written after it, so
    // there is nothing to wait for and nothing a failure here would let us do differently.
    pa_operation* op = pa_stream_flush(this->stream_, nullptr, nullptr);
    if (op != nullptr) {
        pa_operation_unref(op);
    }
}

void PulseAudioSink::stop() {
    // Latched before the mutex is taken so a write() already waiting for room sees it and bails
    // out, instead of making shutdown wait for the server to drain.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    // Forgotten before the early return below: with no format remembered there is nothing recovery
    // can reopen at, so a write() arriving after shutdown attempts nothing.
    this->last_format_ = {};
    if (this->stream_ == nullptr) {
        return;
    }
    this->close_stream_();
    // The connection and its mainloop thread deliberately outlive this, and are released by the
    // destructor: AudioSink::stop() releases the *device*, and a context with no stream on it
    // holds none. PipeWireSink::stop() does take its loop down, because there the loop is what a
    // failed configure() can leave running with nothing else to release it. main() stops and then
    // destroys, so neither costs anything at shutdown.
    cli_log(LogLevel::INFO, "pulse: '%s' closed", this->name().c_str());
}

void PulseAudioSink::poll(int64_t now_ms) {
    // Both read without the lock, and first, because on all but a handful of ticks in a run there
    // is nothing to do and no reason to contend with a write() for the mutex.
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

    // The expensive half of recovery, on the main loop for the reason PortAudioSink's device
    // rescan is: a reconnect waits on the server, and up to PULSE_TIMEOUT_MS of that on the sync
    // task's thread would come out of a write()'s own budget. What is delayed here is the dispatch
    // of messages already received, which the library gives seconds of slack.
    //
    // It is also the attempt that a *restarted* daemon needs, which is what makes SINK_RESCAN_
    // DELAY_MS the right gap rather than an arbitrary one: the socket is gone for a moment and
    // comes back, so an immediate retry fails where one two seconds later succeeds.
    std::string error;
    if (!this->conn_.connect(error, PULSE_RECOVERY_TIMEOUT_MS)) {
        cli_log(LogLevel::WARN,
                "pulse: '%s' is still gone -- discarding until the next stream (%s)",
                this->name().c_str(), error.c_str());
        return;
    }
    if (!this->open_stream_(format.sample_rate, format.channels, format.bit_depth,
                            PULSE_RECOVERY_TIMEOUT_MS)) {
        return;  // open_stream_() has already said why, once
    }
    if (this->stopping_.load()) {
        // stop() latches before it takes mutex_, so it can arrive while the reconnect above is
        // running. Hand the stream straight back rather than leave a live one for the destructor.
        this->close_stream_();
        return;
    }
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
        // The server killed the stream -- it went away, or the sink did. Recorded rather than
        // acted on: write() is what notices, and it is the thread that may spend the attempt.
        self->stream_failed_.store(true);
        self->space_available_.notify_all();
    }
    self->conn_.notify();
}

void PulseAudioSink::stream_write_cb(pa_stream* /*stream*/, size_t /*nbytes*/, void* userdata) {
    auto* self = static_cast<PulseAudioSink*>(userdata);
    // Notified without the sink's mutex, which is what keeps this callback off the mainloop's
    // critical path. It leaves a window in which a writer that has just evaluated its predicate,
    // and not yet slept, misses the wakeup -- harmless, because that wait is sliced.
    self->space_available_.notify_all();
}

void PulseAudioSink::stream_underflow_cb(pa_stream* /*stream*/, void* userdata) {
    auto* self = static_cast<PulseAudioSink*>(userdata);
    // Debug rather than warn: a track boundary or a seek drains the queue legitimately, so this
    // fires in ordinary use and a louder level would train an operator to ignore it.
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

    // The format fields go in before the stream can run, and the generation with them: a write()
    // parked on the previous stream must not feed this one.
    ++this->stream_generation_;
    this->rate_ = sample_rate;
    this->channels_ = channels;
    this->bits_ = bits_per_sample;
    this->bytes_per_frame_ =
        static_cast<size_t>(channels) * (static_cast<size_t>(bits_per_sample) / 8U);
    this->ramp_step_ = volume_ramp_step(sample_rate);
    // A stream opens at the gain it is meant to be at, never ramping up to it: a restored volume
    // reaches set_volume() before anything has played, so without this the run's first track would
    // open with a fade from a gain that was never applied to a sample.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);
    this->stream_failed_.store(false);
    this->stream_state_.store(PA_STREAM_UNCONNECTED);

    // --buffer-ms becomes the server's own queue, which is the only buffer in this path -- and
    // tlength is exactly what pa_stream_get_latency() then answers about. The other fields are
    // left to the server ((uint32_t)-1 means "your default"), because ADJUST_LATENCY below makes
    // it size them around tlength.
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

    // --buffer-ms is a request, not a promise: the server sizes the queue around it and its own
    // minimum wins. Naming what it landed on is the difference between "your number was ignored"
    // and knowing what to ask for instead -- and the other three fields go with it because they
    // are what a stall at the start of a stream is diagnosed from: prebuf is how much must be
    // queued before a note is heard, and minreq how much the server asks for at a time.
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
        // Unreachable as the code stands -- every path closes the stream before the connection
        // goes -- but the guard below admits that the ordering could change, and a stream that
        // outlives its mainloop cannot be unref'd. Said out loud rather than dropped silently,
        // because the next symptom would be a leak with nothing pointing at its cause.
        cli_log(LogLevel::ERROR,
                "pulse: '%s' outlived its mainloop -- the stream cannot be released",
                this->name().c_str());
    }
    if (this->stream_ != nullptr && this->conn_.mainloop() != nullptr) {
        const MainloopLock ml(this->conn_.mainloop());
        // Cleared before the disconnect, so a state change the disconnect itself fires cannot
        // reach a sink that is already tearing this stream down.
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
    // A write() parked on the old stream would otherwise wait out its whole slice; its predicate
    // reads stream_, so waking it here lets it return promptly instead.
    this->space_available_.notify_all();
    // stopping_ is deliberately untouched: only stop() and the destructor latch it, and clearing
    // it here would let a mid-stream configure() un-latch a shutdown in progress.
}

bool PulseAudioSink::reopen_in_place_() {
    if (this->stopping_.load() || this->last_format_.sample_rate == 0) {
        // Nothing worth recovering, or nothing to recover to. Read before the attempt is spent, so
        // neither costs the outage anything.
        return false;
    }
    if (!this->recovery_.reopen_due()) {
        return false;
    }
    if (!this->conn_.ready()) {
        // The context itself has dropped, and reconnecting waits on the server -- which is not
        // this thread's to pay for. Reported as a failed attempt so it escalates to poll(), where
        // the expensive half lives.
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
        // stop() latches before it takes mutex_, so it can arrive while the open above is running.
        this->close_stream_();
        return false;
    }
    cli_log(LogLevel::INFO, "pulse: '%s' recovered without waiting for the next stream",
            this->name().c_str());
    return true;
}

bool PulseAudioSink::stream_alive_() const {
    return this->stream_ != nullptr && this->bytes_per_frame_ != 0 && this->conn_.ready() &&
           !this->stream_failed_.load();
}

const uint8_t* PulseAudioSink::stage_(const uint8_t* data, size_t frames, uint64_t start,
                                      uint64_t target) {
    const bool scaling = (start != target) || (target != Q32_ONE);
    // 8-bit always needs the copy: the player emits signed samples and PulseAudio has no signed
    // 8-bit format, so they go out as U8 with the sign bit flipped.
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
