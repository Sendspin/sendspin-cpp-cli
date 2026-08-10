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

namespace {

/// Target ring size, as a duration rather than upstream's fixed 16 KB: the same byte count
/// is 85 ms at 48 kHz/16-bit/stereo but only 10 ms at 192 kHz/32-bit, which would underrun
/// on every callback. Matches the 100 ms AlsaAudioSink asks ALSA for. Making it tunable is
/// squeezelite's -a flag, which this task leaves to a follow-up.
constexpr int64_t RING_TIME_US = 100000;

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

/// Maps the stream's bit depth onto the interleaved little-endian PCM format PortAudio wants.
/// 24-bit is paInt24, three packed bytes, because the player hands us tightly packed samples.
///
/// 8-bit is here although upstream's reference refuses it: AlsaAudioSink accepts S8, and a
/// stream one backend plays and the other does not would be a difference with no cause.
bool pa_format_for(uint8_t bits_per_sample, PaSampleFormat& format) {
    switch (bits_per_sample) {
        case 8:
            format = paInt8;
            return true;
        case 16:
            format = paInt16;
            return true;
        case 24:
            format = paInt24;
            return true;
        case 32:
            format = paInt32;
            return true;
        default:
            return false;
    }
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

// ============================================================================
// PortAudioRingBuffer
// ============================================================================

size_t PortAudioRingBuffer::write(const uint8_t* data, size_t len) {
    if (this->capacity_ == 0) {
        return 0;
    }

    const size_t write_pos = this->write_pos_.load(std::memory_order_relaxed);
    const size_t read_pos = this->read_pos_.load(std::memory_order_acquire);

    // One byte is always left spare, which is what tells a full ring from an empty one.
    const size_t used = (write_pos - read_pos + this->capacity_) % this->capacity_;
    const size_t to_write = std::min(len, this->capacity_ - 1 - used);
    if (to_write == 0) {
        return 0;
    }

    const size_t first_chunk = std::min(to_write, this->capacity_ - write_pos);
    std::memcpy(&this->buffer_[write_pos], data, first_chunk);
    if (to_write > first_chunk) {
        std::memcpy(&this->buffer_[0], data + first_chunk, to_write - first_chunk);
    }

    this->write_pos_.store((write_pos + to_write) % this->capacity_, std::memory_order_release);
    return to_write;
}

size_t PortAudioRingBuffer::read(uint8_t* dest, size_t len) {
    if (this->capacity_ == 0) {
        std::memset(dest, 0, len);
        return 0;
    }

    // A clear the producer asked for is carried out here, on the consumer's side, so that
    // read_pos_ keeps its single writer. Doing it from the producer would let the consumer
    // compute an available count from a position that moved under it.
    if (this->clear_requested_.load(std::memory_order_acquire)) {
        this->clear_requested_.store(false, std::memory_order_relaxed);
        this->read_pos_.store(this->write_pos_.load(std::memory_order_acquire),
                              std::memory_order_release);
        std::memset(dest, 0, len);
        return 0;
    }

    const size_t read_pos = this->read_pos_.load(std::memory_order_relaxed);
    const size_t write_pos = this->write_pos_.load(std::memory_order_acquire);

    const size_t available = (write_pos - read_pos + this->capacity_) % this->capacity_;
    const size_t to_read = std::min(len, available);

    if (to_read > 0) {
        const size_t first_chunk = std::min(to_read, this->capacity_ - read_pos);
        std::memcpy(dest, &this->buffer_[read_pos], first_chunk);
        if (to_read > first_chunk) {
            std::memcpy(dest + first_chunk, &this->buffer_[0], to_read - first_chunk);
        }
        this->read_pos_.store((read_pos + to_read) % this->capacity_, std::memory_order_release);
    }

    if (to_read < len) {
        std::memset(dest + to_read, 0, len - to_read);
    }
    return to_read;
}

size_t PortAudioRingBuffer::available() const {
    if (this->capacity_ == 0) {
        return 0;
    }
    const size_t write_pos = this->write_pos_.load(std::memory_order_acquire);
    const size_t read_pos = this->read_pos_.load(std::memory_order_acquire);
    return (write_pos - read_pos + this->capacity_) % this->capacity_;
}

size_t PortAudioRingBuffer::free_space() const {
    if (this->capacity_ == 0) {
        return 0;
    }
    return this->capacity_ - 1 - this->available();
}

void PortAudioRingBuffer::request_clear() {
    this->clear_requested_.store(true, std::memory_order_release);
}

void PortAudioRingBuffer::drop() {
    this->clear_requested_.store(false, std::memory_order_relaxed);
    this->read_pos_.store(0, std::memory_order_relaxed);
    this->write_pos_.store(0, std::memory_order_release);
}

void PortAudioRingBuffer::reset(size_t capacity) {
    this->buffer_.assign(capacity, 0);
    this->capacity_ = capacity;
    this->drop();
}

// ============================================================================
// PortAudioSink
// ============================================================================

PortAudioSink::PortAudioSink(std::string device) : device_(std::move(device)) {
    if (!this->pa_.ok()) {
        // Reported rather than thrown: make_audio_sink() has already run probe(), so getting
        // here means PortAudio came up once and then would not again. configure() will fail
        // and the sink degrades to discarding, which beats taking the daemon down.
        cli_log(LogLevel::ERROR, "portaudio sink: cannot initialise PortAudio: %s",
                this->pa_.error());
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
        cli_log(LogLevel::DEBUG, "portaudio sink: ignoring a stream start during shutdown");
        return false;
    }

    // Resolved per stream, not once at construction: a bare -o portaudio then follows the
    // host's default output as the user changes it, and a device that was absent at startup
    // is picked up whenever the next stream starts.
    PaDeviceIndex device = paNoDevice;
    std::string error;
    if (!resolve_pa_device(this->device_, device, error)) {
        cli_log(LogLevel::ERROR, "portaudio sink: %s", error.c_str());
        this->failed_.store(true);
        return false;
    }

    if (this->stream_ != nullptr && this->device_index_ == device && this->rate_ == sample_rate &&
        this->channels_ == channels && this->bits_ == bits_per_sample) {
        // Same device and format as the stream that just ended. Restarting from an empty ring
        // is far cheaper than a close/open round trip, and it spares a CoreAudio device the
        // reopen -- and the gap that comes with it -- at every track boundary.
        if (this->restart_stream_()) {
            cli_log(LogLevel::DEBUG,
                    "portaudio sink: reusing the open stream at %u Hz, %u ch, %u-bit", sample_rate,
                    channels, bits_per_sample);
            return true;
        }
        cli_log(LogLevel::WARN, "portaudio sink: could not restart the stream -- reopening");
    }

    this->close_stream_();
    if (!this->open_stream_(device, sample_rate, channels, bits_per_sample)) {
        this->failed_.store(true);
        return false;
    }
    return true;
}

size_t PortAudioSink::write(const uint8_t* data, size_t length, uint32_t timeout_ms) {
    if (data == nullptr || length == 0) {
        return 0;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(this->mutex_);

    if (!this->stream_alive_() || this->bytes_per_frame_ == 0) {
        // No stream to feed: swallow the audio rather than return 0 forever, which would spin
        // the sync task on a buffer it can never hand off.
        if (!this->failed_.exchange(true)) {
            cli_log(LogLevel::ERROR,
                    "portaudio sink: '%s' is not playing -- discarding audio until a stream "
                    "reconfigures it",
                    this->name().c_str());
        }
        // Frame-aligned per the write() contract wherever the frame size is still known. Once
        // it is not, the sink cannot align and the caller's own framing is what protects it --
        // which holds here, since the player hands over whole frames.
        return (this->bytes_per_frame_ == 0) ? length : length - (length % this->bytes_per_frame_);
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
    if (this->stream_ == nullptr) {
        return;
    }
    this->close_stream_();
    cli_log(LogLevel::INFO, "portaudio sink: '%s' closed", this->name().c_str());
}

void PortAudioSink::set_volume(uint8_t volume) {
    this->volume_.store(volume > 100 ? 100 : volume);
    this->update_volume_multiplier_();
    cli_log(LogLevel::DEBUG, "portaudio sink: volume now %u", this->volume_.load());
}

void PortAudioSink::set_muted(bool muted) {
    this->muted_.store(muted);
    this->update_volume_multiplier_();
    cli_log(LogLevel::DEBUG, "portaudio sink: %s", muted ? "muted" : "unmuted");
}

bool PortAudioSink::open_stream_(PaDeviceIndex device, uint32_t sample_rate, uint8_t channels,
                                 uint8_t bits_per_sample) {
    PaSampleFormat format = 0;
    if (!pa_format_for(bits_per_sample, format)) {
        cli_log(LogLevel::ERROR, "portaudio sink: unsupported bit depth %u", bits_per_sample);
        return false;
    }
    if (channels == 0 || sample_rate == 0) {
        cli_log(LogLevel::ERROR, "portaudio sink: refusing stream with %u ch at %u Hz", channels,
                sample_rate);
        return false;
    }

    const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
    if (info == nullptr) {
        cli_log(LogLevel::ERROR, "portaudio sink: device %d disappeared before it could be opened",
                static_cast<int>(device));
        return false;
    }
    if (channels > info->maxOutputChannels) {
        cli_log(LogLevel::ERROR,
                "portaudio sink: '%s' has %d output channels, so it cannot play %u", info->name,
                info->maxOutputChannels, channels);
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
        cli_log(LogLevel::ERROR,
                "portaudio sink: '%s' would not open at %u Hz / %u ch / %u-bit: %s", info->name,
                sample_rate, channels, bits_per_sample, Pa_GetErrorText(err));
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
        cli_log(LogLevel::ERROR, "portaudio sink: '%s' would not start: %s", info->name,
                Pa_GetErrorText(err));
        this->close_stream_();
        return false;
    }

    this->failed_.store(false);
    cli_log(LogLevel::INFO,
            "portaudio sink: '%s' (%s) open at %u Hz, %u ch, %u-bit (%zu bytes/frame, "
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
        cli_log(LogLevel::DEBUG, "portaudio sink: cannot abort the stream: %s",
                Pa_GetErrorText(err));
        return false;
    }

    // A new stream, even on the same device and format: a write() parked on the old one holds
    // audio that has just been dropped.
    ++this->stream_generation_;

    // The callback has stopped, so dropping the ring from this side is safe here.
    this->ring_.drop();

    err = Pa_StartStream(this->stream_);
    if (err != paNoError) {
        cli_log(LogLevel::DEBUG, "portaudio sink: cannot restart the stream: %s",
                Pa_GetErrorText(err));
        return false;
    }

    this->failed_.store(false);
    return true;
}

bool PortAudioSink::stream_alive_() const {
    if (this->stream_ == nullptr) {
        return false;
    }
    // Pa_IsStreamActive() goes false without us asking when the device goes away -- a USB DAC
    // unplugged, the host switching outputs. The callback then never runs again, so without
    // this test write() would block for its whole timeout on every single call.
    return Pa_IsStreamActive(this->stream_) == 1;
}

size_t PortAudioSink::ring_capacity_(double device_latency_s) const {
    const auto frames_by_time =
        static_cast<size_t>((static_cast<int64_t>(this->rate_) * RING_TIME_US) / 1000000);
    const auto frames_by_latency =
        static_cast<size_t>(device_latency_s * this->stream_rate_ * RING_LATENCY_MULTIPLE);
    const size_t frames = std::max({frames_by_time, frames_by_latency, MIN_RING_FRAMES});
    // The spare byte the ring keeps to tell full from empty, so `frames` really do fit.
    return (frames * this->bytes_per_frame_) + 1;
}

void PortAudioSink::update_volume_multiplier_() {
    this->volume_multiplier_.store(q32_gain_for(this->volume_.load(), this->muted_.load()),
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
    const uint64_t gain = self->volume_multiplier_.load(std::memory_order_relaxed);
    if (gain < Q32_ONE) {
        apply_volume(out, bytes_requested, self->bits_ / 8U, gain);
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
