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

#include "pipewire_sink.h"

#include "log.h"
#include "pcm_volume.h"

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/support/log.h>
#include <spa/utils/dict.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// The name this stream carries in the graph, and one of the reasons the backend exists at all --
/// through ALSA's plugin PCM every stream is just "ALSA plug-in".
constexpr const char* PIPEWIRE_APP_NAME = "sendspin-cli";

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// How each of PROBE_BIT_DEPTHS maps onto a SPA audio format, in the same order.
///
/// All four are exactly what the player emits -- interleaved, signed, little-endian, and 24-bit
/// packed into three bytes rather than padded into four. PipeWire converts from there.
constexpr std::array<spa_audio_format, PROBE_BIT_DEPTHS.size()> PROBE_FORMATS{
    SPA_AUDIO_FORMAT_S8, SPA_AUDIO_FORMAT_S16_LE, SPA_AUDIO_FORMAT_S24_LE, SPA_AUDIO_FORMAT_S32_LE};

/// How this backend spells PROBE_BIT_DEPTHS, in the same order: SPA's own names.
constexpr std::array<const char*, PROBE_BIT_DEPTHS.size()> PROBE_FORMAT_NAMES{"S8", "S16_LE",
                                                                              "S24_LE", "S32_LE"};

/// Maps the stream's bit depth onto the SPA format it is carried in.
bool spa_format_for(uint8_t bits_per_sample, spa_audio_format& format) {
    for (size_t i = 0; i < PROBE_BIT_DEPTHS.size(); ++i) {
        if (PROBE_BIT_DEPTHS[i] == bits_per_sample) {
            format = PROBE_FORMATS[i];
            return true;
        }
    }
    return false;
}

/// @brief Holds the thread-loop lock for a scope. Every pw_stream_* call from another thread
/// needs it -- though it deliberately does *not* hold off the realtime data thread, which is why
/// the ordering invariant in the header is about the stream being disconnected rather than locked.
class LoopLock {
public:
    explicit LoopLock(pw_thread_loop* loop) : loop_(loop) {
        pw_thread_loop_lock(this->loop_);
    }
    ~LoopLock() {
        pw_thread_loop_unlock(this->loop_);
    }

    LoopLock(const LoopLock&) = delete;
    LoopLock& operator=(const LoopLock&) = delete;

private:
    pw_thread_loop* loop_;
};

/// One audio sink node as the graph describes it.
struct PipeWireNode {
    std::string name;
    std::string description;
};

/// The registry walk's state, shared between its callbacks and the thread waiting on them.
struct RegistryWalk {
    pw_thread_loop* loop{nullptr};
    std::vector<PipeWireNode> nodes;
    int sync_seq{0};
    bool done{false};
    bool failed{false};
    /// Why it failed, because a daemon that refuses is not a daemon that is slow. Without it the
    /// only string left to report is the timeout below -- which is the one thing that did not
    /// happen. Written by core_error_cb, read under the loop lock like every other field here.
    std::string error_message;
};

void registry_global_cb(void* data, uint32_t /*id*/, uint32_t /*permissions*/, const char* type,
                        uint32_t /*version*/, const struct spa_dict* props) {
    auto* walk = static_cast<RegistryWalk*>(data);
    if (props == nullptr || type == nullptr || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
    }
    const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    // Audio/Sink is what a stream can be routed to. Audio/Source and the rest are nodes -o cannot
    // play through, so listing them would name devices that do not work.
    if (media_class == nullptr || std::strcmp(media_class, "Audio/Sink") != 0) {
        return;
    }
    const char* node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (node_name == nullptr) {
        return;  // nothing -o could name
    }
    const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (description == nullptr) {
        description = spa_dict_lookup(props, PW_KEY_NODE_NICK);
    }
    walk->nodes.push_back({node_name, (description != nullptr) ? description : ""});
}

void core_done_cb(void* data, uint32_t id, int seq) {
    auto* walk = static_cast<RegistryWalk*>(data);
    // The round trip we asked for has come back, so every global that existed when it was sent has
    // already been reported. That is what makes one pass over the registry complete rather than
    // merely likely.
    if (id == PW_ID_CORE && seq == walk->sync_seq) {
        walk->done = true;
        pw_thread_loop_signal(walk->loop, false);
    }
}

void core_error_cb(void* data, uint32_t id, int /*seq*/, int res, const char* message) {
    auto* walk = static_cast<RegistryWalk*>(data);
    if (id == PW_ID_CORE) {
        // Kept rather than logged and dropped: walk_registry()'s caller is what puts this in
        // front of somebody, at startup and in -l alike.
        walk->error_message =
            (message != nullptr && *message != '\0') ? message : spa_strerror(res);
        walk->failed = true;
        walk->done = true;
        pw_thread_loop_signal(walk->loop, false);
    }
}

// Designated initialisers rather than positional ones, because both of these structs are long
// and mostly nulls: a field inserted upstream would silently shift a positional table's callbacks
// onto the wrong slots, and the compiler would only notice where the signatures happened to differ.
constexpr pw_registry_events REGISTRY_EVENTS = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global_cb,
    .global_remove = nullptr,
};

constexpr pw_core_events CORE_EVENTS = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = nullptr,
    .done = core_done_cb,
    .ping = nullptr,
    .error = core_error_cb,
    .remove_id = nullptr,
    .bound_id = nullptr,
    .add_mem = nullptr,
    .remove_mem = nullptr,
// .bound_props arrived with PW_VERSION_CORE_EVENTS 1, which is after this project's 0.3.64 floor
// (bookworm ships 0.3.65, where pw_core_events still ends at remove_mem). Neither half of the
// obvious answer works on its own: naming it unconditionally is a compile error on an 0.3.6x
// header, and omitting it unconditionally is -Wmissing-field-initializers on a 1.x one, which
// SENDSPIN_CLI_WERROR makes fatal on every CI leg but pipewire-minimum. The version macro is the
// only thing true on both, so it is what the list is cut to. A future member gets the same
// treatment, and the same warning is what will point it out.
#if PW_VERSION_CORE_EVENTS >= 1
    .bound_props = nullptr,
#endif
};

/// Connects to the daemon, walks its registry once, and reports every audio sink node.
///
/// Everything it creates lives and dies inside this call: a thread loop of its own, a context, a
/// core and a registry. That is what lets probe() and list_devices() be static, and it keeps the
/// enumeration entirely out of the path of a sink that is playing.
/// @param error Set to a human-readable reason when the return value is false.
bool walk_registry(std::vector<PipeWireNode>& out, std::string& error) {
    pw_thread_loop* loop = pw_thread_loop_new("sendspin-pw-scan", nullptr);
    if (loop == nullptr) {
        error = "cannot create a PipeWire loop";
        return false;
    }
    if (pw_thread_loop_start(loop) < 0) {
        error = "cannot start the PipeWire loop thread";
        pw_thread_loop_destroy(loop);
        return false;
    }

    RegistryWalk walk;
    walk.loop = loop;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    spa_hook core_hook{};
    spa_hook registry_hook{};
    bool answered = false;
    bool refused = false;
    std::string refusal;

    {
        const LoopLock lock(loop);
        context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
        if (context != nullptr) {
            core = pw_context_connect(context, nullptr, 0);
        }
        if (core != nullptr) {
            pw_core_add_listener(core, &core_hook, &CORE_EVENTS, &walk);
            registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        }
        if (registry != nullptr) {
            pw_registry_add_listener(registry, &registry_hook, &REGISTRY_EVENTS, &walk);
            // A round trip, so the wait below ends when the daemon has finished reporting rather
            // than after an arbitrary pause.
            walk.sync_seq = pw_core_sync(core, PW_ID_CORE, 0);
            while (!walk.done) {
                if (pw_thread_loop_timed_wait(loop, PIPEWIRE_TIMEOUT_S) != 0) {
                    break;
                }
            }
            answered = walk.done && !walk.failed;
            // Copied out under the loop lock along with everything else this function reads from
            // the walk: once the lock is dropped the loop thread may still be running callbacks.
            refused = walk.failed;
            refusal = walk.error_message;
        }
    }

    if (context == nullptr) {
        error = "cannot create a PipeWire context";
    } else if (core == nullptr) {
        error = "cannot reach a PipeWire daemon";
    } else if (registry == nullptr) {
        // Connected, but the daemon would not hand out a registry -- a permissions answer rather
        // than an absence, so it must not be reported as a timeout nobody waited for.
        error = "the PipeWire daemon would not open its registry";
    } else if (refused) {
        // The same distinction one branch further in: the daemon answered, and answered with a
        // reason. It arrived immediately, so the timeout below would name a wait that never
        // happened and bury a permission or protocol refusal behind it.
        error = "the PipeWire daemon refused the registry walk: " + refusal;
    } else if (!answered) {
        error = "the PipeWire daemon did not finish listing its nodes within " +
                std::to_string(PIPEWIRE_TIMEOUT_S) + " s";
    }

    {
        const LoopLock lock(loop);
        if (registry != nullptr) {
            spa_hook_remove(&registry_hook);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
        }
        if (core != nullptr) {
            spa_hook_remove(&core_hook);
            pw_core_disconnect(core);
        }
        if (context != nullptr) {
            pw_context_destroy(context);
        }
    }
    pw_thread_loop_destroy(loop);

    if (!answered) {
        return false;
    }
    out = std::move(walk.nodes);
    return true;
}

}  // namespace

size_t pipewire_ring_frames(uint32_t rate, uint32_t buffer_ms) {
    const auto by_time = static_cast<size_t>((static_cast<uint64_t>(rate) * buffer_ms) / 1000);
    return std::max(by_time, MIN_RING_FRAMES);
}

PipeWireQuantumFit pipewire_quantum_fit(size_t ring_frames, uint32_t quantum, uint32_t rate) {
    PipeWireQuantumFit fit;
    if (quantum == 0 || rate == 0) {
        // Nothing has run a cycle yet, so there is no quantum to be measured against -- and
        // reporting a fault here would mean warning about every stream before its first callback.
        return fit;
    }
    const size_t floor_frames = static_cast<size_t>(quantum) * RING_QUANTUM_MULTIPLE;
    fit.starves = ring_frames < quantum;
    fit.tight = !fit.starves && ring_frames < floor_frames;
    // Rounded up, because the whole use of this figure is to clear the floor when it is passed
    // back in as --buffer-ms: rounding down would hand back a number that fails the same check.
    fit.recommended_buffer_ms =
        static_cast<uint32_t>(((static_cast<uint64_t>(floor_frames) * 1000) + rate - 1) / rate);
    return fit;
}

// ============================================================================
// PipeWireGuard
// ============================================================================

PipeWireGuard::PipeWireGuard() {
    pw_init(nullptr, nullptr);
    // libpipewire writes to stderr through a logger of its own, outside this player's: a host that
    // has the library but no daemon running greets every -l with a "can't load config client.conf"
    // banner nothing here asked for, and the failure is already reported properly below. Left on
    // at debug, where the library's own view of the graph is exactly what someone is after.
    pw_log_set_level(sendspin::SendspinClient::get_log_level() >= sendspin::LogLevel::DEBUG
                         ? SPA_LOG_LEVEL_DEBUG
                         : SPA_LOG_LEVEL_NONE);
}

PipeWireGuard::~PipeWireGuard() {
    pw_deinit();
}

// ============================================================================
// PipeWireSink
// ============================================================================

PipeWireSink::PipeWireSink(std::string device, uint32_t buffer_ms)
    : device_(std::move(device)), buffer_ms_(buffer_ms) {}

PipeWireSink::~PipeWireSink() {
    // stop() is the documented shutdown path, but a sink destroyed without it must still hand the
    // stream back -- and must do so before pw_ deinitialises libpipewire under it.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->close_stream_();
    this->stop_loop_();
}

std::string PipeWireSink::name() const {
    return this->device_.empty() ? "pipewire" : "pipewire:" + this->device_;
}

SinkCapabilities PipeWireSink::capabilities() const {
    return SinkCapabilities::permissive();
}

bool PipeWireSink::probe(const std::string& device, std::string& error) {
    const PipeWireGuard guard;

    std::vector<PipeWireNode> nodes;
    if (!walk_registry(nodes, error)) {
        error += " -- run with -l to see what this host has, or -o alsa:pipewire to reach the same "
                 "graph through ALSA's plugin PCM";
        return false;
    }
    if (device.empty()) {
        // No node named, so a reachable daemon is the whole of what there is to check: where the
        // graph routes a Playback stream by default is its business, and it may change before the
        // first stream.
        return true;
    }

    const bool found = std::any_of(nodes.begin(), nodes.end(),
                                   [&device](const PipeWireNode& n) { return n.name == device; });
    if (!found) {
        error = "-o pipewire:" + device +
                ": this graph has no audio sink node by that name -- run with -l to list them";
        return false;
    }
    return true;
}

void PipeWireSink::list_devices(std::FILE* out) {
    const PipeWireGuard guard;

    std::vector<PipeWireNode> nodes;
    std::string error;
    if (!walk_registry(nodes, error)) {
        std::fprintf(out, "  (%s)\n", error.c_str());
        return;
    }
    if (nodes.empty()) {
        std::fprintf(out, "  (this graph has no audio sink nodes)\n");
        return;
    }

    for (const PipeWireNode& node : nodes) {
        std::fprintf(out, "  %s\n", node.name.c_str());
        if (!node.description.empty()) {
            std::fprintf(out, "      %s\n", node.description.c_str());
        }
    }

    // Printed once rather than under every node, because the answer really is the same for all of
    // them: PipeWire puts an adapter in front of the stream and converts, so what a node accepts
    // is not what the hardware behind it runs at.
    std::fprintf(out, "\n  Every node above accepts all of:\n");
    print_sink_capabilities(out, SinkCapabilities::permissive(), PROBE_FORMAT_NAMES);
}

bool PipeWireSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    if (this->stopping_.load()) {
        cli_log(LogLevel::DEBUG, "pipewire: ignoring a stream start during shutdown");
        return false;
    }

    // Remembered before anything can fail: this is the format the player is about to send audio in
    // whether or not the graph takes it, so it is also the only format recovery may reopen at.
    this->last_format_ = {sample_rate, channels, bits_per_sample};

    this->close_stream_();
    if (!this->open_stream_(sample_rate, channels, bits_per_sample, PIPEWIRE_STREAM_TIMEOUT_MS)) {
        this->failed_.store(true);
        return false;
    }
    // Both attempts go back in hand, and only here: a stream that is really running is what a
    // budget spent on the last one was waiting for.
    this->recovery_.reset();
    return true;
}

size_t PipeWireSink::write(const uint8_t* data, size_t length, uint32_t timeout_ms) {
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
                    "pipewire: '%s' is not playing -- discarding audio until a stream "
                    "reconfigures it",
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
    // actually *pushed*, because the loop can break out with frames left over and the sync task
    // re-presents that tail on its next call.
    const uint64_t start = this->current_multiplier_;
    const uint64_t target = this->target_multiplier_.load(std::memory_order_relaxed);
    const uint8_t* src = this->stage_(data, frames_total, start, target);

    size_t done = 0;
    while (done < usable) {
        if (this->stopping_.load()) {
            break;
        }

        const size_t remaining = usable - done;
        size_t room = this->ring_.free_space();
        if (room < remaining) {
            // Rounded down to a frame boundary, so no remainder byte is ever left in the ring: the
            // process callback counts frames by integer division, and an untracked remainder makes
            // it report more frames played than were handed over.
            room -= room % bytes_per_frame;
        }
        if (room > 0) {
            const size_t written = this->ring_.write(src + done, std::min(remaining, room));
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
            // Closed, reopened or flushed while the mutex was released. What is left of this
            // buffer belongs to a stream that is gone, so report only what really landed.
            break;
        }
    }

    // Committed by what was really pushed, not by what was scaled, so the gain never runs ahead of
    // the audio. Recomputed through ramped_gain() so there is one definition of the arithmetic.
    this->current_multiplier_ =
        ramped_gain(start, target, this->ramp_step_, done / bytes_per_frame);
    return done;
}

void PipeWireSink::clear() {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    // A flush ends this buffer's stream as far as a parked write() is concerned: whatever it still
    // holds is audio the player has just asked us to drop.
    ++this->stream_generation_;
    this->space_available_.notify_all();

    // current_multiplier_ is deliberately *not* snapped here, for PortAudioSink::clear()'s reason:
    // the process callback keeps running through a flush, and a mid-stream flush is followed by
    // more audio through the same stream, so a ramp in progress should carry on being heard. It is
    // the ring rather than the callback that this sink's gain is applied ahead of, but the
    // conclusion is the same -- and at a stream *end* the next configure() snaps it anyway.

    if (this->stream_alive_()) {
        // The consumer owns the read position, so the drain has to happen on its side; it takes
        // effect on the callback's next read.
        this->ring_.request_clear();
        return;
    }
    // Nothing is running to carry out a deferred clear, and leaving one pending would play this
    // stream's tail when the next one starts. No consumer means dropping here is safe.
    this->ring_.drop();
}

void PipeWireSink::stop() {
    // Latched before the mutex is taken so a write() already blocked on a full ring sees it and
    // bails out, instead of making shutdown wait for the graph to drain.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    // Forgotten first: with no format remembered there is nothing recovery can reopen at, so a
    // write() arriving after shutdown attempts nothing.
    this->last_format_ = {};
    const bool was_open = this->stream_ != nullptr;
    // Both run whether or not a stream was open, unlike the sinks that have only one thing to
    // release: a configure() whose stream failed still left the loop thread running, and only
    // this and the destructor take it down.
    this->close_stream_();
    this->stop_loop_();
    if (was_open) {
        cli_log(LogLevel::INFO, "pipewire: '%s' closed", this->name().c_str());
    }
}

void PipeWireSink::poll(int64_t now_ms) {
    // The quantum is only knowable once the graph has run a cycle, so the floor it puts under
    // --buffer-ms is reported from here rather than at open time -- and from here rather than from
    // process() itself, which must not stop to format a log line on the realtime data thread.
    const uint32_t quantum = this->quantum_frames_.load(std::memory_order_relaxed);
    if (quantum != 0 && !this->quantum_logged_.exchange(true, std::memory_order_relaxed)) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->bytes_per_frame_ != 0) {
            const size_t ring_frames = (this->ring_.free_space() + this->ring_.available()) /
                                       this->bytes_per_frame_;
            const PipeWireQuantumFit fit = pipewire_quantum_fit(ring_frames, quantum, this->rate_);
            if (fit.starves) {
                // Broken rather than merely tight, and not something this player can negotiate
                // its way out of: reaching here means the PW_KEY_NODE_LATENCY asked for at open
                // time was overridden, so the only lever left is the one the operator holds.
                // Loud for that reason -- every cycle of this stream will zero-fill.
                cli_log(LogLevel::WARN,
                        "pipewire: '%s' will underrun on every cycle -- the graph is running "
                        "%u-frame quanta and --buffer-ms %u is only a %zu-frame ring; pass "
                        "--buffer-ms %u or more",
                        this->name().c_str(), quantum, this->buffer_ms_, ring_frames,
                        fit.recommended_buffer_ms);
            } else {
                cli_log(LogLevel::DEBUG,
                        "pipewire: the graph is running %u-frame quanta; --buffer-ms %u is a "
                        "%zu-frame ring%s",
                        quantum, this->buffer_ms_, ring_frames,
                        fit.tight ? " -- under three quanta, so it may starve on a busy graph"
                                  : "");
            }
        }
    }

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
    // The loop goes too, which is what makes this the *second* attempt rather than a repeat of the
    // first: a daemon that restarted took the connection behind this loop with it, and only a new
    // one reaches the daemon that came back.
    this->stop_loop_();

    if (!this->open_stream_(format.sample_rate, format.channels, format.bit_depth,
                            PIPEWIRE_RECOVERY_TIMEOUT_MS)) {
        // Reported as a failure, which is what buys another attempt behind a longer delay: a
        // daemon still down now may be up in a few seconds, and that is the whole difference
        // between this backend's second attempt and PortAudio's one-shot device rescan.
        this->recovery_.rescan_done(false);
        // open_stream_() has already said what went wrong, once and at ERROR; this only says what
        // happens next -- quiet while attempts remain, because there will be several and each is
        // a normal step of a restart, and loud once when the sink has really given up.
        const bool retrying = this->recovery_.pending();
        cli_log(retrying ? LogLevel::DEBUG : LogLevel::WARN, "pipewire: '%s' is not back%s",
                this->name().c_str(),
                retrying ? " -- trying again shortly" : " -- discarding until the next stream");
        return;
    }
    if (this->stopping_.load()) {
        // stop() latches before it takes mutex_, so it can arrive while the reconnect above is
        // running. Hand the stream straight back rather than leave a live one for the destructor.
        this->recovery_.rescan_done(true);  // shutting down; there is nothing left to retry for
        this->close_stream_();
        this->stop_loop_();
        return;
    }
    this->recovery_.rescan_done(true);
    cli_log(LogLevel::INFO, "pipewire: '%s' is back after reconnecting to the graph",
            this->name().c_str());
}

void PipeWireSink::set_volume(uint8_t volume) {
    this->volume_.store(volume > 100 ? 100 : volume);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "pipewire: volume now %u", this->volume_.load());
}

void PipeWireSink::set_muted(bool muted) {
    this->muted_.store(muted);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "pipewire: %s", muted ? "muted" : "unmuted");
}

void PipeWireSink::stream_state_cb(void* userdata, enum pw_stream_state /*old*/,
                                   enum pw_stream_state state, const char* error) {
    auto* self = static_cast<PipeWireSink*>(userdata);
    self->stream_state_.store(static_cast<int>(state));
    if (state == PW_STREAM_STATE_ERROR) {
        cli_log(LogLevel::WARN, "pipewire: '%s' failed: %s", self->name().c_str(),
                (error != nullptr) ? error : "(no reason given)");
    }
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        // The graph put the stream in error, or unlinked it -- the node went away. Recorded rather
        // than acted on: write() is what notices, and it is the thread that may spend the attempt.
        self->stream_failed_.store(true);
        self->space_available_.notify_all();
    }
    if (self->loop_ != nullptr) {
        pw_thread_loop_signal(self->loop_, false);
    }
}

void PipeWireSink::stream_process_cb(void* userdata) {
    auto* self = static_cast<PipeWireSink*>(userdata);
    const int64_t entered_us = now_us();

    // stream_ is read from the realtime data thread under the header's ordering invariant, like
    // every other field here: pw_stream_disconnect() does not return while this callback is
    // running, and nothing clears the pointer until it has.
    pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream_);
    if (buffer == nullptr) {
        return;  // the graph has nothing for us this cycle
    }
    spa_data& data = buffer->buffer->datas[0];
    auto* dest = static_cast<uint8_t*>(data.data);
    const size_t stride = self->bytes_per_frame_;
    if (dest == nullptr || stride == 0) {
        pw_stream_queue_buffer(self->stream_, buffer);
        return;
    }

    size_t frames = data.maxsize / stride;
    if (buffer->requested != 0) {
        frames = std::min<size_t>(frames, buffer->requested);
    }
    const size_t bytes = frames * stride;
    // Short reads are zero-filled by the ring, which is silence for signed PCM -- so an underrun
    // is a gap rather than whatever the graph's buffer last held.
    const size_t real_bytes = self->ring_.read(dest, bytes);

    // Read before the buffer is queued: `delay` is how long it will be until the *next* sample this
    // stream hands over is presented, so this buffer's first frame lands then and its last one a
    // buffer later. `queued` and `buffered` are whatever is already ahead of it.
    pw_time time{};
    int64_t finish_us = 0;
    bool have_timing = false;
    if (real_bytes > 0 && self->on_frames_played &&
        pw_stream_get_time_n(self->stream_, &time, sizeof(time)) == 0 && time.rate.denom != 0) {
        const double rate_s = static_cast<double>(time.rate.num) /
                              static_cast<double>(time.rate.denom);
        double ahead_s = static_cast<double>(time.delay) * rate_s;
        if (self->rate_ != 0) {
            ahead_s += static_cast<double>(time.queued + time.buffered + (real_bytes / stride)) /
                       static_cast<double>(self->rate_);
        }
        finish_us = entered_us + static_cast<int64_t>(ahead_s * 1e6);
        have_timing = true;
    }

    data.chunk->offset = 0;
    data.chunk->stride = static_cast<int32_t>(stride);
    data.chunk->size = static_cast<uint32_t>(bytes);
    // In frames, not bytes: pw_time::queued is the sum of this field across queued buffers, and
    // the timing maths above reads it as frames.
    buffer->size = frames;
    pw_stream_queue_buffer(self->stream_, buffer);

    self->quantum_frames_.store(static_cast<uint32_t>(frames), std::memory_order_relaxed);

    // Space has just come free; wake whoever is blocked in write(). Notifying without the mutex is
    // what keeps this callback off the graph's realtime path -- the same pragmatic trade
    // PortAudioSink's callback makes. It does leave a window in which a writer that has just
    // evaluated its predicate, and not yet slept, misses this wakeup: harmless, because the next
    // cycle is only a quantum away and the writer's own deadline bounds the wait regardless.
    self->space_available_.notify_one();

    if (have_timing) {
        self->on_frames_played(static_cast<uint32_t>(real_bytes / stride), finish_us);
    }
}

bool PipeWireSink::start_loop_() {
    if (this->loop_ != nullptr) {
        return true;
    }
    this->loop_ = pw_thread_loop_new("sendspin-pw", nullptr);
    if (this->loop_ == nullptr) {
        cli_log(LogLevel::ERROR, "pipewire: cannot create a loop");
        return false;
    }
    if (pw_thread_loop_start(this->loop_) < 0) {
        cli_log(LogLevel::ERROR, "pipewire: cannot start the loop thread");
        pw_thread_loop_destroy(this->loop_);
        this->loop_ = nullptr;
        return false;
    }
    return true;
}

void PipeWireSink::stop_loop_() {
    if (this->loop_ == nullptr) {
        return;
    }
    pw_thread_loop_destroy(this->loop_);
    this->loop_ = nullptr;
}

bool PipeWireSink::open_stream_(uint32_t sample_rate, uint8_t channels,
                                uint8_t bits_per_sample, int timeout_ms) {
    spa_audio_format format = SPA_AUDIO_FORMAT_UNKNOWN;
    if (!spa_format_for(bits_per_sample, format)) {
        cli_log(LogLevel::ERROR, "pipewire: unsupported bit depth %u", bits_per_sample);
        return false;
    }
    if (channels == 0 || sample_rate == 0) {
        cli_log(LogLevel::ERROR, "pipewire: refusing stream with %u ch at %u Hz", channels,
                sample_rate);
        return false;
    }
    if (!this->start_loop_()) {
        return false;
    }

    // The format fields go in before the stream is connected, so the process callback provably
    // cannot be reading them. See the ordering invariant in the header. The generation moves with
    // them: a write() parked on the previous stream must not feed this one.
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
    this->ring_.reset(this->ring_capacity_());
    this->stream_failed_.store(false);
    this->stream_state_.store(PW_STREAM_STATE_UNCONNECTED);
    this->quantum_frames_.store(0, std::memory_order_relaxed);
    this->quantum_logged_.store(false, std::memory_order_relaxed);

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                          PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, PIPEWIRE_APP_NAME,
                          PW_KEY_NODE_NAME, PIPEWIRE_APP_NAME, nullptr);
    if (props == nullptr) {
        cli_log(LogLevel::ERROR, "pipewire: cannot describe the stream to the graph");
        return false;
    }
    if (!this->device_.empty()) {
        // What -o pipewire:<node> resolves through. The daemon does the matching, which is why a
        // node that appears after startup needs no rescan here.
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, this->device_.c_str());
    }
    // Ask the graph for a quantum the ring can actually hold. Without this the stream states no
    // latency at all and takes whatever the graph happens to be running -- which on a host with
    // default.clock.force-quantum set can be larger than the whole ring, and then every process()
    // short-reads and zero-fills the remainder for the life of the stream. A third of the ring, so
    // RING_QUANTUM_MULTIPLE quanta fit by construction. A request rather than a guarantee: a
    // forced quantum overrides it, which is the case poll()'s warning is left to catch.
    const std::string latency =
        std::to_string(pipewire_ring_frames(sample_rate, this->buffer_ms_) /
                       RING_QUANTUM_MULTIPLE) +
        "/" + std::to_string(sample_rate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latency.c_str());

    uint8_t pod_storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    spa_audio_info_raw info{};
    info.format = format;
    info.rate = sample_rate;
    info.channels = channels;
    if (channels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (channels == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    // Function-local so the two callbacks stay private to the class, and static because
    // pw_stream_new_simple() keeps the pointer for the stream's whole life.
    static const pw_stream_events events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .destroy = nullptr,
        .state_changed = &PipeWireSink::stream_state_cb,
        .control_info = nullptr,
        .io_changed = nullptr,
        .param_changed = nullptr,
        .add_buffer = nullptr,
        .remove_buffer = nullptr,
        .process = &PipeWireSink::stream_process_cb,
        .drained = nullptr,
        .command = nullptr,
        .trigger_done = nullptr,
    };

    {
        const LoopLock lock(this->loop_);
        this->stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(this->loop_),
                                             PIPEWIRE_APP_NAME, props, &events, this);
        if (this->stream_ == nullptr) {
            // pw_stream_new_simple() consumes props whether or not it succeeds, so there is
            // nothing to free here.
            cli_log(LogLevel::ERROR, "pipewire: cannot create a stream");
            return false;
        }
        const auto flags = static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
        const int err =
            pw_stream_connect(this->stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1);
        if (err < 0) {
            cli_log(LogLevel::ERROR, "pipewire: cannot play on '%s': %s", this->name().c_str(),
                    spa_strerror(err));
            pw_stream_destroy(this->stream_);
            this->stream_ = nullptr;
            return false;
        }

        // Waits for the graph to accept the stream, so a node that will not take this format fails
        // here rather than by playing nothing. PAUSED counts: the graph has linked the stream and
        // will start it, and a paused graph is not this player's business.
        // A deadline rather than a per-wait timeout, so a state change that is not the one being
        // waited for cannot restart the clock. timed_wait_full() is what gives it millisecond
        // resolution; pw_thread_loop_timed_wait() only takes whole seconds.
        timespec deadline{};
        pw_thread_loop_get_time(this->loop_, &deadline,
                                static_cast<int64_t>(timeout_ms) * 1000 * 1000);
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

    const auto state = static_cast<pw_stream_state>(this->stream_state_.load());
    if (state != PW_STREAM_STATE_STREAMING && state != PW_STREAM_STATE_PAUSED) {
        cli_log(LogLevel::ERROR, "pipewire: '%s' would not start at %u Hz, %u ch, %u-bit",
                this->name().c_str(), sample_rate, channels, bits_per_sample);
        this->close_stream_();
        return false;
    }

    this->failed_.store(false);
    cli_log(LogLevel::INFO,
            "pipewire: '%s' open at %u Hz, %u ch, %u-bit (%zu bytes/frame, %zu-byte ring)",
            this->name().c_str(), sample_rate, channels, bits_per_sample, this->bytes_per_frame_,
            this->ring_.free_space() + 1);
    return true;
}

void PipeWireSink::close_stream_() {
    if (this->stream_ != nullptr) {
        {
            const LoopLock lock(this->loop_);
            // Disconnect before destroy, and both under the loop lock: neither returns while the
            // data thread is still inside process(), which is what makes the format fields below
            // safe to touch.
            pw_stream_disconnect(this->stream_);
            pw_stream_destroy(this->stream_);
        }
        this->stream_ = nullptr;
    }

    ++this->stream_generation_;
    this->rate_ = 0;
    this->channels_ = 0;
    this->bits_ = 0;
    this->bytes_per_frame_ = 0;
    this->ramp_step_ = 0;
    this->ring_.reset(0);
    this->stream_failed_.store(false);
    this->stream_state_.store(PW_STREAM_STATE_UNCONNECTED);
    // A write() parked on the old stream would otherwise wait out its whole timeout; its predicate
    // reads stream_, so waking it here lets it return promptly instead.
    this->space_available_.notify_all();
    // stopping_ is deliberately untouched: only stop() and the destructor latch it, and clearing
    // it here would let a mid-stream configure() un-latch a shutdown in progress.
}

bool PipeWireSink::reopen_in_place_() {
    if (this->stopping_.load() || this->last_format_.sample_rate == 0) {
        // Nothing worth recovering, or nothing to recover to. Read before the attempt is spent, so
        // neither costs the outage anything.
        return false;
    }
    if (!this->recovery_.reopen_due()) {
        return false;
    }

    const StreamFormat format = this->last_format_;
    this->close_stream_();
    // The loop is deliberately kept: this is the cheap attempt, and it is the one that recovers a
    // node that went away while the daemon stayed up. A daemon that restarted needs the loop
    // rebuilt, which is poll()'s more expensive job.
    if (!this->open_stream_(format.sample_rate, format.channels, format.bit_depth,
                            PIPEWIRE_RECOVERY_TIMEOUT_MS)) {
        this->recovery_.reopen_done(false);  // open_stream_() has already said why, once
        return false;
    }
    this->recovery_.reopen_done(true);

    if (this->stopping_.load()) {
        // stop() latches before it takes mutex_, so it can arrive while the open above is running.
        this->close_stream_();
        return false;
    }
    cli_log(LogLevel::INFO, "pipewire: '%s' recovered without waiting for the next stream",
            this->name().c_str());
    return true;
}

bool PipeWireSink::stream_alive_() const {
    return this->stream_ != nullptr && this->bytes_per_frame_ != 0 && !this->stream_failed_.load();
}

size_t PipeWireSink::ring_capacity_() const {
    const auto frames_by_time =
        static_cast<size_t>((static_cast<int64_t>(this->rate_) * this->buffer_ms_) / 1000);
    const size_t frames = pipewire_ring_frames(this->rate_, this->buffer_ms_);
    if (frames > frames_by_time) {
        // --buffer-ms is a request, not a promise. Naming the floor that won is the difference
        // between "your figure was ignored" and knowing what to ask for instead.
        cli_log(LogLevel::DEBUG,
                "pipewire: --buffer-ms %u is %zu frames at %u Hz, below the minimum-ring floor of "
                "%zu frames -- using the floor",
                this->buffer_ms_, frames_by_time, this->rate_, frames);
    }
    // The spare byte the ring keeps to tell full from empty, so `frames` really do fit.
    return (frames * this->bytes_per_frame_) + 1;
}

const uint8_t* PipeWireSink::stage_(const uint8_t* data, size_t frames, uint64_t start,
                                    uint64_t target) {
    if (start == target && target == Q32_ONE) {
        return data;  // unity, and staying there: the ring can take the caller's bytes as they are
    }
    const size_t bytes = frames * this->bytes_per_frame_;
    this->scratch_.assign(data, data + bytes);
    apply_volume_ramp(this->scratch_.data(), bytes, this->bits_ / 8U, this->channels_, start,
                      target, this->ramp_step_);
    return this->scratch_.data();
}

void PipeWireSink::update_target_multiplier_() {
    this->target_multiplier_.store(q32_gain_for(this->volume_.load(), this->muted_.load()),
                                   std::memory_order_relaxed);
}

}  // namespace sendspin_cli
