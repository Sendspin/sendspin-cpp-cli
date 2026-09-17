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

#include "alsa_sink.h"

#include "log.h"
#include "pcm_volume.h"

#include <sendspin/client.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// Periods per ring; the period is ALSA's wakeup granularity within --buffer-ms.
constexpr unsigned int PERIODS_PER_BUFFER = 5;

/// Longest snd_pcm_wait() slice, bounding how long shutdown waits on a busy device.
constexpr int WAIT_SLICE_MS = 20;

/// snd_pcm_resume() retries before falling back to prepare(), after a system suspend.
constexpr int RESUME_TRIES = 10;
constexpr auto RESUME_PAUSE = std::chrono::milliseconds(10);

/// ALSA's spelling of PROBE_BIT_DEPTHS; 24-bit is packed S24_3LE, not padded S24_LE.
constexpr std::array<snd_pcm_format_t, PROBE_BIT_DEPTHS.size()> PROBE_FORMATS{
    SND_PCM_FORMAT_S8,
    SND_PCM_FORMAT_S16_LE,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S32_LE,
};
constexpr std::array<const char*, PROBE_BIT_DEPTHS.size()> PROBE_FORMAT_NAMES{"S8", "S16_LE",
                                                                              "S24_3LE", "S32_LE"};

/// Routes libasound's own diagnostics to our logger at DEBUG instead of raw stderr.
void alsa_error_handler(const char* file, int line, const char* function, int err, const char* fmt,
                        ...) {
    if (sendspin::SendspinClient::get_log_level() < LogLevel::DEBUG) {
        return;
    }

    char message[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    cli_log(LogLevel::DEBUG, "alsa: %s:%d %s: %s%s%s", (file != nullptr) ? file : "?", line,
            (function != nullptr) ? function : "?", message, (err != 0) ? " -- " : "",
            (err != 0) ? snd_strerror(err) : "");
}

/// Installs the handler above exactly once, from whichever ALSA entry point runs first.
void install_alsa_error_handler() {
    static std::once_flag once;
    std::call_once(once, [] { snd_lib_error_set_handler(&alsa_error_handler); });
}

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Maps the stream's bit depth onto the interleaved little-endian PCM format ALSA wants.
bool alsa_format_for(uint8_t bits_per_sample, snd_pcm_format_t& format) {
    for (size_t i = 0; i < PROBE_BIT_DEPTHS.size(); ++i) {
        if (PROBE_BIT_DEPTHS[i] == bits_per_sample) {
            format = PROBE_FORMATS[i];
            return true;
        }
    }
    return false;
}

/// Why a probe could not describe a PCM. Ok is the only value with capabilities behind it.
enum class ProbeStatus {
    Ok,
    Busy,           ///< -EBUSY: the name resolves, another process holds it right now
    CannotOpen,     ///< any other snd_pcm_open() failure
    NoInterleaved,  ///< opens, but has no SND_PCM_ACCESS_RW_INTERLEAVED configuration
};

struct ProbeResult {
    ProbeStatus status{ProbeStatus::Ok};
    /// snd_strerror() text, only for CannotOpen.
    std::string detail;
    /// Empty on anything but Ok.
    SinkCapabilities caps;
};

/// Asks one PCM what it will take without keeping it; failures come back as a status.
ProbeResult probe_capabilities(const char* name) {
    ProbeResult result;

    snd_pcm_t* pcm = nullptr;
    // NONBLOCK, so an exclusively held card returns instead of blocking.
    const int err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err == -EBUSY) {
        result.status = ProbeStatus::Busy;
        return result;
    }
    if (err < 0) {
        result.status = ProbeStatus::CannotOpen;
        result.detail = snd_strerror(err);
        return result;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    // Narrowed to interleaved access first, the only mode the sink uses.
    if (snd_pcm_hw_params_any(pcm, hw) < 0 ||
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        snd_pcm_close(pcm);
        result.status = ProbeStatus::NoInterleaved;
        return result;
    }

    for (const uint32_t rate : PROBE_RATES) {
        if (snd_pcm_hw_params_test_rate(pcm, hw, rate, 0) == 0) {
            result.caps.rates.push_back(rate);
        }
    }
    for (size_t i = 0; i < PROBE_BIT_DEPTHS.size(); ++i) {
        if (snd_pcm_hw_params_test_format(pcm, hw, PROBE_FORMATS[i]) == 0) {
            result.caps.bit_depths.push_back(PROBE_BIT_DEPTHS[i]);
        }
    }
    for (const uint8_t count : PROBE_CHANNELS) {
        if (snd_pcm_hw_params_test_channels(pcm, hw, count) == 0) {
            result.caps.channels.push_back(count);
        }
    }

    snd_pcm_close(pcm);
    return result;
}

/// Prints what one PCM will actually take, indented under its name in -l.
void print_device_capabilities(std::FILE* out, const char* name) {
    const ProbeResult result = probe_capabilities(name);
    switch (result.status) {
        case ProbeStatus::Busy:
            std::fprintf(out, "      (in use -- capabilities unknown)\n");
            return;
        case ProbeStatus::CannotOpen:
            std::fprintf(out, "      (cannot open: %s)\n", result.detail.c_str());
            return;
        case ProbeStatus::NoInterleaved:
            std::fprintf(out, "      (no interleaved playback configuration)\n");
            return;
        case ProbeStatus::Ok:
            break;
    }
    print_sink_capabilities(out, result.caps, PROBE_FORMAT_NAMES);
}

}  // namespace

AlsaAudioSink::AlsaAudioSink(std::string device, uint32_t buffer_ms)
    : device_(std::move(device)), buffer_ms_(buffer_ms) {}

AlsaAudioSink::~AlsaAudioSink() {
    // A sink destroyed without stop() must still release the device.
    this->stopping_.store(true);
    const std::lock_guard<std::mutex> lock(this->device_mutex_);
    this->close_device_();
}

std::string AlsaAudioSink::name() const {
    return this->device_;
}

bool AlsaAudioSink::probe(const std::string& device, std::string& error) {
    install_alsa_error_handler();

    snd_pcm_t* pcm = nullptr;
    // NONBLOCK, so a busy exclusive device returns instead of blocking startup.
    const int err = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err == 0) {
        snd_pcm_close(pcm);
        return true;
    }
    if (err == -EBUSY) {
        // Resolves but busy: configure() reports that per stream.
        return true;
    }

    error = "cannot open ALSA device '" + device + "': " + snd_strerror(err) +
            " -- run with -l to list this host's PCMs";
    return false;
}

SinkCapabilities AlsaAudioSink::capabilities() const {
    install_alsa_error_handler();

    // What this name accepts; a converting plug PCM like `default` reports nearly everything.
    const ProbeResult result = probe_capabilities(this->device_.c_str());
    if (result.status != ProbeStatus::Ok) {
        cli_log(LogLevel::DEBUG,
                "alsa: could not probe '%s' -- advertising everything sendspin-cli can emit",
                this->device_.c_str());
        return SinkCapabilities::permissive();
    }
    return result.caps;
}

void AlsaAudioSink::list_devices(std::FILE* out) {
    install_alsa_error_handler();

    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0 || hints == nullptr) {
        std::fprintf(out, "  (could not enumerate ALSA PCMs on this host)\n");
        return;
    }

    for (void** hint = hints; *hint != nullptr; ++hint) {
        char* name = snd_device_name_get_hint(*hint, "NAME");
        char* desc = snd_device_name_get_hint(*hint, "DESC");
        char* ioid = snd_device_name_get_hint(*hint, "IOID");

        // A null IOID means the PCM does both directions; anything else must say Output.
        const bool playback = (ioid == nullptr) || (std::strcmp(ioid, "Output") == 0);
        // Skip PCM names -o reads as a backend prefix instead.
        const bool shadowed = (name != nullptr) && !alsa_pcm_is_reachable(name);

        if (name != nullptr && playback && !shadowed) {
            std::fprintf(out, "  %s\n", name);
            for (const char* line = desc; line != nullptr && *line != '\0';) {
                const char* end = std::strchr(line, '\n');
                const int len = (end != nullptr) ? static_cast<int>(end - line)
                                                 : static_cast<int>(std::strlen(line));
                std::fprintf(out, "      %.*s\n", len, line);
                line = (end != nullptr) ? end + 1 : nullptr;
            }
            print_device_capabilities(out, name);
        }

        std::free(name);
        std::free(desc);
        std::free(ioid);
    }

    snd_device_name_free_hint(hints);
}

bool AlsaAudioSink::open_device_(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    snd_pcm_format_t format = SND_PCM_FORMAT_UNKNOWN;
    if (!alsa_format_for(bits_per_sample, format)) {
        cli_log(LogLevel::ERROR, "alsa: unsupported bit depth %u", bits_per_sample);
        return false;
    }
    if (channels == 0 || sample_rate == 0) {
        cli_log(LogLevel::ERROR, "alsa: refusing stream with %u ch at %u Hz", channels,
                sample_rate);
        return false;
    }

    int err = snd_pcm_open(&this->pcm_, this->device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa: cannot open '%s': %s", this->device_.c_str(),
                snd_strerror(err));
        this->pcm_ = nullptr;
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);

    // Each step names itself so a rejection says which parameter the device refused.
    const char* step = "defaults";
    err = snd_pcm_hw_params_any(this->pcm_, hw);
    if (err >= 0) {
        step = "access (interleaved)";
        err = snd_pcm_hw_params_set_access(this->pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    }
    if (err >= 0) {
        step = "sample format";
        err = snd_pcm_hw_params_set_format(this->pcm_, hw, format);
    }
    if (err >= 0) {
        step = "channel count";
        err = snd_pcm_hw_params_set_channels(this->pcm_, hw, channels);
    }
    if (err >= 0) {
        step = "sample rate";
        // Exact: a device that would silently resample is not what the sync maths assumes.
        err = snd_pcm_hw_params_set_rate(this->pcm_, hw, sample_rate, 0);
    }
    const unsigned int buffer_time_us = this->buffer_ms_ * 1000U;
    if (err >= 0) {
        step = "buffer time";
        unsigned int buffer_time = buffer_time_us;
        err = snd_pcm_hw_params_set_buffer_time_near(this->pcm_, hw, &buffer_time, nullptr);
    }
    if (err >= 0) {
        step = "period time";
        unsigned int period_time = buffer_time_us / PERIODS_PER_BUFFER;
        err = snd_pcm_hw_params_set_period_time_near(this->pcm_, hw, &period_time, nullptr);
    }
    if (err >= 0) {
        step = "hardware parameters";
        err = snd_pcm_hw_params(this->pcm_, hw);
    }
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa: '%s' rejected %s for %u Hz / %u ch / %u-bit: %s",
                this->device_.c_str(), step, sample_rate, channels, bits_per_sample,
                snd_strerror(err));
        this->close_device_();
        return false;
    }

    snd_pcm_uframes_t buffer_size = 0;
    snd_pcm_uframes_t period_size = 0;
    snd_pcm_hw_params_get_buffer_size(hw, &buffer_size);
    err = snd_pcm_hw_params_get_period_size(hw, &period_size, nullptr);
    if (err < 0 || period_size == 0) {
        // A zero period would make the start threshold never trip and write() spin.
        cli_log(LogLevel::ERROR, "alsa: '%s' reported no period size", this->device_.c_str());
        this->close_device_();
        return false;
    }

    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    err = snd_pcm_sw_params_current(this->pcm_, sw);
    if (err >= 0) {
        // Start at the first period, so snd_pcm_delay() is meaningful from the first write.
        err = snd_pcm_sw_params_set_start_threshold(this->pcm_, sw, period_size);
    }
    if (err >= 0) {
        err = snd_pcm_sw_params_set_avail_min(this->pcm_, sw, period_size);
    }
    if (err >= 0) {
        err = snd_pcm_sw_params(this->pcm_, sw);
    }
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa: '%s' rejected software parameters: %s",
                this->device_.c_str(), snd_strerror(err));
        this->close_device_();
        return false;
    }

    err = snd_pcm_prepare(this->pcm_);
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa: cannot prepare '%s': %s", this->device_.c_str(),
                snd_strerror(err));
        this->close_device_();
        return false;
    }

    this->rate_ = sample_rate;
    this->channels_ = channels;
    this->bits_ = bits_per_sample;
    this->bytes_per_frame_ =
        static_cast<size_t>(channels) * (static_cast<size_t>(bits_per_sample) / 8U);
    this->failed_.store(false);

    cli_log(LogLevel::INFO,
            "alsa: '%s' open at %u Hz, %u ch, %u-bit (%zu bytes/frame, %lu-frame ring, "
            "%lu-frame period)",
            this->device_.c_str(), sample_rate, channels, bits_per_sample, this->bytes_per_frame_,
            static_cast<unsigned long>(buffer_size), static_cast<unsigned long>(period_size));
    return true;
}

void AlsaAudioSink::close_device_() {
    if (this->pcm_ == nullptr) {
        return;
    }
    snd_pcm_close(this->pcm_);
    this->pcm_ = nullptr;
    this->rate_ = 0;
    this->channels_ = 0;
    this->bits_ = 0;
    this->bytes_per_frame_ = 0;
}

bool AlsaAudioSink::recover_(int err) {
    if (err == -EINTR || err == -EAGAIN) {
        return true;
    }

    if (err == -EPIPE) {
        // Underrun: routine under load, so prepare and carry on.
        const int prepared = snd_pcm_prepare(this->pcm_);
        if (prepared < 0) {
            // A prepare() that fails means the device is gone, not just the ring.
            cli_log(LogLevel::ERROR, "alsa: underrun recovery failed: %s", snd_strerror(prepared));
            return this->handle_device_loss_();
        }
        cli_log(LogLevel::DEBUG, "alsa: underrun recovered");
        return true;
    }

    if (err == -ESTRPIPE) {
        // Suspended: resume keeps the position, prepare is the fallback.
        for (int i = 0; i < RESUME_TRIES; ++i) {
            const int resumed = snd_pcm_resume(this->pcm_);
            if (resumed != -EAGAIN) {
                if (resumed >= 0) {
                    cli_log(LogLevel::INFO, "alsa: resumed after suspend");
                    return true;
                }
                break;
            }
            std::this_thread::sleep_for(RESUME_PAUSE);
        }
        const int prepared = snd_pcm_prepare(this->pcm_);
        if (prepared < 0) {
            cli_log(LogLevel::ERROR, "alsa: suspend recovery failed: %s", snd_strerror(prepared));
            return this->handle_device_loss_();
        }
        cli_log(LogLevel::INFO, "alsa: restarted after suspend");
        return true;
    }

    // Every other error is taken as the device being gone; a wrong guess costs only a reopen.
    cli_log(LogLevel::ERROR, "alsa: '%s' is gone (%s)", this->device_.c_str(), snd_strerror(err));
    return this->handle_device_loss_();
}

bool AlsaAudioSink::handle_device_loss_() {
    // Close now so write() takes the discard path instead of spinning on dead hardware.
    this->close_device_();

    // Spend the inline reopen unmade: snd_pcm_open() is unbounded, so escalate to poll().
    if (this->recovery_.reopen_due()) {
        this->recovery_.reopen_done(false);
    }
    return false;
}

bool AlsaAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const std::lock_guard<std::mutex> lock(this->device_mutex_);

    // Open at the target gain, never ramping up to a restored volume.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    // Before anything can fail: poll() reopens at this format.
    this->last_format_ = {sample_rate, channels, bits_per_sample};
    // A new stream starts from zero buffered frames; here, not beside reset(), so a failed open
    // drops the gap too.
    this->recovery_.forget_discarded_frames();

    if (this->pcm_ != nullptr && this->rate_ == sample_rate && this->channels_ == channels &&
        this->bits_ == bits_per_sample) {
        // Same format: keep the device and restart from a clean ring.
        snd_pcm_drop(this->pcm_);
        const int err = snd_pcm_prepare(this->pcm_);
        if (err == 0) {
            cli_log(LogLevel::DEBUG, "alsa: reusing '%s' at %u Hz, %u ch, %u-bit",
                    this->device_.c_str(), sample_rate, channels, bits_per_sample);
            // Never reset in open_device_(), which poll() calls mid-attempt.
            this->recovery_.reset();
            return true;
        }
        // Went away between tracks; fall through to the reopen.
        cli_log(LogLevel::WARN, "alsa: could not restart '%s' (%s) -- reopening",
                this->device_.c_str(), snd_strerror(err));
    }

    this->close_device_();
    if (!this->open_device_(sample_rate, channels, bits_per_sample)) {
        this->failed_.store(true);
        return false;
    }
    this->recovery_.reset();
    return true;
}

size_t AlsaAudioSink::write(const uint8_t* data, size_t length, uint32_t timeout_ms) {
    if (data == nullptr || length == 0) {
        return 0;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    size_t frames_done = 0;
    size_t bytes_per_frame = 0;
    int64_t finish_us = 0;
    bool have_timing = false;
    uint32_t gap_frames = 0;

    {
        const std::lock_guard<std::mutex> lock(this->device_mutex_);

        if (this->pcm_ == nullptr || this->bytes_per_frame_ == 0) {
            // Discard rather than return 0 forever, which would spin the sync task.
            if (!this->failed_.exchange(true)) {
                cli_log(LogLevel::ERROR,
                        "alsa: '%s' is not open -- discarding audio until it is back or a "
                        "stream reconfigures it",
                        this->device_.c_str());
            }
            // Frame-aligned via last_format_, since close_device_() zeroed bytes_per_frame_.
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

        bytes_per_frame = this->bytes_per_frame_;
        const size_t usable = length - (length % bytes_per_frame);
        if (usable == 0) {
            return 0;
        }

        // Fast paths also require current == target, or they would cut a ramp short.
        const uint8_t* src = data;
        const uint64_t target = this->target_multiplier_.load(std::memory_order_relaxed);
        const uint64_t start = this->current_multiplier_;
        const uint64_t step = volume_ramp_step(this->rate_);
        const bool steady = start == target;
        if (!steady || target != Q32_ONE) {
            this->scaled_.resize(usable);
            if (steady && target == 0) {
                std::memset(this->scaled_.data(), 0, usable);
            } else {
                std::memcpy(this->scaled_.data(), data, usable);
                apply_volume_ramp(this->scaled_.data(), usable, this->bits_ / 8U, this->channels_,
                                  start, target, step);
            }
            src = this->scaled_.data();
        }

        const size_t frames_total = usable / bytes_per_frame;
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

            const snd_pcm_sframes_t avail = snd_pcm_avail_update(this->pcm_);
            if (avail < 0) {
                if (!this->recover_(static_cast<int>(avail))) {
                    break;
                }
                continue;
            }
            if (avail == 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      deadline - std::chrono::steady_clock::now())
                                      .count();
                if (left <= 0) {
                    break;
                }
                // Short slices keep stop() from waiting out the whole ring.
                const int slice = static_cast<int>(std::min<int64_t>(left, WAIT_SLICE_MS));
                const int waited = snd_pcm_wait(this->pcm_, slice);
                if (waited < 0 && !this->recover_(waited)) {
                    break;
                }
                continue;
            }

            const snd_pcm_uframes_t chunk = std::min<snd_pcm_uframes_t>(
                frames_total - frames_done, static_cast<snd_pcm_uframes_t>(avail));
            const snd_pcm_sframes_t written =
                snd_pcm_writei(this->pcm_, src + (frames_done * bytes_per_frame), chunk);
            if (written < 0) {
                if (!this->recover_(static_cast<int>(written))) {
                    break;
                }
                continue;
            }
            frames_done += static_cast<size_t>(written);
        }

        // Commit the ramp by frames written, not scaled: an unwritten tail is re-presented.
        this->current_multiplier_ = ramped_gain(start, target, step, frames_done);

        // Frames written finish snd_pcm_delay() into the future; pcm_ may have been closed above.
        if (frames_done > 0 && this->pcm_ != nullptr && this->rate_ > 0) {
            snd_pcm_sframes_t delay = 0;
            if (snd_pcm_delay(this->pcm_, &delay) == 0 && delay >= 0) {
                finish_us = now_us() + ((static_cast<int64_t>(delay) * 1000000) / this->rate_);
                have_timing = true;
                // Taken with the timestamp under the same lock, so nothing reports between them.
                gap_frames = this->recovery_.take_discarded_frames();
            }
        }

        // recover_() closed the device mid-write: these bytes are consumed but never timestamped.
        if (frames_done > 0 && this->pcm_ == nullptr) {
            this->recovery_.discard_frames(static_cast<uint32_t>(frames_done));
        }
    }

    // Outside the lock, so a callback that touches the sink cannot deadlock. One report covers
    // the gap and this write: the player keeps only the last timestamp it has not read.
    if (have_timing && this->on_frames_played) {
        const uint64_t played = static_cast<uint64_t>(gap_frames) + frames_done;
        const uint64_t ceiling = std::numeric_limits<uint32_t>::max();
        this->on_frames_played(static_cast<uint32_t>(std::min(played, ceiling)), finish_us);
    }

    return frames_done * bytes_per_frame;
}

void AlsaAudioSink::clear() {
    const std::lock_guard<std::mutex> lock(this->device_mutex_);

    // Safe only under device_mutex_; PortAudioSink::clear() must not do the same.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    // Before the early return: the gap's stream is over. Only the gap goes; reset() owns the
    // budget.
    this->recovery_.forget_discarded_frames();

    if (this->pcm_ == nullptr) {
        return;
    }

    // A flush, not a close: the handle and format survive for the next writes.
    snd_pcm_drop(this->pcm_);
    const int err = snd_pcm_prepare(this->pcm_);
    if (err < 0) {
        cli_log(LogLevel::WARN, "alsa: could not re-prepare '%s' after clear: %s",
                this->device_.c_str(), snd_strerror(err));
    }
}

void AlsaAudioSink::stop() {
    // Before the lock, so a blocked write() bails out.
    this->stopping_.store(true);

    const std::lock_guard<std::mutex> lock(this->device_mutex_);
    // Before the early return, so poll() has nothing to reopen after shutdown.
    this->last_format_ = {};
    if (this->pcm_ == nullptr) {
        return;
    }
    // drop(), not drain(): shutdown should be prompt.
    snd_pcm_drop(this->pcm_);
    this->close_device_();
    cli_log(LogLevel::INFO, "alsa: '%s' closed", this->device_.c_str());
}

void AlsaAudioSink::set_volume(uint8_t volume) {
    this->volume_.store(volume > 100 ? 100 : volume);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "alsa: volume now %u", this->volume_.load());
}

void AlsaAudioSink::set_muted(bool muted) {
    this->muted_.store(muted);
    this->update_target_multiplier_();
    cli_log(LogLevel::DEBUG, "alsa: %s", muted ? "muted" : "unmuted");
}

void AlsaAudioSink::poll(int64_t now_ms) {
    // Unlocked fast path: nothing to do on almost every tick.
    if (!this->recovery_.pending() || this->stopping_.load()) {
        return;
    }

    const std::lock_guard<std::mutex> lock(this->device_mutex_);
    if (this->last_format_.sample_rate == 0) {
        return;  // nothing was ever configured, so there is nothing to reopen at
    }
    if (!this->recovery_.rescan_due(now_ms)) {
        return;
    }

    // Reopen on the main loop, where an unbounded snd_pcm_open() is affordable.
    const StreamFormat format = this->last_format_;
    this->close_device_();  // idempotent; recover_() has normally closed it already
    if (!this->open_device_(format.sample_rate, format.channels, format.bit_depth)) {
        // Failure buys another attempt after a longer delay.
        this->recovery_.rescan_done(false);
        const bool retrying = this->recovery_.pending();
        cli_log(retrying ? LogLevel::DEBUG : LogLevel::WARN, "alsa: '%s' is not back%s",
                this->device_.c_str(),
                retrying ? " -- trying again shortly" : " -- discarding until the next stream");
        return;
    }
    if (this->stopping_.load()) {
        // stop() can land during the open above; release the device now.
        this->recovery_.rescan_done(true);  // shutting down; there is nothing left to retry for
        this->close_device_();
        return;
    }
    // The gap stays pending: reporting it here would race write()'s first timed report.
    this->recovery_.rescan_done(true);
    cli_log(LogLevel::INFO, "alsa: '%s' is back -- recovered without waiting for the next stream",
            this->device_.c_str());
}

void AlsaAudioSink::update_target_multiplier_() {
    // Only the target moves; write() walks current_multiplier_ toward it.
    this->target_multiplier_.store(q32_gain_for(this->volume_.load(), this->muted_.load()),
                                   std::memory_order_relaxed);
}

}  // namespace sendspin_cli
