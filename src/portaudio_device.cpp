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

#include "portaudio_device.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sendspin_cli {

namespace {

/// The flag that names a device on that side, for error text.
const char* flag_of(PaDirection direction) {
    return direction == PaDirection::Output ? "-o" : "--input";
}

const char* noun_of(PaDirection direction) {
    return direction == PaDirection::Output ? "output" : "input";
}

/// True if `value` is a non-empty run of decimal digits.
bool is_device_index(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    return value.find_first_not_of("0123456789") == std::string::npos;
}

bool iequals(const std::string& value, const char* other) {
    if (other == nullptr) {
        return false;
    }
    const size_t len = std::strlen(other);
    if (value.size() != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        // Through unsigned char: tolower() is undefined for negative chars.
        const int a = std::tolower(static_cast<unsigned char>(value[i]));
        const int b = std::tolower(static_cast<unsigned char>(other[i]));
        if (a != b) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::mutex& pa_api_mutex() {
    static std::mutex mutex;
    return mutex;
}

PortAudioGuard::PortAudioGuard() {
    const std::lock_guard<std::mutex> pa_lock(pa_api_mutex());
    this->err_ = Pa_Initialize();
}

PortAudioGuard::~PortAudioGuard() {
    const std::lock_guard<std::mutex> pa_lock(pa_api_mutex());
    if (this->err_ == paNoError) {
        Pa_Terminate();
    }
}

bool PortAudioGuard::ok() const {
    return this->err_ == paNoError;
}

const char* PortAudioGuard::error() const {
    return Pa_GetErrorText(this->err_);
}

bool PortAudioGuard::reinitialize() {
    const std::lock_guard<std::mutex> pa_lock(pa_api_mutex());
    if (this->err_ == paNoError) {
        Pa_Terminate();
    }
    // Overwritten so ok() and the destructor track the live state.
    this->err_ = Pa_Initialize();
    return this->err_ == paNoError;
}

bool pa_format_for(uint8_t bits_per_sample, PaSampleFormat& format) {
    for (size_t i = 0; i < PROBE_BIT_DEPTHS.size(); ++i) {
        if (PROBE_BIT_DEPTHS[i] == bits_per_sample) {
            format = PA_FORMATS[i];
            return true;
        }
    }
    return false;
}

bool pa_has_channels(const PaDeviceInfo* info, PaDirection direction) {
    if (info == nullptr) {
        return false;
    }
    return (direction == PaDirection::Output ? info->maxOutputChannels : info->maxInputChannels) >
           0;
}

bool resolve_pa_device(const std::string& device, PaDirection direction, PaDeviceIndex& out,
                       std::string& error) {
    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        error = std::string("cannot enumerate PortAudio devices: ") + Pa_GetErrorText(count);
        return false;
    }
    if (count == 0) {
        error = "PortAudio found no audio devices on this host at all";
        return false;
    }

    const std::string flag = flag_of(direction);
    const std::string noun = noun_of(direction);
    if (device.empty()) {
        const PaDeviceIndex fallback = direction == PaDirection::Output
                                           ? Pa_GetDefaultOutputDevice()
                                           : Pa_GetDefaultInputDevice();
        if (fallback == paNoDevice) {
            error = "this host has no PortAudio " + noun +
                    " device at all -- run with -l to see what it does have";
            return false;
        }
        out = fallback;
        return true;
    }

    const std::string spec = flag + " portaudio:" + device;
    if (is_device_index(device)) {
        // strtoull saturates rather than wrapping into a plausible index.
        const unsigned long long value = std::strtoull(device.c_str(), nullptr, 10);
        if (value >= static_cast<unsigned long long>(count)) {
            // Report the index range, not the count: indices include the other side's devices.
            error = spec + ": no device at that index -- indices run 0-" +
                    std::to_string(count - 1) + " here, and -l lists the ones " + flag +
                    " can reach";
            return false;
        }
        const auto index = static_cast<PaDeviceIndex>(value);
        if (!pa_has_channels(Pa_GetDeviceInfo(index), direction)) {
            error = spec + ": that device has no " + noun + " channels -- run with -l, which " +
                    "lists only the ones " + flag + " can reach";
            return false;
        }
        out = index;
        return true;
    }

    std::vector<PaDeviceIndex> matches;
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (pa_has_channels(info, direction) && iequals(device, info->name)) {
            matches.push_back(i);
        }
    }
    if (matches.empty()) {
        error = spec + ": no " + noun + " device by that name -- run with -l to list them";
        return false;
    }
    if (matches.size() > 1) {
        std::string indices;
        for (const PaDeviceIndex index : matches) {
            if (!indices.empty()) {
                indices += ", ";
            }
            indices += std::to_string(index);
        }
        error = spec + ": " + std::to_string(matches.size()) + " " + noun +
                " devices share that name (indices " + indices +
                ") -- name the one you mean by index instead";
        return false;
    }

    out = matches.front();
    return true;
}

}  // namespace sendspin_cli
