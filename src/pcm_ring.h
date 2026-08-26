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

/// @file pcm_ring.h
/// @brief Lock-free single-producer/single-consumer byte ring, for the pull-model backends

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sendspin_cli {

/// @brief Lock-free single-producer/single-consumer byte ring buffer.
///
/// Bridges AudioSink::write()'s push model to a backend that *pulls*: the sync task writes,
/// the backend's audio callback reads, and neither waits on the other. The ring itself needs
/// no lock and allocates nothing outside reset(); what a sink does around it -- notifying the
/// producer's condition variable, and invoking on_frames_played -- is not strictly
/// realtime-safe, and is the same pragmatic trade upstream's reference makes.
///
/// The producer owns write_pos_ and the consumer owns read_pos_. That each index has exactly
/// one writer is the invariant the whole class rests on, and it is why request_clear() only
/// *asks* for a drain that the consumer performs on its next read() -- resetting read_pos_
/// from the producer side would break it.
///
/// Lifted from upstream's PortAudioSink (examples/common/portaudio_sink.cpp), so the two
/// implementations buffer alike. It lives here rather than in one backend's header because
/// PortAudio and PipeWire both pull, and a second copy of a lock-free ring is the kind of
/// duplication that drifts silently. Device-free and clock-free, so it is compiled and tested
/// on a host with no audio backend at all -- the same split src/sink_recovery.{h,cpp} and
/// src/pcm_volume.{h,cpp} make.
class PcmRingBuffer {
public:
    /// @brief Writes up to `len` bytes. Producer side.
    /// @return Bytes actually written, which is short of `len` when the ring is nearly full.
    size_t write(const uint8_t* data, size_t len);

    /// @brief Reads up to `len` bytes, zero-filling any shortfall. Consumer side.
    ///
    /// Zeroed bytes are silence for the signed PCM the player emits, so a starved callback
    /// outputs a gap rather than whatever the device buffer last held.
    /// @return Bytes of real audio read, not counting the silence padding.
    size_t read(uint8_t* dest, size_t len);

    /// @brief Bytes available to read.
    size_t available() const;

    /// @brief Bytes that can be written before the ring is full.
    size_t free_space() const;

    /// @brief Asks the consumer to drop everything buffered, on its next read().
    ///
    /// Safe to call while the callback is running, and the only clearing operation that is.
    void request_clear();

    /// @brief Drops everything buffered, here and now.
    ///
    /// Writes both positions, so it is only safe with no reader running -- after the backend's
    /// stream has been stopped, with the producer's mutex held.
    void drop();

    /// @brief Resizes the ring and drops everything in it. Same restriction as drop().
    void reset(size_t capacity);

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_{0};
    std::atomic<size_t> write_pos_{0};
    std::atomic<size_t> read_pos_{0};
    std::atomic<bool> clear_requested_{false};
};

}  // namespace sendspin_cli
