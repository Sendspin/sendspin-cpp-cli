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

#include "coreaudio_device.h"

#include "audio_sink.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace sendspin_cli {

namespace {

AudioObjectPropertyScope scope_of(CaDirection direction) {
    return direction == CaDirection::Output ? kAudioDevicePropertyScopeOutput
                                            : kAudioDevicePropertyScopeInput;
}

/// The flag that names a device on that side, for error text.
const char* flag_of(CaDirection direction) {
    return direction == CaDirection::Output ? "-o" : "--input";
}

const char* noun_of(CaDirection direction) {
    return direction == CaDirection::Output ? "output" : "input";
}

std::string cf_string_to_utf8(CFStringRef value) {
    if (value == nullptr) {
        return {};
    }
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(capacity), '\0');
    if (CFStringGetCString(value, out.data(), capacity, kCFStringEncodingUTF8) == 0) {
        return {};
    }
    out.resize(std::strlen(out.c_str()));
    return out;
}

/// True if `value` is a non-empty run of decimal digits.
bool is_device_index(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    return value.find_first_not_of("0123456789") == std::string::npos;
}

bool iequals(const std::string& value, const std::string& other) {
    if (value.size() != other.size()) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
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

AudioObjectPropertyAddress ca_address_of(AudioObjectPropertySelector selector,
                                         AudioObjectPropertyScope scope) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}

std::string ca_status_text(OSStatus status) {
    char code[5] = {};
    const auto value = static_cast<uint32_t>(status);
    for (int i = 0; i < 4; ++i) {
        code[i] = static_cast<char>((value >> (24 - (8 * i))) & 0xFFU);
        if (std::isprint(static_cast<unsigned char>(code[i])) == 0) {
            return std::to_string(static_cast<long>(status));
        }
    }
    return std::string("'") + code + "' (" + std::to_string(static_cast<long>(status)) + ")";
}

std::string ca_device_name(AudioDeviceID device) {
    const AudioObjectPropertyAddress addr = ca_address_of(kAudioObjectPropertyName);
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &value) != noErr ||
        value == nullptr) {
        return "(unknown device)";
    }
    std::string name = cf_string_to_utf8(value);
    CFRelease(value);
    return name.empty() ? "(unknown device)" : name;
}

uint32_t ca_u32_property(AudioObjectID object, AudioObjectPropertySelector selector,
                         AudioObjectPropertyScope scope) {
    const AudioObjectPropertyAddress addr = ca_address_of(selector, scope);
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value) != noErr) {
        return 0;
    }
    return value;
}

uint32_t ca_channels(AudioDeviceID device, CaDirection direction) {
    const AudioObjectPropertyAddress addr =
        ca_address_of(kAudioDevicePropertyStreamConfiguration, scope_of(direction));
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }
    // Through a 64-bit vector: an AudioBufferList holds pointers and must be aligned for them.
    std::vector<uint64_t> storage((size + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) != noErr) {
        return 0;
    }
    uint32_t total = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
        total += list->mBuffers[i].mNumberChannels;
    }
    return total;
}

double ca_nominal_sample_rate(AudioDeviceID device) {
    const AudioObjectPropertyAddress addr = ca_address_of(kAudioDevicePropertyNominalSampleRate);
    Float64 value = 0.0;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &value) != noErr) {
        return 0.0;
    }
    return value;
}

uint32_t ca_first_stream_latency(AudioDeviceID device, CaDirection direction) {
    const AudioObjectPropertyAddress addr =
        ca_address_of(kAudioDevicePropertyStreams, scope_of(direction));
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr ||
        size < sizeof(AudioStreamID)) {
        return 0;
    }
    std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, streams.data()) != noErr) {
        return 0;
    }
    return ca_u32_property(streams.front(), kAudioStreamPropertyLatency,
                           kAudioObjectPropertyScopeGlobal);
}

AudioDeviceID ca_default_device(CaDirection direction) {
    const AudioObjectPropertyAddress addr =
        ca_address_of(direction == CaDirection::Output ? kAudioHardwarePropertyDefaultOutputDevice
                                                       : kAudioHardwarePropertyDefaultInputDevice);
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &device) !=
        noErr) {
        return kAudioObjectUnknown;
    }
    return device;
}

std::vector<AudioDeviceID> ca_enumerate_devices(CaDirection direction) {
    const AudioObjectPropertyAddress addr = ca_address_of(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) !=
            noErr ||
        size < sizeof(AudioDeviceID)) {
        return {};
    }
    std::vector<AudioDeviceID> all(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
                                   all.data()) != noErr) {
        return {};
    }

    std::vector<AudioDeviceID> devices;
    for (const AudioDeviceID device : all) {
        if (ca_channels(device, direction) > 0) {
            devices.push_back(device);
        }
    }
    return devices;
}

bool resolve_ca_device(const std::string& device, CaDirection direction, AudioDeviceID& out,
                       std::string& error) {
    const std::string noun = noun_of(direction);
    if (device.empty()) {
        const AudioDeviceID fallback = ca_default_device(direction);
        if (fallback == kAudioObjectUnknown) {
            error = "this host has no default CoreAudio " + noun +
                    " device at all -- run with -l to see what it does have";
            return false;
        }
        out = fallback;
        return true;
    }

    const std::vector<AudioDeviceID> devices = ca_enumerate_devices(direction);
    if (devices.empty()) {
        error = "this host has no CoreAudio " + noun + " devices at all";
        return false;
    }

    const std::string spec = std::string(flag_of(direction)) + " coreaudio:" + device;
    if (is_device_index(device)) {
        // strtoull saturates rather than wrapping into a plausible index.
        const unsigned long long value = std::strtoull(device.c_str(), nullptr, 10);
        if (value >= devices.size()) {
            error = spec + ": no device at that index -- indices run 0-" +
                    std::to_string(devices.size() - 1) + " here, and -l lists them";
            return false;
        }
        out = devices[static_cast<size_t>(value)];
        return true;
    }

    std::vector<size_t> matches;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (iequals(device, ca_device_name(devices[i]))) {
            matches.push_back(i);
        }
    }
    if (matches.empty()) {
        error = spec + ": no " + noun + " device by that name -- run with -l to list them";
        return false;
    }
    if (matches.size() > 1) {
        std::string indices;
        for (const size_t index : matches) {
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

    out = devices[matches.front()];
    return true;
}

bool ca_asbd_for(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample,
                 AudioStreamBasicDescription& out) {
    if (sample_rate == 0 || channels == 0) {
        return false;
    }
    if (std::find(PROBE_BIT_DEPTHS.begin(), PROBE_BIT_DEPTHS.end(), bits_per_sample) ==
        PROBE_BIT_DEPTHS.end()) {
        return false;
    }
    out = {};
    out.mSampleRate = static_cast<Float64>(sample_rate);
    out.mFormatID = kAudioFormatLinearPCM;
    // Little-endian is the absence of kAudioFormatFlagIsBigEndian; packed rules out 24-in-32.
    out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    out.mBitsPerChannel = bits_per_sample;
    out.mChannelsPerFrame = channels;
    out.mFramesPerPacket = 1;
    out.mBytesPerFrame = static_cast<UInt32>(channels) * (bits_per_sample / 8U);
    out.mBytesPerPacket = out.mBytesPerFrame;
    return true;
}

AudioUnit ca_new_hal_unit() {
    AudioComponentDescription description = {};
    description.componentType = kAudioUnitType_Output;
    description.componentSubType = kAudioUnitSubType_HALOutput;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &description);
    if (component == nullptr) {
        return nullptr;
    }
    AudioUnit unit = nullptr;
    if (AudioComponentInstanceNew(component, &unit) != noErr) {
        return nullptr;
    }
    return unit;
}

double ca_host_ticks_per_us() {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) {
        return 0.0;
    }
    // A tick is numer/denom nanoseconds, so a microsecond is 1000 * denom/numer ticks.
    return 1000.0 * static_cast<double>(timebase.denom) / static_cast<double>(timebase.numer);
}

}  // namespace sendspin_cli
