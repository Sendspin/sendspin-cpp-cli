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
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace sendspin_cli {

using sendspin::LogLevel;

namespace {

/// Target ring size and granularity handed to ALSA. Small enough that snd_pcm_delay()
/// stays a useful sync signal, large enough to ride out scheduling jitter. Making these
/// tunable is squeezelite's -a flag, which this task leaves to a follow-up.
constexpr unsigned int BUFFER_TIME_US = 100000;  // 100 ms of ring
constexpr unsigned int PERIOD_TIME_US = 20000;   // 20 ms per wakeup

/// Longest single snd_pcm_wait() slice. write() re-checks the abort flag and the caller's
/// deadline between slices, so this bounds how long a shutdown waits on a busy device.
constexpr int WAIT_SLICE_MS = 20;

/// How many times to retry snd_pcm_resume() before falling back to prepare(), and how
/// long to pause between tries. Only reached when the host resumes from suspend.
constexpr int RESUME_TRIES = 10;
constexpr auto RESUME_PAUSE = std::chrono::milliseconds(10);

/// The rates -l asks each PCM about. A generous ladder costs nothing: the one
/// snd_pcm_open() is the expensive part, and every test below it runs against the
/// already-fetched configuration space in memory.
constexpr unsigned int PROBE_RATES[] = {22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};

/// The channel counts -l asks about: mono through 7.1.
constexpr unsigned int PROBE_CHANNELS[] = {1, 2, 4, 6, 8};

/// The formats -l asks about -- exactly the four alsa_format_for() can emit.
///
/// Reporting anything wider would advertise a capability sendspin-cli can never use: the
/// player hands us tightly packed little-endian PCM at one of these four depths, so a card
/// that also takes, say, S24_LE or a big-endian format cannot be reached through it.
struct ProbeFormat {
    snd_pcm_format_t format;
    const char* name;
};
constexpr ProbeFormat PROBE_FORMATS[] = {
    {SND_PCM_FORMAT_S8, "S8"},
    {SND_PCM_FORMAT_S16_LE, "S16_LE"},
    {SND_PCM_FORMAT_S24_3LE, "S24_3LE"},
    {SND_PCM_FORMAT_S32_LE, "S32_LE"},
};

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
/// 24-bit is S24_3LE (three packed bytes), not S24_LE (three bytes padded into four),
/// because the player hands us tightly packed samples.
bool alsa_format_for(uint8_t bits_per_sample, snd_pcm_format_t& format) {
    switch (bits_per_sample) {
        case 8:
            format = SND_PCM_FORMAT_S8;
            return true;
        case 16:
            format = SND_PCM_FORMAT_S16_LE;
            return true;
        case 24:
            format = SND_PCM_FORMAT_S24_3LE;
            return true;
        case 32:
            format = SND_PCM_FORMAT_S32_LE;
            return true;
        default:
            return false;
    }
}

/// Appends `value` to a space-separated list.
void append_item(std::string& list, const std::string& value) {
    if (!list.empty()) {
        list += ' ';
    }
    list += value;
}

/// Prints what one PCM will actually take, indented under its name in -l.
///
/// Degrades, never fails: a busy or unopenable device gets a one-line note and the caller
/// moves on to the next hint, because one card nobody can open must not cost the rest of
/// the list.
///
/// The cost here is the single snd_pcm_open(). Everything after it queries a configuration
/// space already in memory, so testing eight rates and four formats is no dearer than
/// testing one.
void print_device_capabilities(std::FILE* out, const char* name) {
    snd_pcm_t* pcm = nullptr;
    // NONBLOCK for probe()'s reason: a card another process holds exclusively must come
    // back rather than park -l on it.
    const int err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err == -EBUSY) {
        std::fprintf(out, "      (in use -- capabilities unknown)\n");
        return;
    }
    if (err < 0) {
        std::fprintf(out, "      (cannot open: %s)\n", snd_strerror(err));
        return;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    // Narrowed to the access mode the sink actually uses before anything is tested, so the
    // report describes how sendspin-cli would drive the card, not what it can do in modes
    // this player never asks for.
    if (snd_pcm_hw_params_any(pcm, hw) < 0 ||
        snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        std::fprintf(out, "      (no interleaved playback configuration)\n");
        snd_pcm_close(pcm);
        return;
    }

    std::string rates;
    for (const unsigned int rate : PROBE_RATES) {
        if (snd_pcm_hw_params_test_rate(pcm, hw, rate, 0) == 0) {
            append_item(rates, std::to_string(rate));
        }
    }

    std::string formats;
    for (const ProbeFormat& candidate : PROBE_FORMATS) {
        if (snd_pcm_hw_params_test_format(pcm, hw, candidate.format) == 0) {
            append_item(formats, candidate.name);
        }
    }

    std::string channels;
    for (const unsigned int count : PROBE_CHANNELS) {
        if (snd_pcm_hw_params_test_channels(pcm, hw, count) == 0) {
            append_item(channels, std::to_string(count));
        }
    }

    snd_pcm_close(pcm);

    // An empty list is meaningful: the device opens but takes nothing this player emits.
    std::fprintf(out, "      rates:    %s\n",
                 rates.empty() ? "(none of the probed rates)" : rates.c_str());
    std::fprintf(out, "      formats:  %s\n",
                 formats.empty() ? "(none sendspin-cli can emit)" : formats.c_str());
    std::fprintf(out, "      channels: %s\n",
                 channels.empty() ? "(none of the probed counts)" : channels.c_str());
}

}  // namespace

AlsaAudioSink::AlsaAudioSink(std::string device) : device_(std::move(device)) {}

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
        cli_log(LogLevel::ERROR, "alsa sink: unsupported bit depth %u", bits_per_sample);
        return false;
    }
    if (channels == 0 || sample_rate == 0) {
        cli_log(LogLevel::ERROR, "alsa sink: refusing stream with %u ch at %u Hz", channels,
                sample_rate);
        return false;
    }

    int err = snd_pcm_open(&this->pcm_, this->device_.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa sink: cannot open '%s': %s", this->device_.c_str(),
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
    if (err >= 0) {
        step = "buffer time";
        unsigned int buffer_time = BUFFER_TIME_US;
        err = snd_pcm_hw_params_set_buffer_time_near(this->pcm_, hw, &buffer_time, nullptr);
    }
    if (err >= 0) {
        step = "period time";
        unsigned int period_time = PERIOD_TIME_US;
        err = snd_pcm_hw_params_set_period_time_near(this->pcm_, hw, &period_time, nullptr);
    }
    if (err >= 0) {
        step = "hardware parameters";
        err = snd_pcm_hw_params(this->pcm_, hw);
    }
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa sink: '%s' rejected %s for %u Hz / %u ch / %u-bit: %s",
                this->device_.c_str(), step, sample_rate, channels, bits_per_sample,
                snd_strerror(err));
        this->close_device_();
        return false;
    }

    snd_pcm_uframes_t buffer_size = 0;
    snd_pcm_uframes_t period_size = 0;
    snd_pcm_hw_params_get_buffer_size(hw, &buffer_size);
    snd_pcm_hw_params_get_period_size(hw, &period_size, nullptr);

    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    err = snd_pcm_sw_params_current(this->pcm_, sw);
    if (err >= 0) {
        // Start only once the ring is full: the extra cushion costs a little startup
        // latency and buys a lot of underrun immunity, and the server compensates for the
        // delay through the timestamps write() feeds back.
        err = snd_pcm_sw_params_set_start_threshold(this->pcm_, sw, buffer_size);
    }
    if (err >= 0) {
        err = snd_pcm_sw_params_set_avail_min(this->pcm_, sw, period_size);
    }
    if (err >= 0) {
        err = snd_pcm_sw_params(this->pcm_, sw);
    }
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa sink: '%s' rejected software parameters: %s",
                this->device_.c_str(), snd_strerror(err));
        this->close_device_();
        return false;
    }

    err = snd_pcm_prepare(this->pcm_);
    if (err < 0) {
        cli_log(LogLevel::ERROR, "alsa sink: cannot prepare '%s': %s", this->device_.c_str(),
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
            "alsa sink: '%s' open at %u Hz, %u ch, %u-bit (%zu bytes/frame, %lu-frame ring, "
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
            cli_log(LogLevel::ERROR, "alsa sink: underrun recovery failed: %s",
                    snd_strerror(prepared));
            return false;
        }
        cli_log(LogLevel::DEBUG, "alsa sink: underrun recovered");
        return true;
    }

    if (err == -ESTRPIPE) {
        // The device was suspended (system sleep). Resume is what keeps the stream's
        // position; prepare is the fallback that restarts it if resume is unsupported.
        for (int i = 0; i < RESUME_TRIES; ++i) {
            const int resumed = snd_pcm_resume(this->pcm_);
            if (resumed != -EAGAIN) {
                if (resumed >= 0) {
                    cli_log(LogLevel::INFO, "alsa sink: resumed after suspend");
                    return true;
                }
                break;
            }
            std::this_thread::sleep_for(RESUME_PAUSE);
        }
        const int prepared = snd_pcm_prepare(this->pcm_);
        if (prepared < 0) {
            cli_log(LogLevel::ERROR, "alsa sink: suspend recovery failed: %s",
                    snd_strerror(prepared));
            return false;
        }
        cli_log(LogLevel::INFO, "alsa sink: restarted after suspend");
        return true;
    }

    cli_log(LogLevel::ERROR, "alsa sink: %s", snd_strerror(err));
    return false;
}

bool AlsaAudioSink::configure(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample) {
    const std::lock_guard<std::mutex> lock(this->device_mutex_);

    if (this->pcm_ != nullptr && this->rate_ == sample_rate && this->channels_ == channels &&
        this->bits_ == bits_per_sample) {
        // Same format as the stream that just ended: keep the device and just start from a
        // clean ring, which is cheaper than a close/open round trip and avoids handing an
        // exclusive device back to a competitor mid-track.
        snd_pcm_drop(this->pcm_);
        const int err = snd_pcm_prepare(this->pcm_);
        if (err < 0) {
            cli_log(LogLevel::WARN, "alsa sink: could not restart '%s' (%s) -- reopening",
                    this->device_.c_str(), snd_strerror(err));
            this->close_device_();
            return this->open_device_(sample_rate, channels, bits_per_sample);
        }
        cli_log(LogLevel::DEBUG, "alsa sink: reusing '%s' at %u Hz, %u ch, %u-bit",
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
                        "alsa sink: '%s' is not open -- discarding audio until a stream "
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

        // Volume: scale into scratch so the caller's buffer stays untouched. Unity gain
        // skips the copy entirely, which is the steady-state case.
        const uint8_t* src = data;
        const uint64_t gain = this->volume_multiplier_.load(std::memory_order_relaxed);
        if (gain < Q32_ONE) {
            this->scaled_.resize(usable);
            if (gain == 0) {
                // Silence. Zero bytes are silence for the signed PCM the player advertises.
                std::memset(this->scaled_.data(), 0, usable);
            } else {
                std::memcpy(this->scaled_.data(), data, usable);
                apply_volume(this->scaled_.data(), usable, this->bits_ / 8U, gain);
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
    if (this->pcm_ == nullptr) {
        return;
    }

    // Drop what is queued and make the device ready for the next stream. The format and
    // the open handle deliberately survive: clear() is a flush, not a close, and the frame
    // size has to stay valid for writes that follow. Releasing the device is stop()'s job.
    snd_pcm_drop(this->pcm_);
    const int err = snd_pcm_prepare(this->pcm_);
    if (err < 0) {
        cli_log(LogLevel::WARN, "alsa sink: could not re-prepare '%s' after clear: %s",
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
    cli_log(LogLevel::INFO, "alsa sink: '%s' closed", this->device_.c_str());
}

void AlsaAudioSink::set_volume(uint8_t volume) {
    this->volume_.store(volume > 100 ? 100 : volume);
    this->update_volume_multiplier_();
    cli_log(LogLevel::DEBUG, "alsa sink: volume now %u", this->volume_.load());
}

void AlsaAudioSink::set_muted(bool muted) {
    this->muted_.store(muted);
    this->update_volume_multiplier_();
    cli_log(LogLevel::DEBUG, "alsa sink: %s", muted ? "muted" : "unmuted");
}

void AlsaAudioSink::update_volume_multiplier_() {
    this->volume_multiplier_.store(q32_gain_for(this->volume_.load(), this->muted_.load()),
                                   std::memory_order_relaxed);
}

}  // namespace sendspin_cli
