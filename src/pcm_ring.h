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

/// Lock-free single-producer/single-consumer byte ring, for the pull-model backends.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sendspin_cli {

/// Lock-free SPSC byte ring bridging write()'s push to a backend callback's pull.
/// The producer owns write_pos_ and the consumer read_pos_; nothing else may write either.
class PcmRingBuffer {
public:
    /// Writes up to `len` bytes. Producer side.
    /// @return Bytes written, short of `len` when the ring is nearly full.
    size_t write(const uint8_t* data, size_t len);

    /// Reads up to `len` bytes, zero-filling any shortfall. Consumer side.
    /// @return Bytes of real audio read, not counting the padding.
    size_t read(uint8_t* dest, size_t len);

    /// Bytes available to read.
    size_t available() const;

    /// Bytes that can be written before the ring is full.
    size_t free_space() const;

    /// Asks the consumer to drop everything on its next read(); safe while the callback runs.
    void request_clear();

    /// Drops everything now. Only with no reader running and the producer's mutex held.
    void drop();

    /// Resizes the ring and drops everything in it. Same restriction as drop().
    void reset(size_t capacity);

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_{0};
    std::atomic<size_t> write_pos_{0};
    std::atomic<size_t> read_pos_{0};
    std::atomic<bool> clear_requested_{false};
};

}  // namespace sendspin_cli
