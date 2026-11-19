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

#include "player_listener.h"

#include "log.h"

#include <sendspin/client.h>

#include <algorithm>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_PLAYER;

namespace {

/// How long poll() waits for the old format to play out before reopening anyway.
constexpr int64_t PLAYOUT_TIMEOUT_US = 5'000'000;

constexpr int64_t US_PER_S = 1'000'000;

size_t bytes_per_frame(const sendspin::AudioFormat& format) {
    return static_cast<size_t>(format.channels) *
           (static_cast<size_t>(format.bits_per_sample) / 8U);
}

}  // namespace

PlayerListener::PlayerListener(sendspin::PlayerRole& player, AudioSink& sink, StateStore* store)
    : player_(player), sink_(sink), store_(store) {
    // Capture the address, not the reference parameter, which dies with this frame.
    this->sink_.on_frames_played = [this, target = &player](uint32_t frames, int64_t timestamp) {
        this->played_frames_.fetch_add(frames, std::memory_order_relaxed);
        int64_t end = this->playout_end_us_.load(std::memory_order_relaxed);
        while (timestamp > end && !this->playout_end_us_.compare_exchange_weak(
                                      end, timestamp, std::memory_order_relaxed)) {}
        target->notify_audio_played(frames, timestamp);
    };
}

size_t PlayerListener::on_audio_output(std::span<const uint8_t> pcm,
                                       const sendspin::AudioFormat& format, uint32_t timeout_ms) {
    bool refused = false;
    {
        const std::lock_guard<std::mutex> lock(this->format_mutex_);
        if (!this->open_.has_value() || this->open_->format != format) {
            this->requested_ = format;
            return 0;
        }
        refused = this->open_->refused;
    }

    const size_t frame = bytes_per_frame(format);
    if (refused) {
        // Discarded, but reported played on a virtual clock so sync neither stalls nor races.
        if (frame == 0 || format.sample_rate == 0) {
            return pcm.size();
        }
        const size_t frames = pcm.size() / frame;
        this->refused_playhead_us_ =
            std::max(this->refused_playhead_us_, sendspin::SendspinClient::now_us()) +
            (static_cast<int64_t>(frames) * US_PER_S) / static_cast<int64_t>(format.sample_rate);
        if (frames > 0) {
            this->player_.notify_audio_played(static_cast<uint32_t>(frames),
                                              this->refused_playhead_us_);
        }
        this->refused_bytes_.fetch_add(frames * frame, std::memory_order_relaxed);
        return frames * frame;
    }

    const size_t written = this->sink_.write(pcm.data(), pcm.size(), timeout_ms);
    if (frame != 0) {
        this->written_frames_.fetch_add(written / frame, std::memory_order_relaxed);
    }
    return written;
}

void PlayerListener::poll(int64_t now_us) {
    sendspin::AudioFormat wanted;
    bool holds_audio = false;
    {
        const std::lock_guard<std::mutex> lock(this->format_mutex_);
        if (!this->requested_.has_value()) {
            return;
        }
        wanted = *this->requested_;
        holds_audio = this->open_.has_value() && !this->open_->refused;
    }

    if (holds_audio) {
        if (!this->requested_since_us_.has_value()) {
            this->requested_since_us_ = now_us;
        }
        const bool drained = this->played_frames_.load(std::memory_order_relaxed) >=
                                 this->written_frames_.load(std::memory_order_relaxed) &&
                             now_us >= this->playout_end_us_.load(std::memory_order_relaxed);
        if (!drained) {
            if (now_us - *this->requested_since_us_ < PLAYOUT_TIMEOUT_US) {
                return;
            }
            cli_log(LogLevel::WARN,
                    "'%s' has not played out the previous format's audio -- reopening anyway",
                    this->sink_.name().c_str());
        }
    }
    this->open_sink(wanted);
}

void PlayerListener::open_sink(const sendspin::AudioFormat& format) {
    // Nothing of the old format is left to report, so the counts start over.
    this->written_frames_.store(0, std::memory_order_relaxed);
    this->played_frames_.store(0, std::memory_order_relaxed);
    this->requested_since_us_.reset();

    const bool accepted =
        this->sink_.configure(format.sample_rate, format.channels, format.bits_per_sample);
    if (accepted) {
        this->stream_format_ =
            StreamFormat{format.sample_rate, format.channels, format.bits_per_sample};
    } else {
        cli_log(LogLevel::ERROR,
                "Output device '%s' refused %u Hz / %u ch / %u-bit -- audio in this format will "
                "be discarded. Run with -l to see what the device accepts.",
                this->sink_.name().c_str(), format.sample_rate, format.channels,
                format.bits_per_sample);
        this->stream_format_.reset();
    }

    const std::lock_guard<std::mutex> lock(this->format_mutex_);
    this->open_ = OpenFormat{format, !accepted};
    this->requested_.reset();
}

void PlayerListener::reset_format() {
    {
        const std::lock_guard<std::mutex> lock(this->format_mutex_);
        this->open_.reset();
        this->requested_.reset();
    }
    this->requested_since_us_.reset();
    this->stream_format_.reset();
    this->written_frames_.store(0, std::memory_order_relaxed);
    this->played_frames_.store(0, std::memory_order_relaxed);
    // The sink is cleared next, so its last finish time no longer means anything.
    this->playout_end_us_.store(0, std::memory_order_relaxed);
}

void PlayerListener::on_stream_start() {
    // Also fired for a stream/start that updates the running stream; its writes reopen the sink.
    const bool fresh = !this->streaming_;
    this->streaming_ = true;

    // Fire the start hook before any guard can return, so it always matches `streaming`.
    if (this->on_stream_event) {
        this->on_stream_event(true);
    }

    const std::optional<sendspin::PlayerStreamInfo> info = this->player_.stream_info();
    if (!fresh) {
        if (info.has_value()) {
            cli_log(LogLevel::INFO, "Stream updated: %u Hz / %u ch / %u-bit", info->sample_rate,
                    info->channels, info->bit_depth);
        }
        return;
    }

    cli_log(LogLevel::INFO, "Stream started");
    this->refused_bytes_.store(0, std::memory_order_relaxed);
    if (!info.has_value()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(this->format_mutex_);
        if (this->open_.has_value()) {
            return;
        }
    }
    // Opened ahead of the first write to save its latency; writes are rounded to whole bytes.
    const auto container_bits = static_cast<uint8_t>((info->bit_depth + 7U) / 8U * 8U);
    this->open_sink(sendspin::AudioFormat{info->sample_rate, info->channels, container_bits});
}

void PlayerListener::on_stream_end() {
    // Also fires when the role is removed with no stream running; that is not a stop.
    if (!this->streaming_) {
        return;
    }
    const uint64_t discarded = this->refused_bytes_.exchange(0, std::memory_order_relaxed);
    this->streaming_ = false;
    this->reset_format();
    if (discarded > 0) {
        cli_log(LogLevel::ERROR,
                "Stream ended: %llu bytes discarded, because '%s' refused their format",
                static_cast<unsigned long long>(discarded), this->sink_.name().c_str());
    } else {
        cli_log(LogLevel::INFO, "Stream ended");
    }
    // Before the stop hook, so a hook that starts another player finds the device free.
    if (this->release_device_) {
        this->sink_.release();
    } else {
        this->sink_.clear();
    }
    if (this->on_stream_event) {
        this->on_stream_event(false);
    }
}

void PlayerListener::on_volume_changed(uint8_t volume) {
    // A command that slips through while fixed must not reach the sink or the state file.
    if (this->volume_source_ == VolumeSource::Fixed) {
        return;
    }
    this->sink_.set_volume(volume);
    this->applied_volume_ = volume;
    this->volume_source_ = VolumeSource::Server;
    this->persist_volume();
}

void PlayerListener::on_mute_changed(bool muted) {
    if (this->volume_source_ == VolumeSource::Fixed) {
        return;
    }
    this->sink_.set_muted(muted);
    this->applied_muted_ = muted;
    this->volume_source_ = VolumeSource::Server;
    this->persist_volume();
}

void PlayerListener::on_output_delay_changed(uint16_t delay_ms) {
    cli_log(LogLevel::INFO, "Output delay set to %u ms by the server", delay_ms);
}

void PlayerListener::restore_volume(uint8_t volume, bool muted) {
    this->sink_.set_volume(volume);
    this->sink_.set_muted(muted);
    this->applied_volume_ = volume;
    this->applied_muted_ = muted;
    this->volume_source_ = VolumeSource::Restored;
}

void PlayerListener::seed_volume(uint8_t volume, bool muted) {
    // The sink already holds this level; setting it again would write a rounded value back.
    this->applied_volume_ = volume;
    this->applied_muted_ = muted;
    this->volume_source_ = VolumeSource::Hardware;
}

void PlayerListener::fix_volume() {
    this->sink_.set_volume(DEFAULT_SINK_VOLUME);
    this->sink_.set_muted(false);
    this->applied_volume_ = DEFAULT_SINK_VOLUME;
    this->applied_muted_ = false;
    this->volume_source_ = VolumeSource::Fixed;
}

void PlayerListener::on_sink_volume_changed(uint8_t volume, bool muted) {
    // A local change while fixed must not reach the state file or the server's view.
    if (this->volume_source_ == VolumeSource::Fixed) {
        return;
    }
    this->applied_volume_ = volume;
    this->applied_muted_ = muted;
    this->volume_source_ = VolumeSource::Hardware;
    // Through the role, not the sink, which already applied it: this sends client/state.
    this->player_.update_volume(volume);
    this->player_.update_muted(muted);
    this->persist_volume();
}

void PlayerListener::persist_volume() const {
    if (this->store_ == nullptr) {
        return;
    }
    this->store_->set_volume_and_muted(this->applied_volume_, this->applied_muted_);
}

}  // namespace sendspin_cli
