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

#include "coreaudio_sink.h"

#include "coreaudio_device.h"
#include "log.h"
#include "pcm_volume.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// Ring floor in device buffers: the callback takes a whole buffer per wakeup.
constexpr double RING_LATENCY_MULTIPLE = 3.0;

/// Absolute floor on the ring, for a device that reports no latency at all.
constexpr size_t MIN_RING_FRAMES = 1024;

/// CoreAudio's spelling of PROBE_BIT_DEPTHS, in the same order.
constexpr std::array<const char*, PROBE_BIT_DEPTHS.size()> PROBE_FORMAT_NAMES{"SInt8", "SInt16",
                                                                              "SInt24", "SInt32"};

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Asks the unit whether it will take this exact format on its input scope; the AU converts on
/// top of what the device itself does, so this is what a stream would really get.
bool unit_accepts(AudioUnit unit, uint32_t sample_rate, uint8_t channels, uint8_t bits) {
    AudioStreamBasicDescription asbd = {};
    if (!ca_asbd_for(sample_rate, channels, bits, asbd)) {
        return false;
    }
    return AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                &asbd, sizeof(asbd)) == noErr;
}

/// Why a probe could not describe a device.
enum class ProbeStatus {
    Ok,
    NoDevice,  ///< the device has no output channels, or it went away mid-probe
    NoUnit,    ///< AUHAL would not instantiate, so nothing can be asked
};

struct ProbeResult {
    ProbeStatus status{ProbeStatus::Ok};
    /// Empty on anything but Ok.
    SinkCapabilities caps;
};

/// Asks one device what it will take, without starting it; axes are independent.
ProbeResult probe_capabilities(AudioDeviceID device) {
    ProbeResult result;
    const uint32_t max_channels = ca_channels(device, CaDirection::Output);
    if (max_channels == 0) {
        result.status = ProbeStatus::NoDevice;
        return result;
    }

    AudioUnit unit = ca_new_hal_unit();
    if (unit == nullptr) {
        result.status = ProbeStatus::NoUnit;
        return result;
    }
    if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global,
                             0, &device, sizeof(device)) != noErr) {
        AudioComponentInstanceDispose(unit);
        result.status = ProbeStatus::NoDevice;
        return result;
    }

    // One rate x depth pass at the channel count we would use.
    const auto probe_channels = static_cast<uint8_t>(std::min<uint32_t>(2, max_channels));
    bool accepted[PROBE_RATES.size()][PROBE_BIT_DEPTHS.size()] = {};
    for (size_t r = 0; r < PROBE_RATES.size(); ++r) {
        for (size_t d = 0; d < PROBE_BIT_DEPTHS.size(); ++d) {
            accepted[r][d] =
                unit_accepts(unit, PROBE_RATES[r], probe_channels, PROBE_BIT_DEPTHS[d]);
        }
    }

    for (size_t r = 0; r < PROBE_RATES.size(); ++r) {
        if (std::any_of(std::begin(accepted[r]), std::end(accepted[r]),
                        [](bool ok) { return ok; })) {
            result.caps.rates.push_back(PROBE_RATES[r]);
        }
    }
    for (size_t d = 0; d < PROBE_BIT_DEPTHS.size(); ++d) {
        for (size_t r = 0; r < PROBE_RATES.size(); ++r) {
            if (accepted[r][d]) {
                result.caps.bit_depths.push_back(PROBE_BIT_DEPTHS[d]);
                break;
            }
        }
    }

    // The channel axis reuses a depth the grid accepted, at the device's own rate.
    if (!result.caps.bit_depths.empty()) {
        const double device_rate = ca_nominal_sample_rate(device);
        const auto probe_rate =
            (device_rate > 0.0) ? static_cast<uint32_t>(device_rate) : result.caps.rates.front();
        for (const uint8_t count : PROBE_CHANNELS) {
            if (count > max_channels) {
                break;  // PROBE_CHANNELS ascends, so nothing after this fits either
            }
            if (unit_accepts(unit, probe_rate, count, result.caps.bit_depths.front())) {
                result.caps.channels.push_back(count);
            }
        }
    }

    AudioComponentInstanceDispose(unit);
    return result;
}

/// Prints what one device will take, indented under it in -l.
void print_device_capabilities(std::FILE* out, AudioDeviceID device) {
    const ProbeResult result = probe_capabilities(device);
    switch (result.status) {
        case ProbeStatus::NoDevice:
            std::fprintf(out, "      (cannot query: the device went away)\n");
            return;
        case ProbeStatus::NoUnit:
            std::fprintf(out, "      (cannot query: no AUHAL output unit on this host)\n");
            return;
        case ProbeStatus::Ok:
            break;
    }
    print_sink_capabilities(out, result.caps, PROBE_FORMAT_NAMES);
}

}  // namespace

CoreAudioSink::CoreAudioSink(std::string device, uint32_t buffer_ms)
    : device_(std::move(device)), buffer_ms_(buffer_ms) {}

CoreAudioSink::~CoreAudioSink() {
    // A sink destroyed without stop() must still stop its unit and drop its listeners.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->close_unit_();
}

std::string CoreAudioSink::name() const {
    return this->device_.empty() ? "coreaudio" : "coreaudio:" + this->device_;
}

bool CoreAudioSink::probe(const std::string& device, std::string& error) {
    AudioDeviceID resolved = kAudioObjectUnknown;
    return resolve_ca_device(device, CaDirection::Output, resolved, error);
}

SinkCapabilities CoreAudioSink::capabilities() const {
    // Describes the default device at startup; a later default move shows up as a reopen.
    AudioDeviceID device = kAudioObjectUnknown;
    std::string error;
    if (!resolve_ca_device(this->device_, CaDirection::Output, device, error)) {
        cli_log(LogLevel::DEBUG, "coreaudio: %s -- advertising everything sendspin-cli can emit",
                error.c_str());
        return SinkCapabilities::permissive();
    }

    const std::string label = ca_device_name(device);
    const ProbeResult result = probe_capabilities(device);
    if (result.status != ProbeStatus::Ok) {
        cli_log(LogLevel::DEBUG,
                "coreaudio: could not probe '%s' -- advertising everything sendspin-cli can emit",
                label.c_str());
        return SinkCapabilities::permissive();
    }
    cli_log(LogLevel::DEBUG, "coreaudio: capabilities probed from '%s'", label.c_str());
    return result.caps;
}

void CoreAudioSink::list_devices(std::FILE* out) {
    const std::vector<AudioDeviceID> outputs = ca_enumerate_devices(CaDirection::Output);
    if (outputs.empty()) {
        std::fprintf(out, "  (this host has no CoreAudio output devices)\n");
        return;
    }

    const AudioDeviceID fallback = ca_default_device(CaDirection::Output);
    std::fprintf(out, "  idx  name                                   out ch  default rate\n");
    for (size_t i = 0; i < outputs.size(); ++i) {
        const AudioDeviceID device = outputs[i];
        std::fprintf(out, "  %3zu  %-38s %2u ch  %6.0f Hz%s\n", i, ca_device_name(device).c_str(),
                     ca_channels(device, CaDirection::Output), ca_nominal_sample_rate(device),
                     (device == fallback) ? "  (system default)" : "");
        print_device_capabilities(out, device);
    }
}

bool CoreAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    if (this->stopping_.load()) {
        // A stream start racing shutdown; never clear stopping_ here.
        cli_log(LogLevel::DEBUG, "coreaudio: ignoring a stream start during shutdown");
        return false;
    }

    // Before anything can fail: recovery reopens at this format.
    this->last_format_ = {sample_rate, channels, bits_per_sample};
    // A new stream starts the producer from zero buffered frames, so an outage gap left over from
    // the last one is owed to nobody, on either side of the handoff. Before the device is
    // resolved, so the restart_unit_() reuse path and a failed open both drop it too.
    this->recovery_.forget_discarded_frames();
    this->gap_handoff_.forget();
    this->default_moved_.store(false);

    // Resolved per stream, so a bare -o coreaudio follows the host's default.
    AudioDeviceID device = kAudioObjectUnknown;
    std::string error;
    if (!resolve_ca_device(this->device_, CaDirection::Output, device, error)) {
        cli_log(LogLevel::ERROR, "coreaudio: %s", error.c_str());
        this->failed_.store(true);
        return false;
    }

    if (this->unit_alive_() && this->device_id_ == device && this->rate_ == sample_rate &&
        this->channels_ == channels && this->bits_ == bits_per_sample) {
        // Same device and format: restart from an empty ring instead of reopening.
        if (this->restart_unit_()) {
            this->recovery_.reset();
            cli_log(LogLevel::DEBUG, "coreaudio: reusing the open unit at %u Hz, %u ch, %u-bit",
                    sample_rate, channels, bits_per_sample);
            return true;
        }
        cli_log(LogLevel::WARN, "coreaudio: could not restart the unit -- reopening");
    }

    this->close_unit_();
    if (!this->open_unit_(device, sample_rate, channels, bits_per_sample)) {
        this->failed_.store(true);
        return false;
    }
    // Refill the recovery budget only once a unit is really running.
    this->recovery_.reset();
    return true;
}

size_t CoreAudioSink::write(const uint8_t* data, size_t length, uint32_t timeout_ms) {
    if (data == nullptr || length == 0) {
        return 0;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(this->mutex_);

    if ((!this->unit_alive_() || this->bytes_per_frame_ == 0) && !this->reopen_in_place_()) {
        // Discard rather than return 0 forever, which would spin the sync task.
        if (!this->failed_.exchange(true)) {
            cli_log(LogLevel::ERROR,
                    "coreaudio: '%s' is not playing -- discarding audio until a stream "
                    "reconfigures it",
                    this->name().c_str());
        }
        // Frame-aligned via last_format_ when a failed reopen has zeroed bytes_per_frame_.
        const size_t frame = (this->bytes_per_frame_ != 0)
                                 ? this->bytes_per_frame_
                                 : static_cast<size_t>(this->last_format_.channels) *
                                       (static_cast<size_t>(this->last_format_.bit_depth) / 8U);
        const size_t consumed = (frame == 0) ? length : length - (length % frame);
        if (frame != 0) {
            this->recovery_.discard_frames(static_cast<uint32_t>(consumed / frame));
        }
        return consumed;
    }

    const size_t bytes_per_frame = this->bytes_per_frame_;
    // Re-checked after every wait: waiting drops the mutex.
    const uint64_t generation = this->stream_generation_;
    const size_t usable = length - (length % bytes_per_frame);
    if (usable == 0) {
        return 0;
    }

    // The unit is alive, so an outage's gap can go to the callback: it has the timestamp to
    // retire the gap against, and this thread does not.
    this->gap_handoff_.add(this->recovery_.take_discarded_frames());

    size_t done = 0;
    while (done < usable) {
        if (this->stopping_.load()) {
            break;
        }

        const size_t remaining = usable - done;
        size_t room = this->ring_.free_space();
        if (room < remaining) {
            // Whole frames only: the callback counts frames by integer division.
            room -= room % bytes_per_frame;
        }
        if (room > 0) {
            const size_t written = this->ring_.write(data + done, std::min(remaining, room));
            if (written == 0) {
                break;  // the ring reported room and then took none; nothing to gain by spinning
            }
            done += written;
            continue;
        }

        if (!this->space_available_.wait_until(lock, deadline, [this, bytes_per_frame, generation] {
                return this->ring_.free_space() >= bytes_per_frame || this->stopping_.load() ||
                       this->unit_ == nullptr || this->stream_generation_ != generation;
            })) {
            break;  // out of time; the caller gets a short write and comes back
        }
        if (this->unit_ == nullptr || this->stream_generation_ != generation) {
            // The unit changed while unlocked; report only what landed.
            break;
        }
    }

    return done;
}

void CoreAudioSink::clear() {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    // A flush ends a parked write()'s stream.
    ++this->stream_generation_;

    // The player zeroes its buffered-frame count with a flush, so the gap is owed to nobody. Not
    // reset(): the recovery budget is configure()'s to refill.
    this->recovery_.forget_discarded_frames();
    this->gap_handoff_.forget();

    // Do not snap current_multiplier_: the callback keeps running through a flush.

    if (this->callback_running_()) {
        // The consumer owns read_pos_, so it drains on its next read. Liveness, not unit_alive_():
        // a lost device keeps being pulled, and drop() from this side would race that read.
        this->ring_.request_clear();
        return;
    }
    // No consumer running, so drop now rather than leave a clear pending for the next stream.
    this->ring_.drop();
}

void CoreAudioSink::stop() {
    // Before the mutex, so a blocked write() bails out.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    // Before the early return, so a write() after shutdown attempts nothing.
    this->last_format_ = {};
    if (this->unit_ == nullptr) {
        return;
    }
    this->close_unit_();
    cli_log(LogLevel::INFO, "coreaudio: '%s' closed", this->name().c_str());
}

void CoreAudioSink::poll(int64_t now_ms) {
    if (this->stopping_.load()) {
        return;
    }
    // A default move is an ordinary event, not a failure: it gets a clean reopen of its own so it
    // cannot spend the recovery budget a real outage needs.
    if (this->default_moved_.exchange(false)) {
        this->follow_default_();
    }

    // Unlocked fast path, and checked before rescan_due() so shutdown never burns an attempt.
    if (!this->recovery_.pending()) {
        return;
    }

    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->last_format_.sample_rate == 0) {
        return;  // nothing was ever configured, so there is nothing to reopen at
    }
    // Cleared only once a rescan is owed, so a replug that lands before the escalation still
    // counts; a stale flag costs one early attempt and nothing else.
    if (this->devices_changed_.exchange(false)) {
        this->recovery_.rescan_soon();
    }
    if (!this->recovery_.rescan_due(now_ms)) {
        return;
    }

    AudioDeviceID device = kAudioObjectUnknown;
    std::string error;
    if (!resolve_ca_device(this->device_, CaDirection::Output, device, error)) {
        cli_log(LogLevel::WARN,
                "coreaudio: '%s' is still gone -- discarding until it comes back "
                "or the next stream (%s)",
                this->name().c_str(), error.c_str());
        this->recovery_.rescan_done(false);
        return;
    }

    const StreamFormat format = this->last_format_;
    this->discard_ring_tail_();
    this->close_unit_();
    if (!this->open_unit_(device, format.sample_rate, format.channels, format.bit_depth)) {
        this->recovery_.rescan_done(false);  // open_unit_() has already said why, once
        return;
    }
    this->recovery_.rescan_done(true);

    if (this->stopping_.load()) {
        // stop() can land during the slow cycle above; release the device now.
        this->close_unit_();
        return;
    }
    cli_log(LogLevel::INFO, "coreaudio: '%s' is back, on '%s'", this->name().c_str(),
            ca_device_name(device).c_str());
}

void CoreAudioSink::set_volume(uint8_t volume) {
    this->volume_.store(volume > 100 ? 100 : volume);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "coreaudio: volume now %u (gain %.4f)", this->volume_.load(),
            static_cast<double>(this->target_multiplier_.load()) / static_cast<double>(Q32_ONE));
}

void CoreAudioSink::set_muted(bool muted) {
    this->muted_.store(muted);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "coreaudio: %s", muted ? "muted" : "unmuted");
}

void CoreAudioSink::follow_default_() {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (!this->device_.empty() || this->unit_ == nullptr || this->last_format_.sample_rate == 0) {
        return;
    }

    AudioDeviceID device = kAudioObjectUnknown;
    std::string error;
    if (!resolve_ca_device(this->device_, CaDirection::Output, device, error) ||
        device == this->device_id_) {
        return;  // no default to move to, or it did not actually move
    }

    const StreamFormat format = this->last_format_;
    const std::string from = ca_device_name(this->device_id_);
    this->discard_ring_tail_();
    this->close_unit_();
    if (!this->open_unit_(device, format.sample_rate, format.channels, format.bit_depth)) {
        // Spend the in-place attempt so poll()'s retry ladder takes over: write() cannot reopen a
        // unit that is already disposed.
        if (this->recovery_.reopen_due()) {
            this->recovery_.reopen_done(false);
        }
        this->failed_.store(true);
        return;
    }
    this->recovery_.reset();

    if (this->stopping_.load()) {
        this->close_unit_();
        return;
    }
    cli_log(LogLevel::INFO,
            "coreaudio: the system default output moved from '%s' to '%s' -- following it",
            from.c_str(), ca_device_name(device).c_str());
}

bool CoreAudioSink::open_unit_(AudioDeviceID device, uint32_t sample_rate, uint8_t channels,
                               uint8_t bits_per_sample) {
    AudioStreamBasicDescription asbd = {};
    if (!ca_asbd_for(sample_rate, channels, bits_per_sample, asbd)) {
        cli_log(LogLevel::ERROR, "coreaudio: refusing stream at %u Hz / %u ch / %u-bit",
                sample_rate, channels, bits_per_sample);
        return false;
    }

    const uint32_t max_channels = ca_channels(device, CaDirection::Output);
    const std::string label = ca_device_name(device);
    if (max_channels == 0) {
        cli_log(LogLevel::ERROR, "coreaudio: '%s' disappeared before it could be opened",
                label.c_str());
        return false;
    }
    if (channels > max_channels) {
        cli_log(LogLevel::ERROR, "coreaudio: '%s' has %u output channels, so it cannot play %u",
                label.c_str(), max_channels, channels);
        return false;
    }

    AudioUnit unit = ca_new_hal_unit();
    if (unit == nullptr) {
        cli_log(LogLevel::ERROR, "coreaudio: this host has no AUHAL output unit to play through");
        return false;
    }
    // Held from here on so every failure below goes out through close_unit_().
    this->unit_ = unit;

    OSStatus err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                        kAudioUnitScope_Global, 0, &device, sizeof(device));
    if (err != noErr) {
        cli_log(LogLevel::ERROR, "coreaudio: '%s' would not take the output unit: %s",
                label.c_str(), ca_status_text(err).c_str());
        this->close_unit_();
        return false;
    }

    err = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                               &asbd, sizeof(asbd));
    if (err != noErr) {
        cli_log(LogLevel::ERROR, "coreaudio: '%s' would not take %u Hz / %u ch / %u-bit: %s",
                label.c_str(), sample_rate, channels, bits_per_sample, ca_status_text(err).c_str());
        this->close_unit_();
        return false;
    }

    AURenderCallbackStruct callback = {};
    callback.inputProc = &CoreAudioSink::render_callback;
    callback.inputProcRefCon = this;
    err = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                               &callback, sizeof(callback));
    if (err != noErr) {
        cli_log(LogLevel::ERROR, "coreaudio: '%s' would not take a render callback: %s",
                label.c_str(), ca_status_text(err).c_str());
        this->close_unit_();
        return false;
    }

    err = AudioUnitInitialize(unit);
    if (err != noErr) {
        cli_log(LogLevel::ERROR,
                "coreaudio: '%s' would not initialise at %u Hz / %u ch / %u-bit: "
                "%s",
                label.c_str(), sample_rate, channels, bits_per_sample, ca_status_text(err).c_str());
        this->close_unit_();
        return false;
    }

    // Format fields and generation change before the unit starts, while the callback cannot run.
    ++this->stream_generation_;
    this->device_id_ = device;
    this->rate_ = sample_rate;
    this->channels_ = channels;
    this->bits_ = bits_per_sample;
    this->bytes_per_frame_ =
        static_cast<size_t>(channels) * (static_cast<size_t>(bits_per_sample) / 8U);
    this->stream_rate_ = static_cast<double>(sample_rate);
    this->ramp_step_ = volume_ramp_step(sample_rate);
    this->host_ticks_per_us_ = ca_host_ticks_per_us();
    // Open at the target gain, never ramping up to a restored volume.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    // What the unit really took on its input scope.
    AudioStreamBasicDescription actual = {};
    UInt32 actual_size = sizeof(actual);
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                             &actual, &actual_size) == noErr &&
        actual.mSampleRate > 0.0) {
        this->stream_rate_ = actual.mSampleRate;
    }

    // Presentation latency goes on top of the render timestamp; the safety offset must not, since
    // the HAL has already scheduled the buffer that far ahead.
    const double device_rate = ca_nominal_sample_rate(device);
    double latency_s = 0.0;
    if (device_rate > 0.0) {
        const uint32_t frames =
            ca_u32_property(device, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput) +
            ca_first_stream_latency(device, CaDirection::Output);
        latency_s = static_cast<double>(frames) / device_rate;
    }
    Float64 unit_latency_s = 0.0;
    UInt32 unit_latency_size = sizeof(unit_latency_s);
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0,
                             &unit_latency_s, &unit_latency_size) == noErr) {
        latency_s += unit_latency_s;
    }
    this->output_latency_us_ = static_cast<int64_t>(std::llround(latency_s * 1e6));

    const size_t capacity = this->ring_capacity_(latency_s);
    this->ring_.reset(capacity);

    // Cleared before the listeners go on, so a death between the two is not lost.
    this->device_lost_.store(false);
    this->devices_changed_.store(false);
    this->add_listeners_(device);

    err = AudioOutputUnitStart(unit);
    if (err != noErr) {
        cli_log(LogLevel::ERROR, "coreaudio: '%s' would not start: %s", label.c_str(),
                ca_status_text(err).c_str());
        this->close_unit_();
        return false;
    }
    this->running_ = true;

    this->failed_.store(false);
    // Spelled out because a zero step snaps, which is audible but hard to tell from a fast ramp.
    cli_log(LogLevel::DEBUG, "coreaudio: volume ramp %llu Q32/frame, %u ms for a full-scale change",
            static_cast<unsigned long long>(this->ramp_step_), VOLUME_RAMP_MS);
    cli_log(LogLevel::INFO,
            "coreaudio: '%s' (%s) open at %u Hz, %u ch, %u-bit (%zu bytes/frame, "
            "%zu-byte ring, %.1f ms output latency)",
            label.c_str(), this->name().c_str(), sample_rate, channels, bits_per_sample,
            this->bytes_per_frame_, capacity, latency_s * 1000.0);
    return true;
}

void CoreAudioSink::close_unit_() {
    this->remove_listeners_();

    if (this->unit_ != nullptr) {
        if (this->running_) {
            AudioOutputUnitStop(this->unit_);
        }
        // Uninitialize waits the render callback out, so nothing below races it.
        AudioUnitUninitialize(this->unit_);
        AudioComponentInstanceDispose(this->unit_);
        this->unit_ = nullptr;
    }

    this->running_ = false;
    ++this->stream_generation_;
    this->device_id_ = kAudioObjectUnknown;
    this->rate_ = 0;
    this->channels_ = 0;
    this->bits_ = 0;
    this->bytes_per_frame_ = 0;
    this->stream_rate_ = 0.0;
    this->ramp_step_ = 0;
    this->output_latency_us_ = 0;
    this->ring_.reset(0);
    // Wake a write() parked on the old unit.
    this->space_available_.notify_all();
    // Never clear stopping_ here: a mid-stream configure() would un-latch a shutdown.
}

bool CoreAudioSink::restart_unit_() {
    if (this->running_) {
        const OSStatus err = AudioOutputUnitStop(this->unit_);
        if (err != noErr) {
            cli_log(LogLevel::DEBUG, "coreaudio: cannot stop the unit: %s",
                    ca_status_text(err).c_str());
            return false;
        }
        this->running_ = false;
    }

    // A new stream: a write() parked on the old one holds dropped audio.
    ++this->stream_generation_;

    // The callback has stopped, so dropping the ring from this side is safe here.
    this->ring_.drop();
    // Snap the gain too, so the next stream starts at its target.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    const OSStatus err = AudioOutputUnitStart(this->unit_);
    if (err != noErr) {
        cli_log(LogLevel::DEBUG, "coreaudio: cannot restart the unit: %s",
                ca_status_text(err).c_str());
        return false;
    }
    this->running_ = true;

    this->failed_.store(false);
    return true;
}

bool CoreAudioSink::reopen_in_place_() {
    if (this->unit_ == nullptr || this->stopping_.load() || this->last_format_.sample_rate == 0) {
        // Only a unit that ran and died: a refused format has a null unit_ and needs no retry.
        return false;
    }
    if (!this->recovery_.reopen_due()) {
        return false;
    }

    // Resolve before closing, so a failure leaves write() its frame size.
    AudioDeviceID device = kAudioObjectUnknown;
    std::string error;
    if (!resolve_ca_device(this->device_, CaDirection::Output, device, error)) {
        cli_log(LogLevel::WARN, "coreaudio: cannot reopen '%s': %s", this->name().c_str(),
                error.c_str());
        this->recovery_.reopen_done(false);
        return false;
    }

    const StreamFormat format = this->last_format_;
    this->discard_ring_tail_();
    this->close_unit_();
    if (!this->open_unit_(device, format.sample_rate, format.channels, format.bit_depth)) {
        this->recovery_.reopen_done(false);  // open_unit_() has already said why, once
        return false;
    }
    this->recovery_.reopen_done(true);

    if (this->stopping_.load()) {
        // stop() can land during the open above; release the device now.
        this->close_unit_();
        return false;
    }

    cli_log(LogLevel::INFO,
            "coreaudio: '%s' recovered on '%s' without waiting for the next "
            "stream",
            this->name().c_str(), ca_device_name(device).c_str());
    return true;
}

void CoreAudioSink::discard_ring_tail_() {
    // Stopped first: a callback draining between the count and close_unit_()'s own stop would
    // report those frames as played on top of the gap they are counted into here.
    if (this->callback_running_()) {
        AudioOutputUnitStop(this->unit_);
        this->running_ = false;
    }
    if (this->bytes_per_frame_ == 0) {
        return;  // no unit, so close_unit_() has already emptied the ring
    }
    this->recovery_.discard_frames(
        static_cast<uint32_t>(this->ring_.available() / this->bytes_per_frame_));
}

bool CoreAudioSink::callback_running_() const {
    return this->unit_ != nullptr && this->running_;
}

bool CoreAudioSink::unit_alive_() const {
    return this->callback_running_() && !this->device_lost_.load();
}

size_t CoreAudioSink::ring_capacity_(double device_latency_s) const {
    const auto frames_by_time =
        static_cast<size_t>((static_cast<int64_t>(this->rate_) * this->buffer_ms_) / 1000);
    const auto frames_by_latency =
        static_cast<size_t>(device_latency_s * this->stream_rate_ * RING_LATENCY_MULTIPLE);
    const size_t frames = std::max({frames_by_time, frames_by_latency, MIN_RING_FRAMES});
    // Log which floor overrode --buffer-ms.
    if (frames > frames_by_time) {
        cli_log(LogLevel::DEBUG,
                "coreaudio: --buffer-ms %u is %zu frames at %u Hz, below the %s floor of "
                "%zu frames -- using the floor",
                this->buffer_ms_, frames_by_time, this->rate_,
                (frames_by_latency >= MIN_RING_FRAMES) ? "device-latency" : "minimum-ring", frames);
    }
    // The spare byte the ring keeps to tell full from empty, so `frames` really do fit.
    return (frames * this->bytes_per_frame_) + 1;
}

void CoreAudioSink::update_target_multiplier_() {
    // Only the target moves; the callback advances current_multiplier_.
    this->target_multiplier_.store(q32_gain_for(this->volume_.load(), this->muted_.load()),
                                   std::memory_order_relaxed);
}

void CoreAudioSink::add_listeners_(AudioDeviceID device) {
    this->remove_listeners_();

    const AudioObjectPropertyAddress alive = ca_address_of(kAudioDevicePropertyDeviceIsAlive);
    if (AudioObjectAddPropertyListener(device, &alive, &CoreAudioSink::property_listener, this) ==
        noErr) {
        this->listening_alive_ = true;
        this->listening_device_ = device;
    } else {
        // Nothing else sets device_lost_, so without this a death is never noticed and recovery
        // never runs. Still worth playing through; the user just has to restart the stream.
        cli_log(LogLevel::WARN,
                "coreaudio: '%s' will not report its own death -- playback will not recover by "
                "itself if it goes away",
                ca_device_name(device).c_str());
    }

    if (this->device_.empty()) {
        const AudioObjectPropertyAddress fallback =
            ca_address_of(kAudioHardwarePropertyDefaultOutputDevice);
        if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &fallback,
                                           &CoreAudioSink::property_listener, this) == noErr) {
            this->listening_default_ = true;
        }
    }

    // A device that died tells us so itself; a device that comes back cannot, so the host's
    // device list is what turns a replug into a reopen instead of a wait on the backoff.
    const AudioObjectPropertyAddress devices = ca_address_of(kAudioHardwarePropertyDevices);
    if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &devices,
                                       &CoreAudioSink::property_listener, this) == noErr) {
        this->listening_devices_ = true;
    }
}

void CoreAudioSink::remove_listeners_() {
    if (this->listening_alive_) {
        const AudioObjectPropertyAddress alive = ca_address_of(kAudioDevicePropertyDeviceIsAlive);
        AudioObjectRemovePropertyListener(this->listening_device_, &alive,
                                          &CoreAudioSink::property_listener, this);
        this->listening_alive_ = false;
        this->listening_device_ = kAudioObjectUnknown;
    }
    if (this->listening_default_) {
        const AudioObjectPropertyAddress fallback =
            ca_address_of(kAudioHardwarePropertyDefaultOutputDevice);
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &fallback,
                                          &CoreAudioSink::property_listener, this);
        this->listening_default_ = false;
    }
    if (this->listening_devices_) {
        const AudioObjectPropertyAddress devices = ca_address_of(kAudioHardwarePropertyDevices);
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &devices,
                                          &CoreAudioSink::property_listener, this);
        this->listening_devices_ = false;
    }
}

OSStatus CoreAudioSink::property_listener(AudioObjectID /*object*/, UInt32 count,
                                          const AudioObjectPropertyAddress* addresses,
                                          void* user_data) {
    auto* self = static_cast<CoreAudioSink*>(user_data);
    for (UInt32 i = 0; i < count; ++i) {
        switch (addresses[i].mSelector) {
            case kAudioDevicePropertyDeviceIsAlive:
                // Only flagged: this runs on a HAL thread that must not block on mutex_.
                self->device_lost_.store(true);
                self->space_available_.notify_all();
                break;
            case kAudioHardwarePropertyDefaultOutputDevice:
                self->default_moved_.store(true);
                break;
            case kAudioHardwarePropertyDevices:
                self->devices_changed_.store(true);
                break;
            default:
                break;
        }
    }
    return noErr;
}

OSStatus CoreAudioSink::render_callback(void* user_data, AudioUnitRenderActionFlags* /*flags*/,
                                        const AudioTimeStamp* timestamp, UInt32 /*bus*/,
                                        UInt32 frames, AudioBufferList* data) {
    const int64_t entered_us = now_us();
    const uint64_t entered_host = mach_absolute_time();

    auto* self = static_cast<CoreAudioSink*>(user_data);
    if (data == nullptr || data->mNumberBuffers == 0 || self->bytes_per_frame_ == 0) {
        return noErr;
    }
    auto* out = static_cast<uint8_t*>(data->mBuffers[0].mData);
    if (out == nullptr) {
        return noErr;
    }

    // Interleaved, so one buffer carries every channel; the AU's own size is the hard limit.
    const size_t bytes_requested = std::min(static_cast<size_t>(frames) * self->bytes_per_frame_,
                                            static_cast<size_t>(data->mBuffers[0].mDataByteSize));

    const size_t bytes_read = self->ring_.read(out, bytes_requested);

    // The unity fast path also requires current == target, or it would skip a ramp.
    const uint64_t target = self->target_multiplier_.load(std::memory_order_relaxed);
    const uint64_t current = self->current_multiplier_;
    if (current != target || target != Q32_ONE) {
        // Advanced by every frame handed over, zero-filled tail included: silence is played too.
        self->current_multiplier_ =
            apply_volume_ramp(out, bytes_requested, self->bits_ / 8U, self->channels_, current,
                              target, self->ramp_step_);
    }

    // Notified without the mutex, keeping the callback lock-free; a missed wakeup is bounded.
    self->space_available_.notify_one();

    if (bytes_read > 0 && self->on_frames_played) {
        const auto frames_played = static_cast<uint32_t>(bytes_read / self->bytes_per_frame_);
        int64_t ahead_us = 0;
        if (self->host_ticks_per_us_ > 0.0 &&
            (timestamp->mFlags & kAudioTimeStampHostTimeValid) != 0) {
            // Signed on purpose: an already-past timestamp must subtract, not wrap.
            const auto ticks = static_cast<int64_t>(timestamp->mHostTime - entered_host);
            ahead_us = static_cast<int64_t>(
                std::llround(static_cast<double>(ticks) / self->host_ticks_per_us_));
        }
        const auto buffer_us = static_cast<int64_t>(
            std::llround((static_cast<double>(frames_played) / self->stream_rate_) * 1e6));
        const int64_t finish_us = entered_us + ahead_us + self->output_latency_us_ + buffer_us;
        // The gap rides on this report rather than its own: the player sums the frames of reports
        // it has not read yet but keeps only the last timestamp.
        self->on_frames_played(frames_with_gap(self->gap_handoff_.take(), frames_played),
                               finish_us);
    }

    return noErr;
}

}  // namespace sendspin_cli
