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

#include "coreaudio_source.h"

#include "coreaudio_device.h"
#include "coreaudio_permission.h"
#include "log.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;

namespace {

/// Capture ring, in milliseconds; the ALSA source's size.
constexpr uint32_t RING_MS = 200;

/// Floor on the render buffer, for a unit that reports no slice size.
constexpr uint32_t MIN_RENDER_FRAMES = 4096;

constexpr int64_t US_PER_S = 1000000;

/// AUHAL's elements: 1 faces the device's input side, 0 its output side.
constexpr UInt32 INPUT_BUS = 1;
constexpr UInt32 OUTPUT_BUS = 0;

int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// The device's own rate as a whole number of Hz; 0 when it cannot be read.
uint32_t device_rate(AudioDeviceID device) {
    return static_cast<uint32_t>(std::llround(ca_nominal_sample_rate(device)));
}

/// An uninitialized AUHAL unit set to capture from `device`, or nullptr with `err` set.
AudioUnit new_input_unit(AudioDeviceID device, OSStatus& err) {
    AudioUnit unit = ca_new_hal_unit();
    if (unit == nullptr) {
        err = kAudioUnitErr_FailedInitialization;
        return nullptr;
    }
    // Before the device goes on: the unit checks the device against the sides it has enabled.
    const UInt32 on = 1;
    const UInt32 off = 0;
    err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input,
                               INPUT_BUS, &on, sizeof(on));
    if (err == noErr) {
        err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output,
                                   OUTPUT_BUS, &off, sizeof(off));
    }
    if (err == noErr) {
        err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global, 0, &device, sizeof(device));
    }
    if (err != noErr) {
        AudioComponentInstanceDispose(unit);
        return nullptr;
    }
    return unit;
}

/// Sets the format the unit hands captured audio over in.
OSStatus set_capture_format(AudioUnit unit, const AudioStreamBasicDescription& asbd) {
    return AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
                                INPUT_BUS, &asbd, sizeof(asbd));
}

}  // namespace

CoreAudioSource::CoreAudioSource(std::string device) : device_(std::move(device)) {}

CoreAudioSource::~CoreAudioSource() {
    this->close();
}

std::string CoreAudioSource::name() const {
    return this->device_.empty() ? "coreaudio" : "coreaudio:" + this->device_;
}

void CoreAudioSource::list_devices(std::FILE* out) {
    const std::vector<AudioDeviceID> inputs = ca_enumerate_devices(CaDirection::Input);
    if (inputs.empty()) {
        std::fprintf(out, "  (this host has no CoreAudio input devices)\n");
        return;
    }

    const AudioDeviceID fallback = ca_default_device(CaDirection::Input);
    std::fprintf(out, "  idx  name                                    in ch  current rate\n");
    for (size_t i = 0; i < inputs.size(); ++i) {
        const AudioDeviceID device = inputs[i];
        std::fprintf(out, "  %3zu  %-38s %2u ch  %6.0f Hz%s\n", i, ca_device_name(device).c_str(),
                     ca_channels(device, CaDirection::Input), ca_nominal_sample_rate(device),
                     (device == fallback) ? "  (system default)" : "");
    }
}

bool CoreAudioSource::negotiate(StreamFormat& format, std::string& error) {
    // First: without it the unit opens, starts and delivers silence.
    if (!request_microphone_access(error)) {
        return false;
    }

    AudioDeviceID device = kAudioObjectUnknown;
    if (!resolve_ca_device(this->device_, CaDirection::Input, device, error)) {
        return false;
    }
    const std::string label = "CoreAudio input device '" + ca_device_name(device) + "'";

    const uint32_t max_channels = ca_channels(device, CaDirection::Input);
    // The device's own rate: AUHAL converts depth and channels on capture, but does not resample.
    const uint32_t rate = device_rate(device);
    if (max_channels == 0 || rate == 0) {
        error = label + " went away before it could be asked what it captures -- run with -l to "
                        "list this host's input devices";
        return false;
    }
    const uint8_t wanted = (format.channels != 0) ? format.channels : uint8_t{2};
    const auto channels = static_cast<uint8_t>(std::min<uint32_t>(wanted, max_channels));

    OSStatus err = noErr;
    AudioUnit unit = new_input_unit(device, err);
    if (unit == nullptr) {
        error = "cannot open " + label + ": " + ca_status_text(err);
        return false;
    }

    std::vector<uint8_t> depths{format.bit_depth};
    depths.insert(depths.end(), CAPTURE_BIT_DEPTHS.begin(), CAPTURE_BIT_DEPTHS.end());
    uint8_t depth = 0;
    for (const uint8_t candidate : depths) {
        AudioStreamBasicDescription asbd = {};
        if (ca_asbd_for(rate, channels, candidate, asbd) &&
            set_capture_format(unit, asbd) == noErr) {
            depth = candidate;
            break;
        }
    }
    AudioComponentInstanceDispose(unit);

    if (depth == 0) {
        error = label + " captures nothing sendspin-cli can send (16, 24 or 32-bit PCM) -- run "
                        "with -l to list others";
        return false;
    }
    format = {rate, channels, depth};
    return true;
}

bool CoreAudioSource::open(const StreamFormat& format) {
    this->close();

    AudioDeviceID device = kAudioObjectUnknown;
    std::string error;
    if (!resolve_ca_device(this->device_, CaDirection::Input, device, error)) {
        cli_log(LogLevel::ERROR, "coreaudio: %s", error.c_str());
        return false;
    }
    const std::string label = ca_device_name(device);

    AudioStreamBasicDescription asbd = {};
    if (!ca_asbd_for(format.sample_rate, format.channels, format.bit_depth, asbd)) {
        cli_log(LogLevel::ERROR, "coreaudio: refusing capture at %u Hz / %u ch / %u-bit",
                format.sample_rate, format.channels, format.bit_depth);
        return false;
    }
    // Exact: the role announced this rate, and nothing here resamples.
    const uint32_t rate = device_rate(device);
    if (rate != format.sample_rate) {
        cli_log(LogLevel::ERROR,
                "coreaudio: input '%s' runs at %u Hz, not the %u Hz this run started with -- set "
                "it back in Audio MIDI Setup, or restart sendspin-cli",
                label.c_str(), rate, format.sample_rate);
        return false;
    }
    if (format.channels > ca_channels(device, CaDirection::Input)) {
        cli_log(LogLevel::ERROR, "coreaudio: input '%s' has %u channels, so it cannot capture %u",
                label.c_str(), ca_channels(device, CaDirection::Input), format.channels);
        return false;
    }

    OSStatus err = noErr;
    AudioUnit unit = new_input_unit(device, err);
    if (unit == nullptr) {
        cli_log(LogLevel::ERROR, "coreaudio: cannot open input '%s': %s", label.c_str(),
                ca_status_text(err).c_str());
        return false;
    }
    // Held from here on so every failure below goes out through close().
    this->unit_ = unit;

    err = set_capture_format(unit, asbd);
    if (err == noErr) {
        AURenderCallbackStruct callback = {};
        callback.inputProc = &CoreAudioSource::input_callback;
        callback.inputProcRefCon = this;
        err = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback,
                                   kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    }
    if (err == noErr) {
        err = AudioUnitInitialize(unit);
    }
    if (err != noErr) {
        cli_log(LogLevel::ERROR, "coreaudio: input '%s' rejected %u Hz / %u ch / %u-bit: %s",
                label.c_str(), format.sample_rate, format.channels, format.bit_depth,
                ca_status_text(err).c_str());
        this->close();
        return false;
    }

    // Everything the callback reads is set before the unit starts.
    this->device_id_ = device;
    this->format_ = format;
    this->bytes_per_frame_ =
        static_cast<size_t>(format.channels) * (static_cast<size_t>(format.bit_depth) / 8U);
    this->host_ticks_per_us_ = ca_host_ticks_per_us();
    this->frames_captured_ = 0;
    this->frames_read_ = 0;
    this->epoch_us_.store(0);
    this->frames_dropped_.store(0);

    const uint32_t latency_frames =
        ca_u32_property(device, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeInput) +
        ca_first_stream_latency(device, CaDirection::Input);
    this->input_latency_us_ = static_cast<int64_t>(latency_frames) * US_PER_S / rate;

    UInt32 slice_frames = 0;
    UInt32 slice_size = sizeof(slice_frames);
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                             0, &slice_frames, &slice_size) != noErr) {
        slice_frames = 0;
    }
    const size_t render_frames =
        std::max({slice_frames,
                  ca_u32_property(device, kAudioDevicePropertyBufferFrameSize,
                                  kAudioObjectPropertyScopeGlobal),
                  MIN_RENDER_FRAMES});
    this->render_buffer_.assign(render_frames * this->bytes_per_frame_, 0);
    const size_t ring_frames =
        std::max<size_t>(static_cast<size_t>(rate) * RING_MS / 1000U, render_frames * 2U);
    // The spare byte the ring keeps to tell full from empty.
    this->ring_.reset((ring_frames * this->bytes_per_frame_) + 1);

    // Cleared before the listeners go on, so a death between the two is not lost.
    this->device_lost_.store(false);
    this->default_moved_.store(false);
    this->add_listeners_(device);

    err = AudioOutputUnitStart(unit);
    if (err != noErr) {
        cli_log(LogLevel::ERROR, "coreaudio: input '%s' would not start: %s", label.c_str(),
                ca_status_text(err).c_str());
        this->close();
        return false;
    }

    cli_log(LogLevel::INFO, "coreaudio: input '%s' (%s) open at %u Hz, %u ch, %u-bit",
            label.c_str(), this->name().c_str(), format.sample_rate, format.channels,
            format.bit_depth);
    return true;
}

int CoreAudioSource::read(uint8_t* data, size_t length, uint32_t timeout_ms,
                          int64_t& capture_time_us) {
    if (this->unit_ == nullptr) {
        return -1;
    }

    if (this->default_moved_.exchange(false) && this->device_.empty()) {
        const AudioDeviceID moved_to = ca_default_device(CaDirection::Input);
        if (moved_to != kAudioObjectUnknown && moved_to != this->device_id_) {
            cli_log(LogLevel::INFO,
                    "coreaudio: the system default input moved from '%s' to '%s' -- following it",
                    ca_device_name(this->device_id_).c_str(), ca_device_name(moved_to).c_str());
            // A copy: open() closes first, and close() is free to forget format_.
            const StreamFormat format = this->format_;
            return this->open(format) ? 0 : -1;
        }
    }

    {
        std::unique_lock<std::mutex> lock(this->wait_mutex_);
        this->data_ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
            return this->ring_.available() >= this->bytes_per_frame_ || this->device_lost_.load();
        });
    }
    if (this->device_lost_.load()) {
        cli_log(LogLevel::ERROR, "coreaudio: input '%s' is gone, or changed its sample rate",
                this->name().c_str());
        return -1;
    }

    const uint64_t dropped = this->frames_dropped_.exchange(0);
    if (dropped != 0) {
        cli_log(LogLevel::DEBUG, "coreaudio: input overran, dropping %llu frames",
                static_cast<unsigned long long>(dropped));
    }

    size_t bytes = std::min(length, this->ring_.available());
    bytes -= bytes % this->bytes_per_frame_;
    if (bytes == 0) {
        return 0;
    }
    // Loaded before the read: the callback publishes it before the frames it covers.
    const int64_t epoch_us = this->epoch_us_.load();
    this->ring_.read(data, bytes);
    capture_time_us =
        epoch_us + static_cast<int64_t>(this->frames_read_) * US_PER_S / this->format_.sample_rate;
    this->frames_read_ += bytes / this->bytes_per_frame_;
    return static_cast<int>(bytes);
}

void CoreAudioSource::close() {
    this->remove_listeners_();
    if (this->unit_ == nullptr) {
        return;
    }
    AudioOutputUnitStop(this->unit_);
    // Uninitialize waits the input callback out, so nothing below races it.
    AudioUnitUninitialize(this->unit_);
    AudioComponentInstanceDispose(this->unit_);
    this->unit_ = nullptr;
    this->device_id_ = kAudioObjectUnknown;
    this->bytes_per_frame_ = 0;
    this->ring_.reset(0);
}

void CoreAudioSource::add_listeners_(AudioDeviceID device) {
    this->remove_listeners_();

    const AudioObjectPropertyAddress alive = ca_address_of(kAudioDevicePropertyDeviceIsAlive);
    if (AudioObjectAddPropertyListener(device, &alive, &CoreAudioSource::property_listener, this) ==
        noErr) {
        this->listening_alive_ = true;
    } else {
        cli_log(LogLevel::WARN,
                "coreaudio: input '%s' will not report its own death -- capture will not recover "
                "by itself if it goes away",
                ca_device_name(device).c_str());
    }
    const AudioObjectPropertyAddress rate = ca_address_of(kAudioDevicePropertyNominalSampleRate);
    if (AudioObjectAddPropertyListener(device, &rate, &CoreAudioSource::property_listener, this) ==
        noErr) {
        this->listening_rate_ = true;
    }
    this->listening_device_ = device;

    if (this->device_.empty()) {
        const AudioObjectPropertyAddress fallback =
            ca_address_of(kAudioHardwarePropertyDefaultInputDevice);
        if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &fallback,
                                           &CoreAudioSource::property_listener, this) == noErr) {
            this->listening_default_ = true;
        }
    }
}

void CoreAudioSource::remove_listeners_() {
    if (this->listening_alive_) {
        const AudioObjectPropertyAddress alive = ca_address_of(kAudioDevicePropertyDeviceIsAlive);
        AudioObjectRemovePropertyListener(this->listening_device_, &alive,
                                          &CoreAudioSource::property_listener, this);
        this->listening_alive_ = false;
    }
    if (this->listening_rate_) {
        const AudioObjectPropertyAddress rate =
            ca_address_of(kAudioDevicePropertyNominalSampleRate);
        AudioObjectRemovePropertyListener(this->listening_device_, &rate,
                                          &CoreAudioSource::property_listener, this);
        this->listening_rate_ = false;
    }
    this->listening_device_ = kAudioObjectUnknown;
    if (this->listening_default_) {
        const AudioObjectPropertyAddress fallback =
            ca_address_of(kAudioHardwarePropertyDefaultInputDevice);
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &fallback,
                                          &CoreAudioSource::property_listener, this);
        this->listening_default_ = false;
    }
}

OSStatus CoreAudioSource::property_listener(AudioObjectID /*object*/, UInt32 count,
                                            const AudioObjectPropertyAddress* addresses,
                                            void* user_data) {
    auto* self = static_cast<CoreAudioSource*>(user_data);
    for (UInt32 i = 0; i < count; ++i) {
        switch (addresses[i].mSelector) {
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyNominalSampleRate:
                self->device_lost_.store(true);
                self->data_ready_.notify_all();
                break;
            case kAudioHardwarePropertyDefaultInputDevice:
                self->default_moved_.store(true);
                break;
            default:
                break;
        }
    }
    return noErr;
}

OSStatus CoreAudioSource::input_callback(void* user_data, AudioUnitRenderActionFlags* flags,
                                         const AudioTimeStamp* timestamp, UInt32 bus, UInt32 frames,
                                         AudioBufferList* /*data*/) {
    const int64_t entered_us = now_us();
    const uint64_t entered_host = mach_absolute_time();

    auto* self = static_cast<CoreAudioSource*>(user_data);
    const size_t bytes_per_frame = self->bytes_per_frame_;
    const size_t wanted = static_cast<size_t>(frames) * bytes_per_frame;
    if (wanted == 0 || wanted > self->render_buffer_.size()) {
        return noErr;
    }

    // Interleaved, so one buffer carries every channel.
    AudioBufferList list = {};
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = self->format_.channels;
    list.mBuffers[0].mDataByteSize = static_cast<UInt32>(wanted);
    list.mBuffers[0].mData = self->render_buffer_.data();
    const OSStatus err = AudioUnitRender(self->unit_, flags, timestamp, bus, frames, &list);
    if (err != noErr) {
        return err;
    }

    int64_t first_us = entered_us - self->input_latency_us_;
    if (self->host_ticks_per_us_ > 0.0 && (timestamp->mFlags & kAudioTimeStampHostTimeValid) != 0) {
        // Signed on purpose: the buffer was captured before this callback, so this is negative.
        const auto ticks = static_cast<int64_t>(timestamp->mHostTime - entered_host);
        first_us += static_cast<int64_t>(
            std::llround(static_cast<double>(ticks) / self->host_ticks_per_us_));
    }
    // Before the frames go in, so read() never times a frame against an epoch older than its own.
    self->epoch_us_.store(first_us - static_cast<int64_t>(self->frames_captured_) * US_PER_S /
                                         self->format_.sample_rate);

    const size_t rendered = std::min(wanted, static_cast<size_t>(list.mBuffers[0].mDataByteSize));
    // Whole frames only: read() counts frames by integer division.
    size_t room = self->ring_.free_space();
    room -= room % bytes_per_frame;
    const size_t written = self->ring_.write(self->render_buffer_.data(), std::min(rendered, room));
    self->frames_captured_ += written / bytes_per_frame;
    if (written < rendered) {
        self->frames_dropped_.fetch_add((rendered - written) / bytes_per_frame,
                                        std::memory_order_relaxed);
    }

    // Notified without the mutex, keeping the callback lock-free; a missed wakeup is bounded.
    self->data_ready_.notify_one();
    return noErr;
}

}  // namespace sendspin_cli
