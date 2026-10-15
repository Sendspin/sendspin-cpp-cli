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

/// A CoreAudio process tap on this host's output, read through a private aggregate device.

#pragma once

#include <CoreAudio/CoreAudio.h>

#include <string>

namespace sendspin_cli {

/// How to grant System Audio Recording, for logs that need to say so.
extern const char* const SYSTEM_AUDIO_HOW_TO_GRANT;

/// A tap on everything this host plays, or on one output device, minus this process's own
/// playback. The tap lives inside a private aggregate device that AUHAL can capture from.
class CoreAudioTap {
public:
    CoreAudioTap() = default;
    ~CoreAudioTap();

    CoreAudioTap(const CoreAudioTap&) = delete;
    CoreAudioTap& operator=(const CoreAudioTap&) = delete;

    /// True if this build and this macOS can tap, else false with `error` saying why.
    static bool supported(std::string& error);

    /// Creates the tap and its aggregate device, replacing any it already holds.
    /// @param output The output device to tap, or kAudioObjectUnknown for every output.
    bool create(AudioDeviceID output, std::string& error);

    /// Destroys the aggregate device, then the tap. Idempotent.
    void destroy();

    /// The aggregate device to capture from, or kAudioObjectUnknown before create().
    AudioDeviceID device() const {
        return this->aggregate_;
    }

private:
    AudioObjectID tap_{kAudioObjectUnknown};
    AudioDeviceID aggregate_{kAudioObjectUnknown};
};

}  // namespace sendspin_cli
