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

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_PLAYER;

PlayerListener::PlayerListener(sendspin::PlayerRole& player, AudioSink& sink, StateStore* store)
    : player_(player), sink_(sink), store_(store) {
    // Capture the address, not the reference parameter, which dies with this frame.
    this->sink_.on_frames_played = [target = &player](uint32_t frames, int64_t timestamp) {
        target->notify_audio_played(frames, timestamp);
    };
}

size_t PlayerListener::on_audio_write(uint8_t* data, size_t length, uint32_t timeout_ms) {
    const size_t written = this->sink_.write(data, length, timeout_ms);
    if (this->stream_refused_.load(std::memory_order_relaxed)) {
        this->refused_bytes_.fetch_add(written, std::memory_order_relaxed);
    }
    return written;
}

void PlayerListener::on_stream_start() {
    // Cleared here, not at stream end: the early return below never reaches one.
    this->stream_refused_.store(false, std::memory_order_relaxed);
    this->refused_bytes_.store(0, std::memory_order_relaxed);
    // Before the guards, so an incomplete or refused stream still reports as streaming.
    this->streaming_ = true;
    this->stream_format_.reset();

    // Fire the start hook before any guard can return, so it always matches `streaming`.
    if (this->on_stream_event) {
        this->on_stream_event(true);
    }

    const sendspin::ServerPlayerStreamObject& params = this->player_.get_current_stream_params();
    if (!params.is_complete()) {
        cli_log(LogLevel::WARN,
                "Stream started without complete parameters -- sink not configured");
        return;
    }

    cli_log(LogLevel::INFO, "Stream started");
    if (!this->sink_.configure(*params.sample_rate, *params.channels, *params.bit_depth)) {
        cli_log(LogLevel::ERROR,
                "Output device '%s' refused %u Hz / %u ch / %u-bit -- this stream's audio will be "
                "discarded. Run with -l to see what the device accepts.",
                this->sink_.name().c_str(), *params.sample_rate, *params.channels,
                *params.bit_depth);
        this->stream_refused_.store(true, std::memory_order_relaxed);
        return;
    }
    this->stream_format_ = StreamFormat{*params.sample_rate, *params.channels, *params.bit_depth};
}

void PlayerListener::on_stream_end() {
    // Clear the flag before taking the counter, so a racing write cannot add to a spent total.
    const bool refused = this->stream_refused_.exchange(false, std::memory_order_relaxed);
    const uint64_t discarded = this->refused_bytes_.exchange(0, std::memory_order_relaxed);
    this->streaming_ = false;
    this->stream_format_.reset();
    if (refused) {
        cli_log(LogLevel::ERROR,
                "Stream ended having played nothing: %llu bytes discarded, because '%s' refused "
                "its format",
                static_cast<unsigned long long>(discarded), this->sink_.name().c_str());
    } else {
        cli_log(LogLevel::INFO, "Stream ended");
    }
    this->sink_.clear();
    if (this->on_stream_event) {
        this->on_stream_event(false);
    }
}

void PlayerListener::on_volume_changed(uint8_t volume) {
    this->sink_.set_volume(volume);
    this->applied_volume_ = volume;
    this->volume_source_ = VolumeSource::Server;
    this->persist_volume();
}

void PlayerListener::on_mute_changed(bool muted) {
    this->sink_.set_muted(muted);
    this->applied_muted_ = muted;
    this->volume_source_ = VolumeSource::Server;
    this->persist_volume();
}

void PlayerListener::on_static_delay_changed(uint16_t delay_ms) {
    cli_log(LogLevel::INFO, "Static delay set to %u ms by the server", delay_ms);
}

void PlayerListener::restore_volume(uint8_t volume, bool muted) {
    this->sink_.set_volume(volume);
    this->sink_.set_muted(muted);
    this->applied_volume_ = volume;
    this->applied_muted_ = muted;
    this->volume_source_ = VolumeSource::Restored;
}

void PlayerListener::persist_volume() const {
    if (this->store_ == nullptr) {
        return;
    }
    this->store_->set_volume_and_muted(this->applied_volume_, this->applied_muted_);
}

}  // namespace sendspin_cli
