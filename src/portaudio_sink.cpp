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

#include "portaudio_sink.h"

#include "log.h"
#include "pcm_volume.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// Floor on the ring, as a multiple of PortAudio's own buffer. The callback asks for a whole
/// device buffer at a time, so a ring no bigger than that starves on every wakeup however
/// promptly write() refills it -- and PortAudio's high-latency buffer can exceed 100 ms.
constexpr double RING_LATENCY_MULTIPLE = 3.0;

/// Absolute floor on the ring, for a device that reports no latency at all.
constexpr size_t MIN_RING_FRAMES = 1024;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// How each of PROBE_BIT_DEPTHS is spelled on this backend, in the same order.
///
/// 24-bit is paInt24, three packed bytes, because the player hands us tightly packed samples.
/// 8-bit is here although upstream's reference refuses it: AlsaAudioSink accepts S8, and a
/// stream one backend plays and the other does not would be a difference with no cause.
constexpr std::array<PaSampleFormat, PROBE_BIT_DEPTHS.size()> PROBE_FORMATS{paInt8, paInt16,
                                                                            paInt24, paInt32};
constexpr std::array<const char*, PROBE_BIT_DEPTHS.size()> PROBE_FORMAT_NAMES{"paInt8", "paInt16",
                                                                              "paInt24", "paInt32"};

/// Maps the stream's bit depth onto the interleaved little-endian PCM format PortAudio wants.
bool pa_format_for(uint8_t bits_per_sample, PaSampleFormat& format) {
    for (size_t i = 0; i < PROBE_BIT_DEPTHS.size(); ++i) {
        if (PROBE_BIT_DEPTHS[i] == bits_per_sample) {
            format = PROBE_FORMATS[i];
            return true;
        }
    }
    return false;
}

/// True if `value` is a non-empty run of decimal digits, so `-o portaudio:2abc` and
/// `-o portaudio:-1` are read as names that will not match rather than as sloppy indices.
bool is_device_index(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    return value.find_first_not_of("0123456789") == std::string::npos;
}

bool iequals(const std::string& value, const char* other) {
    if (other == nullptr) {
        return false;
    }
    const size_t len = std::strlen(other);
    if (value.size() != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        // Cast through unsigned char: tolower's argument must be representable as one, and a
        // device name with a high-bit byte in it would otherwise be undefined behaviour.
        const int a = std::tolower(static_cast<unsigned char>(value[i]));
        const int b = std::tolower(static_cast<unsigned char>(other[i]));
        if (a != b) {
            return false;
        }
    }
    return true;
}

/// True if this device can be played out of at all.
bool is_output_device(const PaDeviceInfo* info) {
    return info != nullptr && info->maxOutputChannels > 0;
}

/// Resolves what followed `-o portaudio:` to a device index. PortAudio must be initialized.
///
/// Empty is this host's default output. All digits is an index as -l prints it. Anything else
/// is a device name, matched in full and case-insensitively -- a name that matches more than
/// one device is an error naming the candidates rather than a silent pick, because two host
/// APIs can offer the same card under the same name and guessing would make one -o mean
/// different things on different hosts.
/// @param error Set to a human-readable reason when the return value is false.
bool resolve_pa_device(const std::string& device, PaDeviceIndex& out, std::string& error) {
    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        error = std::string("cannot enumerate PortAudio devices: ") + Pa_GetErrorText(count);
        return false;
    }
    // Checked before the spec is even looked at, so a device-less host gets one clear answer
    // rather than three differently-worded ways of saying the same thing.
    if (count == 0) {
        error = "PortAudio found no audio devices on this host at all";
        return false;
    }

    if (device.empty()) {
        const PaDeviceIndex fallback = Pa_GetDefaultOutputDevice();
        if (fallback == paNoDevice) {
            error = "this host has no PortAudio output device at all -- run with -l to see what "
                    "it does have";
            return false;
        }
        out = fallback;
        return true;
    }

    if (is_device_index(device)) {
        // strtoull rather than stoul: no exceptions to catch, and a number too large for the
        // type saturates instead of wrapping round into a plausible-looking index.
        const unsigned long long value = std::strtoull(device.c_str(), nullptr, 10);
        if (value >= static_cast<unsigned long long>(count)) {
            // The range, not the count: PortAudio numbers input and output devices in one
            // sequence, so "this host has N devices" would disagree with the shorter list -l
            // prints -- it shows only the ones with output channels.
            error = "-o portaudio:" + device + ": no device at that index -- indices run 0-" +
                    std::to_string(count - 1) + " here, and -l lists the ones -o can reach";
            return false;
        }
        const auto index = static_cast<PaDeviceIndex>(value);
        if (!is_output_device(Pa_GetDeviceInfo(index))) {
            error = "-o portaudio:" + device +
                    ": that device has no output channels -- run with -l, which lists only the "
                    "ones -o can reach";
            return false;
        }
        out = index;
        return true;
    }

    std::vector<PaDeviceIndex> matches;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (is_output_device(info) && iequals(device, info->name)) {
            matches.push_back(i);
        }
    }
    if (matches.empty()) {
        error = "-o portaudio:" + device +
                ": no output device by that name -- run with -l to list them";
        return false;
    }
    if (matches.size() > 1) {
        std::string indices;
        for (const PaDeviceIndex index : matches) {
            if (!indices.empty()) {
                indices += ", ";
            }
            indices += std::to_string(index);
        }
        error = "-o portaudio:" + device + ": " + std::to_string(matches.size()) +
                " output devices share that name (indices " + indices +
                ") -- name the one you mean by index instead";
        return false;
    }

    out = matches.front();
    return true;
}

/// Asks whether `device` will take this exact (rate, depth, channels) triple.
///
/// Not as cheap as it looks on every host: CoreAudio answers from its own device list, but
/// PortAudio's ALSA host API opens and closes the PCM per call. That is what keeps the probe
/// below to one pass over the grid rather than one pass per axis.
PaError pa_accepts(PaDeviceIndex device, const PaDeviceInfo* info, uint32_t rate,
                   PaSampleFormat format, int channels) {
    PaStreamParameters params = {};
    params.device = device;
    params.channelCount = channels;
    params.sampleFormat = format;
    // The same latency open_stream_() asks for, so the probe describes the stream this sink
    // would really open rather than one it never asks for.
    params.suggestedLatency = info->defaultHighOutputLatency;
    params.hostApiSpecificStreamInfo = nullptr;
    return Pa_IsFormatSupported(nullptr, &params, static_cast<double>(rate));
}

/// Why a probe could not describe a device, mirroring AlsaAudioSink's own probe statuses so
/// -l reads the same way whichever backend answered.
enum class ProbeStatus {
    Ok,
    Busy,      ///< paDeviceUnavailable: the device is there, something else holds it
    NoDevice,  ///< the index does not resolve, or has no output channels
};

struct ProbeResult {
    ProbeStatus status{ProbeStatus::Ok};
    /// Empty on anything but Ok. Callers needing an answer regardless substitute
    /// SinkCapabilities::permissive(); -l prints the status instead.
    SinkCapabilities caps;
};

/// Asks one device what it will take, without opening a stream on it.
///
/// The PortAudio half of what AlsaAudioSink's probe_capabilities() does, and reports the three
/// axes independently for the same reason -- Pa_IsFormatSupported() answers per triple, so
/// each list is "supported with at least one of the others", not a promise that every
/// combination works. A refusal is still configure()'s to report per stream.
ProbeResult probe_capabilities(PaDeviceIndex device) {
    ProbeResult result;
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (info == nullptr || info->maxOutputChannels <= 0) {
        result.status = ProbeStatus::NoDevice;
        return result;
    }

    // One pass over the rate x depth grid at the channel count this player would actually ask
    // for -- stereo, or mono on a device with a single output. Both axes are read off the one
    // grid rather than walked separately, which halves the calls on a host where each is a
    // device open.
    const int probe_channels = std::min(2, info->maxOutputChannels);
    bool accepted[PROBE_RATES.size()][PROBE_BIT_DEPTHS.size()] = {};
    for (size_t r = 0; r < PROBE_RATES.size(); ++r) {
        for (size_t d = 0; d < PROBE_BIT_DEPTHS.size(); ++d) {
            const PaError err =
                pa_accepts(device, info, PROBE_RATES[r], PROBE_FORMATS[d], probe_channels);
            if (err == paDeviceUnavailable) {
                // Held exclusively by something else. Every further call would say the same,
                // so stop and let the caller report "in use" rather than "supports nothing".
                result.status = ProbeStatus::Busy;
                result.caps = {};
                return result;
            }
            accepted[r][d] = (err == paFormatIsSupported);
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

    // The channel axis needs a format to ask with, so it reuses a depth the grid already
    // accepted; a device the grid found nothing for has no channel count worth reporting.
    if (result.caps.bit_depths.empty()) {
        return result;
    }
    PaSampleFormat probe_format = PROBE_FORMATS[0];
    for (size_t d = 0; d < PROBE_BIT_DEPTHS.size(); ++d) {
        if (PROBE_BIT_DEPTHS[d] == result.caps.bit_depths.front()) {
            probe_format = PROBE_FORMATS[d];
        }
    }
    const auto probe_rate = static_cast<uint32_t>(info->defaultSampleRate);
    for (const uint8_t count : PROBE_CHANNELS) {
        if (count > info->maxOutputChannels) {
            break;  // PROBE_CHANNELS ascends, so nothing after this fits either
        }
        if (pa_accepts(device, info, probe_rate, probe_format, count) == paFormatIsSupported) {
            result.caps.channels.push_back(count);
        }
    }
    return result;
}

/// Prints what one device will take, indented under it in -l -- the same three lines, and the
/// same failure notes, AlsaAudioSink prints per PCM.
void print_device_capabilities(std::FILE* out, PaDeviceIndex device) {
    const ProbeResult result = probe_capabilities(device);
    switch (result.status) {
        case ProbeStatus::Busy:
            std::fprintf(out, "      (in use -- capabilities unknown)\n");
            return;
        case ProbeStatus::NoDevice:
            std::fprintf(out, "      (cannot query: the device went away)\n");
            return;
        case ProbeStatus::Ok:
            break;
    }
    print_sink_capabilities(out, result.caps, PROBE_FORMAT_NAMES);
}

}  // namespace

// ============================================================================
// PortAudioGuard
// ============================================================================

PortAudioGuard::PortAudioGuard() : err_(Pa_Initialize()) {}

PortAudioGuard::~PortAudioGuard() {
    if (this->err_ == paNoError) {
        Pa_Terminate();
    }
}

bool PortAudioGuard::ok() const {
    return this->err_ == paNoError;
}

const char* PortAudioGuard::error() const {
    return Pa_GetErrorText(this->err_);
}

bool PortAudioGuard::reinitialize() {
    if (this->err_ == paNoError) {
        Pa_Terminate();
    }
    // Overwritten rather than left alone, so ok() keeps describing the live state -- and so the
    // destructor terminates exactly when it should if this half failed.
    this->err_ = Pa_Initialize();
    return this->err_ == paNoError;
}

// ============================================================================
// PortAudioSink
// ============================================================================

PortAudioSink::PortAudioSink(std::string device, uint32_t buffer_ms)
    : device_(std::move(device)), buffer_ms_(buffer_ms) {
    if (!this->pa_.ok()) {
        // Reported rather than thrown: make_audio_sink() has already run probe(), so getting
        // here means PortAudio came up once and then would not again. configure() will fail
        // and the sink degrades to discarding, which beats taking the daemon down.
        cli_log(LogLevel::ERROR, "portaudio: cannot initialise PortAudio: %s", this->pa_.error());
    }
}

PortAudioSink::~PortAudioSink() {
    // stop() is the documented shutdown path, but a sink destroyed without it must still hand
    // the device back -- and must close the stream before pa_ terminates PortAudio under it.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->close_stream_();
}

std::string PortAudioSink::name() const {
    return this->device_.empty() ? "portaudio" : "portaudio:" + this->device_;
}

bool PortAudioSink::probe(const std::string& device, std::string& error) {
    const PortAudioGuard pa;
    if (!pa.ok()) {
        error = std::string("cannot initialise PortAudio: ") + pa.error();
        return false;
    }

    PaDeviceIndex index = paNoDevice;
    return resolve_pa_device(device, index, error);
}

SinkCapabilities PortAudioSink::capabilities() const {
    // Answered once, for the hello handshake -- but configure() re-resolves the device at
    // every stream, so a bare `-o portaudio` follows the host's default output as the user
    // changes it. This therefore describes whichever device was default *when the player
    // started*, and a later switch to a narrower device is not reflected in what the server
    // was told. What catches that is the refusal path: PlayerListener names the device and
    // the format it would not take, rather than the player going quietly silent.
    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, device, error)) {
        // Advertising nothing would leave the player unable to play at all, where being
        // over-broad costs at worst a per-stream refusal.
        cli_log(LogLevel::DEBUG, "portaudio: %s -- advertising everything sendspin-cli can emit",
                error.c_str());
        return SinkCapabilities::permissive();
    }

    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    // The fallback is held in a named local rather than spelled inline: name() returns by
    // value, so `this->name().c_str()` inside the conditional would leave device_name
    // pointing at a string already destroyed by the time the logging below reads it.
    const std::string fallback_name = this->name();
    const char* device_name = (info != nullptr) ? info->name : fallback_name.c_str();
    ProbeResult result = probe_capabilities(device);
    if (result.status != ProbeStatus::Ok) {
        // Busy or gone. Advertising nothing would leave the player unable to play at all,
        // where being over-broad costs at worst a per-stream refusal.
        cli_log(LogLevel::DEBUG,
                "portaudio: could not probe '%s' -- advertising everything sendspin-cli "
                "can emit",
                device_name);
        return SinkCapabilities::permissive();
    }
    // A device that probes cleanly but accepts nothing is reported as it answered: main() is
    // the layer that names the sink and decides what to advertise instead, and having both
    // backends fall through to it is what keeps that decision in one place.
    cli_log(LogLevel::DEBUG, "portaudio: capabilities probed from '%s'", device_name);
    return result.caps;
}

void PortAudioSink::list_devices(std::FILE* out) {
    const PortAudioGuard pa;
    if (!pa.ok()) {
        std::fprintf(out, "  (cannot initialise PortAudio: %s)\n", pa.error());
        return;
    }

    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        std::fprintf(out, "  (cannot enumerate PortAudio devices: %s)\n", Pa_GetErrorText(count));
        return;
    }

    const PaDeviceIndex fallback = Pa_GetDefaultOutputDevice();
    int listed = 0;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!is_output_device(info)) {
            continue;  // input-only: -o cannot reach it, so listing it would mislead
        }
        // The header lives here rather than with the surrounding -l prose, so the column widths
        // sit next to the format string they have to agree with -- and inside the loop, so a
        // host with no output devices gets the note below instead of a header over nothing.
        if (listed == 0) {
            std::fprintf(out, "  idx  name                                   host API     "
                              "out ch  default rate\n");
        }
        const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
        std::fprintf(out, "  %3d  %-38s %-12s %2d ch  %6.0f Hz%s\n", static_cast<int>(i),
                     info->name, (host != nullptr) ? host->name : "(unknown host API)",
                     info->maxOutputChannels, info->defaultSampleRate,
                     (i == fallback) ? "  (system default)" : "");
        print_device_capabilities(out, i);
        ++listed;
    }

    if (listed == 0) {
        std::fprintf(out, "  (this host has no PortAudio output devices)\n");
    }
}

bool PortAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    if (this->stopping_.load()) {
        // A stream start racing shutdown. Opening a device here would give write() nothing it
        // could feed -- the latch makes it return 0 -- and would put a fresh CoreAudio stream
        // in the destructor's way. Note that this reads the latch without ever clearing it:
        // that is stop()'s alone to set and nothing's to reset.
        cli_log(LogLevel::DEBUG, "portaudio: ignoring a stream start during shutdown");
        return false;
    }

    // Remembered before anything can fail, because this is the format the player is about to
    // send audio in whether or not a device takes it -- so it is also the only format recovery
    // may reopen at from here on. See last_format_.
    this->last_format_ = {sample_rate, channels, bits_per_sample};

    // Resolved per stream, not once at construction: a bare -o portaudio then follows the
    // host's default output as the user changes it, and a device that was absent at startup
    // is picked up whenever the next stream starts.
    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, device, error)) {
        cli_log(LogLevel::ERROR, "portaudio: %s", error.c_str());
        this->failed_.store(true);
        return false;
    }

    if (this->stream_ != nullptr && this->device_index_ == device && this->rate_ == sample_rate &&
        this->channels_ == channels && this->bits_ == bits_per_sample) {
        // Same device and format as the stream that just ended. Restarting from an empty ring
        // is far cheaper than a close/open round trip, and it spares a CoreAudio device the
        // reopen -- and the gap that comes with it -- at every track boundary.
        if (this->restart_stream_()) {
            this->recovery_.reset();
            cli_log(LogLevel::DEBUG, "portaudio: reusing the open stream at %u Hz, %u ch, %u-bit",
                    sample_rate, channels, bits_per_sample);
            return true;
        }
        cli_log(LogLevel::WARN, "portaudio: could not restart the stream -- reopening");
    }

    this->close_stream_();
    if (!this->open_stream_(device, sample_rate, channels, bits_per_sample)) {
        this->failed_.store(true);
        return false;
    }
    // Both recovery attempts go back in hand, and only here: a stream that is really running is
    // what a budget spent on the last one was waiting for. A configure() that failed deliberately
    // does not refill, or a device refusing every stream would buy a fresh rescan per track.
    this->recovery_.reset();
    return true;
}

size_t PortAudioSink::write(const uint8_t* data, size_t length, uint32_t timeout_ms) {
    if (data == nullptr || length == 0) {
        return 0;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(this->mutex_);

    if ((!this->stream_alive_() || this->bytes_per_frame_ == 0) && !this->reopen_in_place_()) {
        // No stream to feed, and none to be had: swallow the audio rather than return 0 forever,
        // which would spin the sync task on a buffer it can never hand off.
        if (!this->failed_.exchange(true)) {
            cli_log(LogLevel::ERROR,
                    "portaudio: '%s' is not playing -- discarding audio until a stream "
                    "reconfigures it",
                    this->name().c_str());
        }
        // Frame-aligned per the write() contract wherever the frame size is still known -- from
        // the remembered format when a failed reopen has just closed the stream out from under
        // bytes_per_frame_, which is the same figure it held. Once neither is known the sink
        // cannot align and the caller's own framing is what protects it -- which holds here,
        // since the player hands over whole frames.
        const size_t frame = (this->bytes_per_frame_ != 0)
                                 ? this->bytes_per_frame_
                                 : static_cast<size_t>(this->last_format_.channels) *
                                       (static_cast<size_t>(this->last_format_.bit_depth) / 8U);
        return (frame == 0) ? length : length - (length % frame);
    }

    const size_t bytes_per_frame = this->bytes_per_frame_;
    // Which stream this buffer is for. Re-checked after every wait, because waiting drops the
    // mutex and the stream can be reconfigured or flushed in the meantime.
    const uint64_t generation = this->stream_generation_;
    const size_t usable = length - (length % bytes_per_frame);
    if (usable == 0) {
        // Less than one whole frame: consuming it would mean returning a mid-frame count,
        // which the contract forbids.
        return 0;
    }

    size_t done = 0;
    while (done < usable) {
        if (this->stopping_.load()) {
            break;
        }

        const size_t remaining = usable - done;
        size_t room = this->ring_.free_space();
        if (room < remaining) {
            // Round a partial write down to a frame boundary, so no remainder byte is ever
            // left in the ring: the callback counts frames by integer division, and an
            // untracked remainder makes it report more frames played than were handed over.
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
                       this->stream_ == nullptr || this->stream_generation_ != generation;
            })) {
            break;  // out of time; the caller gets a short write and comes back
        }
        if (this->stream_ == nullptr || this->stream_generation_ != generation) {
            // The stream was closed, reopened or flushed while we had the mutex released. What
            // is left of this buffer belongs to a stream that is gone, and its frame size may
            // not even match the new one -- so stop here and report only what really landed.
            break;
        }
    }

    return done;
}

void PortAudioSink::clear() {
    const std::lock_guard<std::mutex> lock(this->mutex_);

    // A flush ends this buffer's stream as far as a parked write() is concerned: whatever it
    // still holds is audio the player has just asked us to drop.
    ++this->stream_generation_;

    // current_multiplier_ is deliberately *not* snapped here, unlike in AlsaAudioSink::clear().
    // Two reasons, and they agree. The callback keeps running through a flush -- nothing here stops
    // it -- so writing the gain from this thread would break the class's ordering invariant. And it
    // would be wrong anyway: a mid-stream flush is followed by more audio through the same open
    // stream, so a ramp in progress should carry on being heard rather than jump. At a stream *end*
    // the next configure() snaps it before a sample is played, which is where that matters.

    if (this->stream_alive_()) {
        // The consumer owns read_pos_, so the drain has to happen on its side; it takes
        // effect on the callback's next read.
        this->ring_.request_clear();
        return;
    }
    // Nothing is running to carry out a deferred clear, and leaving one pending would play
    // this stream's tail when the next one starts. No consumer means dropping here is safe.
    this->ring_.drop();
}

void PortAudioSink::stop() {
    // Latched before the mutex is taken so a write() already blocked on a full ring sees it
    // and bails out, instead of making shutdown wait for the device to drain.
    this->stopping_.store(true);
    this->space_available_.notify_all();

    const std::lock_guard<std::mutex> lock(this->mutex_);
    // Forgotten before the early return below, not after the close: with no format remembered
    // there is nothing recovery can reopen at, so a write() arriving after shutdown behaves
    // exactly as it did before any of this existed. The stopping_ latch says the same thing, and
    // both are cheap.
    this->last_format_ = {};
    if (this->stream_ == nullptr) {
        return;
    }
    this->close_stream_();
    cli_log(LogLevel::INFO, "portaudio: '%s' closed", this->name().c_str());
}

void PortAudioSink::poll(int64_t now_ms) {
    // Both read without the lock, and first, because on all but a handful of ticks in a run there
    // is nothing to do and no reason to contend with a write() for the mutex. See
    // SinkRecovery::pending(). Shutting down is checked here rather than under the lock so that
    // case is lock-free too, and it is checked before rescan_due() so a shutdown never burns the
    // one rescan there is.
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
    // Reported before the work rather than after it, and always as recovered, because this
    // backend's second attempt really is one-shot: a device-list rebuild that found nothing and
    // one that found the device leave the same nothing left to try. Up front so none of the early
    // returns below can drop it -- the sound-server sinks, whose reconnect *is* worth repeating,
    // report the real outcome instead.
    this->recovery_.rescan_done(true);

    const StreamFormat format = this->last_format_;
    // The stream goes first whatever happens next: Pa_Terminate() with one open is undefined,
    // and every PaDeviceIndex -- device_index_ among them, which this clears -- dies with it.
    // Nothing playing is torn down by this, because only reopen_in_place_() can ask for a rescan
    // and it only runs on a stream that has already died.
    this->close_stream_();

    if (!this->pa_.reinitialize()) {
        // The sink is now inert -- with PortAudio down, even the next configure() cannot resolve
        // a device. Reported rather than fatal, for the reason the constructor gives.
        cli_log(LogLevel::ERROR, "portaudio: could not restart PortAudio to look for '%s': %s",
                this->name().c_str(), this->pa_.error());
        return;
    }

    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, device, error)) {
        cli_log(LogLevel::WARN,
                "portaudio: '%s' is still gone after a device rescan -- discarding until the "
                "next stream (%s)",
                this->name().c_str(), error.c_str());
        return;
    }
    if (!this->open_stream_(device, format.sample_rate, format.channels, format.bit_depth)) {
        return;  // open_stream_() has already said why, once
    }
    if (this->stopping_.load()) {
        // stop() latches before it takes mutex_, so it can arrive while the cycle above is
        // running -- which takes long enough to make that likely rather than theoretical. Same
        // rule as reopen_in_place_(): no opener here leaves a shutdown a fresh device.
        this->close_stream_();
        return;
    }
    // Names the device it landed on, not just the -o spec, because a rescan is exactly what
    // renumbers PortAudio's device list: `-o portaudio:2` after one may well be a different card
    // than it was before. The spec is resolved afresh either way -- so would the next configure()
    // be -- but an operator reading this line should not have to assume which.
    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    cli_log(LogLevel::INFO, "portaudio: '%s' is back after a device rescan, on '%s'",
            this->name().c_str(), (info != nullptr) ? info->name : "(unknown device)");
}

void PortAudioSink::set_volume(uint8_t volume) {
    this->volume_.store(volume > 100 ? 100 : volume);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "portaudio: volume now %u", this->volume_.load());
}

void PortAudioSink::set_muted(bool muted) {
    this->muted_.store(muted);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "portaudio: %s", muted ? "muted" : "unmuted");
}

bool PortAudioSink::open_stream_(PaDeviceIndex device, uint32_t sample_rate, uint8_t channels,
                                 uint8_t bits_per_sample) {
    PaSampleFormat format = 0;
    if (!pa_format_for(bits_per_sample, format)) {
        cli_log(LogLevel::ERROR, "portaudio: unsupported bit depth %u", bits_per_sample);
        return false;
    }
    if (channels == 0 || sample_rate == 0) {
        cli_log(LogLevel::ERROR, "portaudio: refusing stream with %u ch at %u Hz", channels,
                sample_rate);
        return false;
    }

    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (info == nullptr) {
        cli_log(LogLevel::ERROR, "portaudio: device %d disappeared before it could be opened",
                static_cast<int>(device));
        return false;
    }
    if (channels > info->maxOutputChannels) {
        cli_log(LogLevel::ERROR, "portaudio: '%s' has %d output channels, so it cannot play %u",
                info->name, info->maxOutputChannels, channels);
        return false;
    }

    // The format fields go in before the stream is started, so the callback provably cannot be
    // reading them. See the ordering invariant in the header. The generation moves with them:
    // a write() parked on the previous stream must not feed this one.
    ++this->stream_generation_;
    this->device_index_ = device;
    this->rate_ = sample_rate;
    this->channels_ = channels;
    this->bits_ = bits_per_sample;
    this->bytes_per_frame_ =
        static_cast<size_t>(channels) * (static_cast<size_t>(bits_per_sample) / 8U);
    this->stream_rate_ = static_cast<double>(sample_rate);
    this->ramp_step_ = volume_ramp_step(sample_rate);
    // A stream opens at the gain it is meant to be at, never ramping up to it: a restored volume
    // reaches set_volume() before anything has played, so without this the run's first track would
    // open with a fade from a gain that was never applied to a sample. Here rather than in
    // configure() because here is where the callback provably cannot be running.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    PaStreamParameters output_params = {};
    output_params.device = device;
    output_params.channelCount = channels;
    output_params.sampleFormat = format;
    // High rather than low latency, as upstream's reference does: this is a network player, so
    // a comfortable device buffer is worth more than reaction time, and the sync engine
    // compensates for the delay through the timestamps the callback feeds back.
    output_params.suggestedLatency = info->defaultHighOutputLatency;
    output_params.hostApiSpecificStreamInfo = nullptr;

    PaError err =
        Pa_OpenStream(&this->stream_, nullptr, &output_params, sample_rate,
                      paFramesPerBufferUnspecified, paNoFlag, &PortAudioSink::pa_callback, this);
    if (err != paNoError) {
        cli_log(LogLevel::ERROR, "portaudio: '%s' would not open at %u Hz / %u ch / %u-bit: %s",
                info->name, sample_rate, channels, bits_per_sample, Pa_GetErrorText(err));
        this->stream_ = nullptr;
        this->close_stream_();
        return false;
    }

    // What PortAudio actually gave us: the nominal rate drives the callback's frame-to-time
    // maths, and the real device latency is the floor on how small the ring may be. Both are
    // still written before Pa_StartStream(), so the callback cannot be reading either yet --
    // which is what the invariant turns on, not whether a stream handle exists.
    double device_latency_s = 0.0;
    const PaStreamInfo* stream_info = Pa_GetStreamInfo(this->stream_);
    if (stream_info != nullptr) {
        if (stream_info->sampleRate > 0.0) {
            this->stream_rate_ = stream_info->sampleRate;
        }
        device_latency_s = stream_info->outputLatency;
    }
    const size_t capacity = this->ring_capacity_(device_latency_s);
    this->ring_.reset(capacity);

    err = Pa_StartStream(this->stream_);
    if (err != paNoError) {
        cli_log(LogLevel::ERROR, "portaudio: '%s' would not start: %s", info->name,
                Pa_GetErrorText(err));
        this->close_stream_();
        return false;
    }

    this->failed_.store(false);
    cli_log(LogLevel::INFO,
            "portaudio: '%s' (%s) open at %u Hz, %u ch, %u-bit (%zu bytes/frame, "
            "%zu-byte ring, %.1f ms device latency)",
            info->name, this->name().c_str(), sample_rate, channels, bits_per_sample,
            this->bytes_per_frame_, capacity, device_latency_s * 1000.0);
    return true;
}

void PortAudioSink::close_stream_() {
    if (this->stream_ != nullptr) {
        // Abort, not stop: Pa_StopStream() waits for every buffered frame to play out, which
        // would make shutdown as slow as the device buffer -- or hang on a wedged device.
        // Both calls guarantee the callback has finished by the time they return, which is
        // what makes the format fields below safe to touch.
        Pa_AbortStream(this->stream_);
        Pa_CloseStream(this->stream_);
        this->stream_ = nullptr;
    }

    ++this->stream_generation_;
    this->device_index_ = paNoDevice;
    this->rate_ = 0;
    this->channels_ = 0;
    this->bits_ = 0;
    this->bytes_per_frame_ = 0;
    this->stream_rate_ = 0.0;
    this->ramp_step_ = 0;
    this->ring_.reset(0);
    // A write() parked on the old stream would otherwise wait out its whole timeout; its
    // predicate reads stream_, so waking it here lets it return promptly instead.
    this->space_available_.notify_all();
    // stopping_ is deliberately untouched: only stop() and the destructor latch it, and
    // clearing it here would let a mid-stream configure() un-latch a shutdown in progress.
}

bool PortAudioSink::restart_stream_() {
    PaError err = Pa_AbortStream(this->stream_);
    if (err != paNoError && err != paStreamIsStopped) {
        cli_log(LogLevel::DEBUG, "portaudio: cannot abort the stream: %s", Pa_GetErrorText(err));
        return false;
    }

    // A new stream, even on the same device and format: a write() parked on the old one holds
    // audio that has just been dropped.
    ++this->stream_generation_;

    // The callback has stopped, so dropping the ring from this side is safe here.
    this->ring_.drop();
    // And so is snapping the gain, for open_stream_()'s reason: the next stream must start at the
    // gain it is meant to be at rather than fade into it. This is the reuse path, which is what a
    // track boundary at an unchanged format takes.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    err = Pa_StartStream(this->stream_);
    if (err != paNoError) {
        cli_log(LogLevel::DEBUG, "portaudio: cannot restart the stream: %s", Pa_GetErrorText(err));
        return false;
    }

    this->failed_.store(false);
    return true;
}

bool PortAudioSink::reopen_in_place_() {
    if (this->stream_ == nullptr || this->stopping_.load() || this->last_format_.sample_rate == 0) {
        // Recovery is for a stream that was running and has died, and stream_ is what tells that
        // apart from never having had one: PortAudio does not null the handle when a device goes
        // away, but open_stream_() does when it fails. Without this test a device that merely
        // *refused* a format would be chased -- a second identical failed open here, and then a
        // whole device-list rescan on the main loop -- for a stream no rescan can help, since the
        // device is present and simply will not take it.
        //
        // The other two say there is nothing to recover to, or nothing worth recovering: no
        // format has ever been configured, or shutdown has already begun and must not be handed a
        // fresh device. All three are read before the attempt is spent, so none costs the outage
        // anything.
        return false;
    }
    if (!this->recovery_.reopen_due()) {
        return false;
    }

    // Resolved before the stream is closed rather than after, so a host that cannot name the
    // device at all leaves write() with the frame size it already had.
    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, device, error)) {
        cli_log(LogLevel::WARN, "portaudio: cannot reopen '%s': %s", this->name().c_str(),
                error.c_str());
        this->recovery_.reopen_done(false);
        return false;
    }

    const StreamFormat format = this->last_format_;
    this->close_stream_();
    if (!this->open_stream_(device, format.sample_rate, format.channels, format.bit_depth)) {
        this->recovery_.reopen_done(false);  // open_stream_() has already said why, once
        return false;
    }
    this->recovery_.reopen_done(true);

    if (this->stopping_.load()) {
        // stop() latches before it takes mutex_, precisely so a parked write() gives up rather
        // than making shutdown wait -- so the latch can arrive while Pa_OpenStream() is running
        // here. Hand the device straight back rather than leave a live stream for the destructor,
        // which is the hazard configure() refuses to create.
        this->close_stream_();
        return false;
    }

    cli_log(LogLevel::INFO, "portaudio: '%s' recovered without waiting for the next stream",
            this->name().c_str());
    return true;
}

bool PortAudioSink::stream_alive_() const {
    if (this->stream_ == nullptr) {
        return false;
    }
    // Pa_IsStreamActive() goes false without us asking when the device goes away -- a USB DAC
    // unplugged, the host switching outputs. The callback then never runs again, so without
    // this test write() would block for its whole timeout on every single call.
    //
    // That "inactive means the device is gone" reading is only sound because pa_callback() never
    // returns anything but paContinue, so a stream of ours never completes on its own. Anything
    // that taught it paComplete would turn every stream end into an attempted recovery.
    return Pa_IsStreamActive(this->stream_) == 1;
}

size_t PortAudioSink::ring_capacity_(double device_latency_s) const {
    const auto frames_by_time =
        static_cast<size_t>((static_cast<int64_t>(this->rate_) * this->buffer_ms_) / 1000);
    const auto frames_by_latency =
        static_cast<size_t>(device_latency_s * this->stream_rate_ * RING_LATENCY_MULTIPLE);
    const size_t frames = std::max({frames_by_time, frames_by_latency, MIN_RING_FRAMES});
    // --buffer-ms is a request, not a promise: a ring that cannot hold a whole device buffer
    // starves on every callback however promptly write() refills it. Naming which floor won
    // is the difference between "your figure was ignored" and knowing what to ask for
    // instead -- the latency floor moves with the device, MIN_RING_FRAMES does not.
    if (frames > frames_by_time) {
        cli_log(LogLevel::DEBUG,
                "portaudio: --buffer-ms %u is %zu frames at %u Hz, below the %s floor of "
                "%zu frames -- using the floor",
                this->buffer_ms_, frames_by_time, this->rate_,
                (frames_by_latency >= MIN_RING_FRAMES) ? "device-latency" : "minimum-ring", frames);
    }
    // The spare byte the ring keeps to tell full from empty, so `frames` really do fit.
    return (frames * this->bytes_per_frame_) + 1;
}

void PortAudioSink::update_target_multiplier_() {
    // Only the target moves. current_multiplier_ is the callback's to advance, which is what keeps
    // this setter lock-free and the callback free of any synchronisation with it.
    this->target_multiplier_.store(q32_gain_for(this->volume_.load(), this->muted_.load()),
                                   std::memory_order_relaxed);
}

int PortAudioSink::pa_callback(const void* /*input*/, void* output, unsigned long frame_count,
                               const PaStreamCallbackTimeInfo* time_info,
                               PaStreamCallbackFlags /*status_flags*/, void* user_data) {
    const int64_t entered_us = now_us();

    auto* self = static_cast<PortAudioSink*>(user_data);
    auto* out = static_cast<uint8_t*>(output);
    const size_t bytes_requested = frame_count * self->bytes_per_frame_;

    const size_t bytes_read = self->ring_.read(out, bytes_requested);

    // Volume is applied here, on PortAudio's own buffer, rather than on the way into the ring:
    // the callback already owns writable memory, so no scratch copy is needed.
    //
    // The unity fast path tests `current == target` as well, or it would skip a ramp heading *away*
    // from unity -- the one case where the gain is unity and the buffer still needs scaling. A
    // steady unity, which is the common case, costs the same comparison it always did.
    const uint64_t target = self->target_multiplier_.load(std::memory_order_relaxed);
    const uint64_t current = self->current_multiplier_;
    if (current != target || target != Q32_ONE) {
        // Advanced by frame_count -- every frame handed to PortAudio, including any zero-filled
        // tail a short ring read left. See the class docstring: that silence is played, so it
        // consumes ramp time as legitimately as audio does.
        self->current_multiplier_ =
            apply_volume_ramp(out, bytes_requested, self->bits_ / 8U, self->channels_, current,
                              target, self->ramp_step_);
    }

    // Space has just come free; wake whoever is blocked in write(). Notifying without the
    // mutex is what keeps this callback lock-free, which is the whole point of the ring. It
    // does leave a window in which a writer that has just evaluated its predicate, and not yet
    // slept, misses this wakeup -- harmless, because the next callback is only a device buffer
    // away and the writer's own deadline bounds the wait regardless.
    self->space_available_.notify_one();

    // Sync feedback. outputBufferDacTime is when this buffer's first sample reaches the DAC,
    // on PortAudio's own clock; its distance from currentTime is an offset into the future,
    // which carries over to our clock unchanged. Adding the buffer's own duration gives the
    // moment the last frame leaves the DAC, which is what the player's sync maths wants.
    if (bytes_read > 0 && self->on_frames_played) {
        const auto frames_played = static_cast<uint32_t>(bytes_read / self->bytes_per_frame_);
        const double dac_offset_s = (time_info->outputBufferDacTime - time_info->currentTime) +
                                    (static_cast<double>(frames_played) / self->stream_rate_);
        const int64_t finish_us =
            entered_us + static_cast<int64_t>(std::llround(dac_offset_s * 1e6));
        self->on_frames_played(frames_played, finish_us);
    }

    return paContinue;
}

}  // namespace sendspin_cli
