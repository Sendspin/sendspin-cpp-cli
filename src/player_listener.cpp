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
    // Capture the address by value: capturing the `player` reference parameter itself
    // would leave the callback holding a reference into this constructor's frame.
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
    // Cleared here rather than at stream end, because the early return below never reaches
    // one: a stream that arrives without parameters would otherwise inherit the last
    // stream's refusal and report its audio as discarded.
    this->stream_refused_.store(false, std::memory_order_relaxed);
    this->refused_bytes_.store(0, std::memory_order_relaxed);
    // Above every guard below, so a stream that arrives with incomplete parameters -- or one the
    // device then refuses -- still reports as a stream. Audio is arriving either way, and that is
    // the whole of what this flag says.
    this->streaming_ = true;
    this->stream_format_.reset();

    const sendspin::ServerPlayerStreamObject& params = this->player_.get_current_stream_params();
    if (!params.is_complete()) {
        // The sink cannot open a device without a format. The player keeps the stream
        // alive, so a later start with complete params still works.
        cli_log(LogLevel::WARN,
                "Stream started without complete parameters -- sink not configured");
        return;
    }

    cli_log(LogLevel::INFO, "Stream started");
    if (!this->sink_.configure(*params.sample_rate, *params.channels, *params.bit_depth)) {
        // Names the device as well as the format, and says what happens next: the sink keeps
        // accepting writes and drops them, so a player that looks healthy plays nothing.
        cli_log(LogLevel::ERROR,
                "Output device '%s' refused %u Hz / %u ch / %u-bit -- this stream's audio will be "
                "discarded. Run with -l to see what the device accepts.",
                this->sink_.name().c_str(), *params.sample_rate, *params.channels,
                *params.bit_depth);
        this->stream_refused_.store(true, std::memory_order_relaxed);
        return;
    }
    // Recorded only on a format the device accepted, so `status` describes what is really
    // being played rather than what was asked for and refused.
    this->stream_format_ =
        StreamFormat{*params.sample_rate, *params.channels, *params.bit_depth};
}

void PlayerListener::on_stream_end() {
    // Said again at the end, so a whole track lost to one refusal is visible in the log even
    // where the ERROR above has scrolled away -- and so "nothing came out" has a cause next
    // to it rather than a quiet gap.
    // Flag first, counter second: on_audio_write() reads the flag before adding, so clearing
    // it first is what stops a write racing this from adding to a total already taken.
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
}

void PlayerListener::on_volume_changed(uint8_t volume) {
    this->sink_.set_volume(volume);
    // Recorded as well as forwarded, so `status` can report the gain the sink is applying rather
    // than the one PlayerRole stores -- which is 0 until this callback has fired at least once.
    this->applied_volume_ = volume;
    this->volume_source_ = VolumeSource::Server;
    this->persist_volume();
}

void PlayerListener::on_mute_changed(bool muted) {
    this->sink_.set_muted(muted);
    this->applied_muted_ = muted;
    // A mute is a volume decision a server made too, so it moves the source the same way. Without
    // that, a player a server had only ever muted would report its gain as one nobody chose.
    this->volume_source_ = VolumeSource::Server;
    this->persist_volume();
}

void PlayerListener::on_static_delay_changed(uint16_t delay_ms) {
    // INFO rather than DEBUG: it changes when audio comes out of this speaker relative to every
    // other player in the group, which is exactly the kind of thing someone chasing a sync problem
    // needs to find in a log.
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
    // One call, so the pair reaches the disk in one write. A server can send volume and mute in the
    // same command, which fires both callbacks above inside one drain -- two writes would let a kill
    // land between them and persist a pair that was never true.
    this->store_->set_volume_and_muted(this->applied_volume_, this->applied_muted_);
}

}  // namespace sendspin_cli
