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

#include "pcm_ring.h"

#include <algorithm>
#include <cstring>

namespace sendspin_cli {

size_t PcmRingBuffer::write(const uint8_t* data, size_t len) {
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

size_t PcmRingBuffer::read(uint8_t* dest, size_t len) {
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

size_t PcmRingBuffer::available() const {
    if (this->capacity_ == 0) {
        return 0;
    }
    const size_t write_pos = this->write_pos_.load(std::memory_order_acquire);
    const size_t read_pos = this->read_pos_.load(std::memory_order_acquire);
    return (write_pos - read_pos + this->capacity_) % this->capacity_;
}

size_t PcmRingBuffer::free_space() const {
    if (this->capacity_ == 0) {
        return 0;
    }
    return this->capacity_ - 1 - this->available();
}

void PcmRingBuffer::request_clear() {
    this->clear_requested_.store(true, std::memory_order_release);
}

void PcmRingBuffer::drop() {
    this->clear_requested_.store(false, std::memory_order_relaxed);
    this->read_pos_.store(0, std::memory_order_relaxed);
    this->write_pos_.store(0, std::memory_order_release);
}

void PcmRingBuffer::reset(size_t capacity) {
    this->buffer_.assign(capacity, 0);
    this->capacity_ = capacity;
    this->drop();
}

}  // namespace sendspin_cli
