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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

/// Both sinks and the -o resolver share one tag: which backend a line came from is
/// already in the message, and "audio" is what an operator greps for.
static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// How many periods the ring is divided into. The ring itself comes from --buffer-ms; the
/// period is what ALSA wakes us on, so this is the granularity/overhead trade rather than a
/// second knob. Five keeps the 100 ms ring / 20 ms period the defaults have always meant.
constexpr unsigned int PERIODS_PER_BUFFER = 5;

/// Longest single snd_pcm_wait() slice. write() re-checks the abort flag and the caller's
/// deadline between slices, so this bounds how long a shutdown waits on a busy device.
constexpr int WAIT_SLICE_MS = 20;

/// How many times to retry snd_pcm_resume() before falling back to prepare(), and how
/// long to pause between tries. Only reached when the host resumes from suspend.
constexpr int RESUME_TRIES = 10;
constexpr auto RESUME_PAUSE = std::chrono::milliseconds(10);

/// How each of PROBE_BIT_DEPTHS is spelled on this backend, in the same order.
///
/// The rate, depth and channel ladders themselves are shared with PortAudio (audio_sink.h);
/// only the spelling is ALSA's. 24-bit is S24_3LE (three packed bytes), not S24_LE (three
/// bytes padded into four), because the player hands us tightly packed samples.
constexpr std::array<snd_pcm_format_t, PROBE_BIT_DEPTHS.size()> PROBE_FORMATS{
    SND_PCM_FORMAT_S8,
    SND_PCM_FORMAT_S16_LE,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S32_LE,
};
constexpr std::array<const char*, PROBE_BIT_DEPTHS.size()> PROBE_FORMAT_NAMES{"S8", "S16_LE",
                                                                              "S24_3LE", "S32_LE"};

/// Routes libasound's own diagnostics through our logger instead of letting it write
/// straight to stderr.
///
/// ALSA's default handler prints things like "Unknown PCM bogus:9,9" unconditionally,
/// which both duplicates the error we report ourselves and spews during shutdown. At
/// DEBUG they are still there for diagnosis, just not in normal output.
void alsa_error_handler(const char* file, int line, const char* function, int err,
                        const char* fmt, ...) {
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
    /// Empty on anything but Ok. Callers that need an answer regardless substitute
    /// SinkCapabilities::permissive(); -l prints the status instead.
    SinkCapabilities caps;
};

/// Asks one PCM what it will take, without keeping it.
///
/// The single source of truth behind both -l's per-PCM detail and the advertised format
/// list, so the two agree by construction rather than by two ladders happening to match.
///
/// The cost is the one snd_pcm_open(). Everything after it queries a configuration space
/// already in memory, so testing eight rates and four formats is no dearer than testing one.
///
/// Degrades, never fails: a busy or unopenable device comes back with a status, because one
/// card nobody can open must cost neither the rest of -l's listing nor the player's startup.
ProbeResult probe_capabilities(const char* name) {
    ProbeResult result;

    snd_pcm_t* pcm = nullptr;
    // NONBLOCK for probe()'s reason: a card another process holds exclusively must come
    // back rather than park the caller on it.
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
    // Narrowed to the access mode the sink actually uses before anything is tested, so the
    // report describes how sendspin-cli would drive the card, not what it can do in modes
    // this player never asks for.
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
    // stop() is the documented shutdown path, but a sink destroyed without it must still
    // hand the device back rather than leak it to the next process that wants exclusive use.
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
    // NONBLOCK so probing a busy exclusive device returns instead of parking the startup
    // path on it.
    const int err = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err == 0) {
        snd_pcm_close(pcm);
        return true;
    }
    if (err == -EBUSY) {
        // The name resolves; something else just holds it right now. That is configure()'s
        // problem to report per stream, not a reason to refuse to start.
        return true;
    }

    error = "cannot open ALSA device '" + device + "': " + snd_strerror(err) +
            " -- run with -l to list this host's PCMs";
    return false;
}

SinkCapabilities AlsaAudioSink::capabilities() const {
    install_alsa_error_handler();

    // What this *device name* will take, which is the question the advertisement answers --
    // not a description of the hardware. `default` is usually PipeWire's or PulseAudio's ALSA
    // plugin, and a plug layer converts, so it reports nearly everything whatever card is
    // behind it. That is still the honest answer to "what can I push through this -o value".
    const ProbeResult result = probe_capabilities(this->device_.c_str());
    if (result.status != ProbeStatus::Ok) {
        // Busy, absent or interleaved-incapable. Advertising nothing would leave the player
        // unable to play at all, where being over-broad costs at worst a per-stream refusal.
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
        // ALSA ships its own "null" PCM, but -o null is reserved for the discard sink
        // above, so listing it here would name a device -o cannot actually reach.
        const bool shadowed = (name != nullptr) && (std::strcmp(name, "null") == 0);

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
        // Derived from the ring rather than asked for separately: --buffer-ms is one figure
        // for both backends, and a period is the wakeup granularity within it.
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
        // Both software parameters below are the period, so a zero would set a start
        // threshold that never trips and an avail_min that turns write()'s snd_pcm_wait()
        // into a spin. Refusing here degrades to the discard path, which is bounded.
        cli_log(LogLevel::ERROR, "alsa: '%s' reported no period size", this->device_.c_str());
        this->close_device_();
        return false;
    }

    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    err = snd_pcm_sw_params_current(this->pcm_, sw);
    if (err >= 0) {
        // Start at the first period boundary rather than waiting for a full ring. What that
        // buys is a truthful sync signal from the first write: snd_pcm_delay() only counts
        // frames queued ahead of a *running* stream, so a device still waiting to start
        // reports a finish time that has not begun ticking. What it costs is the underrun
        // margin those unplayed frames represented -- recover_() turns an -EPIPE into a
        // prepare() and playback continues, and --buffer-ms is the knob for a host that
        // needs more headroom.
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
        // Underrun: the ring drained before we refilled it. Prepare and carry on -- this is
        // routine under load, so it is a debug line rather than a warning per occurrence.
        const int prepared = snd_pcm_prepare(this->pcm_);
        if (prepared < 0) {
            cli_log(LogLevel::ERROR, "alsa: underrun recovery failed: %s", snd_strerror(prepared));
            return false;
        }
        cli_log(LogLevel::DEBUG, "alsa: underrun recovered");
        return true;
    }

    if (err == -ESTRPIPE) {
        // The device was suspended (system sleep). Resume is what keeps the stream's
        // position; prepare is the fallback that restarts it if resume is unsupported.
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
            return false;
        }
        cli_log(LogLevel::INFO, "alsa: restarted after suspend");
        return true;
    }

    cli_log(LogLevel::ERROR, "alsa: %s", snd_strerror(err));
    return false;
}

bool AlsaAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const std::lock_guard<std::mutex> lock(this->device_mutex_);

    // A stream opens at the gain it is meant to be at, never ramping up to it. Without this a
    // restored volume -- which reaches set_volume() before anything has played -- would leave the
    // sink at unity with a remembered target, and the run's first track would open with a fade
    // nobody asked for, from a gain that was never applied to a sample.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    if (this->pcm_ != nullptr && this->rate_ == sample_rate && this->channels_ == channels &&
        this->bits_ == bits_per_sample) {
        // Same format as the stream that just ended: keep the device and just start from a
        // clean ring, which is cheaper than a close/open round trip and avoids handing an
        // exclusive device back to a competitor mid-track.
        snd_pcm_drop(this->pcm_);
        const int err = snd_pcm_prepare(this->pcm_);
        if (err < 0) {
            cli_log(LogLevel::WARN, "alsa: could not restart '%s' (%s) -- reopening",
                    this->device_.c_str(), snd_strerror(err));
            this->close_device_();
            return this->open_device_(sample_rate, channels, bits_per_sample);
        }
        cli_log(LogLevel::DEBUG, "alsa: reusing '%s' at %u Hz, %u ch, %u-bit",
                this->device_.c_str(), sample_rate, channels, bits_per_sample);
        return true;
    }

    // A format change means the hardware parameters change, and those are fixed for the
    // life of an open handle -- so the device has to be closed and reopened.
    this->close_device_();
    if (!this->open_device_(sample_rate, channels, bits_per_sample)) {
        this->failed_.store(true);
        return false;
    }
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

    {
        const std::lock_guard<std::mutex> lock(this->device_mutex_);

        if (this->pcm_ == nullptr || this->bytes_per_frame_ == 0) {
            // No usable device: swallow the audio rather than return 0 forever, which would
            // spin the sync task on a buffer it can never hand off.
            if (!this->failed_.exchange(true)) {
                cli_log(LogLevel::ERROR,
                        "alsa: '%s' is not open -- discarding audio until a stream "
                        "reconfigures it",
                        this->device_.c_str());
            }
            // Frame-aligned per the write() contract wherever the frame size is still known.
            // Once it is not, the sink cannot align and the caller's own framing is what
            // protects it -- which holds here, since the player hands over whole frames.
            return (this->bytes_per_frame_ == 0) ? length
                                                 : length - (length % this->bytes_per_frame_);
        }

        bytes_per_frame = this->bytes_per_frame_;
        const size_t usable = length - (length % bytes_per_frame);
        if (usable == 0) {
            // Less than one whole frame: consuming it would mean returning a mid-frame
            // count, which the contract forbids.
            return 0;
        }

        // Volume: scale into scratch so the caller's buffer stays untouched.
        //
        // Both fast paths test `current == target` as well as the gain itself, and that is what
        // makes them safe alongside the ramp: at unity they would otherwise skip a ramp *away*
        // from unity, and the memset would flatten a ramp still on its way down to silence. A
        // steady unity still costs nothing, which is the case that matters.
        const uint8_t* src = data;
        const uint64_t target = this->target_multiplier_.load(std::memory_order_relaxed);
        const uint64_t start = this->current_multiplier_;
        const uint64_t step = volume_ramp_step(this->rate_);
        const bool steady = start == target;
        if (!steady || target != Q32_ONE) {
            this->scaled_.resize(usable);
            if (steady && target == 0) {
                // Silence. Zero bytes are silence for the signed PCM the player advertises.
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

            const snd_pcm_uframes_t chunk =
                std::min<snd_pcm_uframes_t>(frames_total - frames_done,
                                            static_cast<snd_pcm_uframes_t>(avail));
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

        // Commit the ramp by what was really written, not by what was scaled. The loop above can
        // break out with frames left over -- a deadline, stopping_, a recover_() that failed -- and
        // the sync task re-presents that tail on the next call. Advancing by frames_total would put
        // the gain ahead of the audio and leave a step across the seam. Recomputed through
        // ramped_gain() rather than tracked by the scaler, so there is one definition of the
        // arithmetic and the two cannot drift.
        this->current_multiplier_ = ramped_gain(start, target, step, frames_done);

        // Sync feedback: snd_pcm_delay() is how many frames are still queued ahead of the
        // DAC, so the frames just written finish that far into the future. Sampled here,
        // under the lock, so the timestamp matches the query.
        if (frames_done > 0 && this->rate_ > 0) {
            snd_pcm_sframes_t delay = 0;
            if (snd_pcm_delay(this->pcm_, &delay) == 0 && delay >= 0) {
                finish_us = now_us() + ((static_cast<int64_t>(delay) * 1000000) / this->rate_);
                have_timing = true;
            }
        }
    }

    // Fired outside the lock: notify_audio_played() runs the player's own bookkeeping, and
    // holding the device mutex across a callback is how a future callback that touches the
    // sink would deadlock.
    if (have_timing && this->on_frames_played) {
        this->on_frames_played(static_cast<uint32_t>(frames_done), finish_us);
    }

    return frames_done * bytes_per_frame;
}

void AlsaAudioSink::clear() {
    const std::lock_guard<std::mutex> lock(this->device_mutex_);

    // Snapped before the early return, so a flush with no device open still leaves the gain where
    // the next stream should start. Any ramp in progress was heading for audio that is about to be
    // dropped, so there is nothing left for it to be heard across.
    //
    // Safe here only because clear() holds device_mutex_, which write() also holds -- this sink
    // serialises everything through it. PortAudioSink::clear() deliberately does *not* do this: its
    // callback keeps running through a flush, so the same write would be a data race there. The
    // discriminator is the threading model, not the audio: do not harmonise the two.
    this->current_multiplier_ = this->target_multiplier_.load(std::memory_order_relaxed);

    if (this->pcm_ == nullptr) {
        return;
    }

    // Drop what is queued and make the device ready for the next stream. The format and
    // the open handle deliberately survive: clear() is a flush, not a close, and the frame
    // size has to stay valid for writes that follow. Releasing the device is stop()'s job.
    snd_pcm_drop(this->pcm_);
    const int err = snd_pcm_prepare(this->pcm_);
    if (err < 0) {
        cli_log(LogLevel::WARN, "alsa: could not re-prepare '%s' after clear: %s",
                this->device_.c_str(), snd_strerror(err));
    }
}

void AlsaAudioSink::stop() {
    // Set before taking the lock so a write() already blocked on the device sees it and
    // bails out, instead of making shutdown wait for the ring to drain.
    this->stopping_.store(true);

    const std::lock_guard<std::mutex> lock(this->device_mutex_);
    if (this->pcm_ == nullptr) {
        return;
    }
    // drop(), not drain(): shutdown should be prompt, and draining would block for the
    // ring's worth of audio -- or indefinitely if the device has wedged.
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

void AlsaAudioSink::update_target_multiplier_() {
    // Only the target moves. current_multiplier_ is left for write() to walk toward it, on the
    // thread that owns it -- which is what keeps this setter lock-free.
    this->target_multiplier_.store(q32_gain_for(this->volume_.load(), this->muted_.load()),
                                   std::memory_order_relaxed);
}

}  // namespace sendspin_cli
