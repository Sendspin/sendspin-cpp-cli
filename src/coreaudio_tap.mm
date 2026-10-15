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

#include "coreaudio_tap.h"

#include "coreaudio_device.h"
#include "log.h"

#import <Foundation/Foundation.h>

#include <unistd.h>

// Process taps arrived in the macOS 14.2 SDK; older SDKs build without them.
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140200
#define SENDSPIN_CLI_HAVE_PROCESS_TAP 1
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
static_assert(sendspin_cli::TAP_FORMAT_SELECTOR == kAudioTapPropertyFormat);
#endif

namespace sendspin_cli {

#ifdef SENDSPIN_CLI_HAVE_PROCESS_TAP
static constexpr const char* LOG_TAG = LOG_TAG_AUDIO;
#endif

const char* const SYSTEM_AUDIO_HOW_TO_GRANT =
    "allow it in System Settings > Privacy & Security > Screen & System Audio Recording, under "
    "System Audio Recording Only, for the app sendspin-cli is started from (the terminal, or "
    "sendspin-cli itself under launchd). After a refusal macOS never asks again by itself: "
    "`tccutil reset AudioCapture` makes it ask on the next start";

CoreAudioTap::~CoreAudioTap() {
    this->destroy();
}

bool CoreAudioTap::supported(std::string& error) {
#ifdef SENDSPIN_CLI_HAVE_PROCESS_TAP
    if (@available(macOS 14.2, *)) {
        return true;
    }
    error = "--input loopback needs macOS 14.2 or later, which added the process taps it "
            "captures through";
#else
    error = "--input loopback needs process taps, and this build of sendspin-cli was made with a "
            "macOS SDK older than 14.2, which has none";
#endif
    return false;
}

bool CoreAudioTap::create(AudioDeviceID output, std::string& error) {
    this->destroy();
    if (!supported(error)) {
        return false;
    }
#ifdef SENDSPIN_CLI_HAVE_PROCESS_TAP
    if (@available(macOS 14.2, *)) {
        @autoreleasepool {
            NSString* output_uid = nil;
            if (output != kAudioObjectUnknown) {
                CFStringRef uid = nullptr;
                UInt32 size = sizeof(uid);
                const AudioObjectPropertyAddress address = ca_address_of(kAudioDevicePropertyDeviceUID);
                if (AudioObjectGetPropertyData(output, &address, 0, nullptr, &size, &uid) != noErr ||
                    uid == nullptr) {
                    error = "output device '" + ca_device_name(output) + "' has no UID to tap by";
                    return false;
                }
                output_uid = (__bridge_transfer NSString*)uid;
            }

            // Our own playback is left out, so a player on this host is never looped back.
            NSMutableArray<NSNumber*>* excluded = [NSMutableArray array];
            pid_t pid = getpid();
            AudioObjectID self_object = kAudioObjectUnknown;
            UInt32 size = sizeof(self_object);
            const AudioObjectPropertyAddress translate =
                ca_address_of(kAudioHardwarePropertyTranslatePIDToProcessObject);
            if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &translate, sizeof(pid), &pid,
                                           &size, &self_object) == noErr &&
                self_object != kAudioObjectUnknown) {
                [excluded addObject:@(self_object)];
            } else {
                cli_log(sendspin::LogLevel::WARN,
                        "coreaudio: macOS cannot name sendspin-cli's own audio process, so the "
                        "loopback tap includes this player's own playback");
            }

            CATapDescription* description =
                (output_uid == nil)
                    ? [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:excluded]
                    : [[CATapDescription alloc] initExcludingProcesses:excluded
                                                          andDeviceUID:output_uid
                                                            withStream:0];
            description.name = @"sendspin-cli loopback";
            description.privateTap = YES;
            description.muteBehavior = CATapUnmuted;

            OSStatus err = AudioHardwareCreateProcessTap(description, &this->tap_);
            if (err != noErr) {
                this->tap_ = kAudioObjectUnknown;
                error = "macOS would not create a process tap: " + ca_status_text(err);
                return false;
            }

            NSDictionary* aggregate = @{
                @kAudioAggregateDeviceNameKey : @"sendspin-cli loopback",
                @kAudioAggregateDeviceUIDKey : [[NSUUID UUID] UUIDString],
                @kAudioAggregateDeviceIsPrivateKey : @YES,
                @kAudioAggregateDeviceTapAutoStartKey : @YES,
                @kAudioAggregateDeviceTapListKey : @[ @{
                    @kAudioSubTapUIDKey : description.UUID.UUIDString,
                    @kAudioSubTapDriftCompensationKey : @YES,
                } ],
            };
            err = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate,
                                                     &this->aggregate_);
            if (err != noErr) {
                this->aggregate_ = kAudioObjectUnknown;
                error = "macOS would not create the aggregate device a process tap is read "
                        "through: " +
                        ca_status_text(err);
                this->destroy();
                return false;
            }
        }
        return true;
    }
#else
    (void)output;
#endif
    return false;
}

void CoreAudioTap::destroy() {
#ifdef SENDSPIN_CLI_HAVE_PROCESS_TAP
    if (@available(macOS 14.2, *)) {
        // The aggregate first: it holds the tap.
        if (this->aggregate_ != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(this->aggregate_);
        }
        if (this->tap_ != kAudioObjectUnknown) {
            AudioHardwareDestroyProcessTap(this->tap_);
        }
    }
#endif
    this->aggregate_ = kAudioObjectUnknown;
    this->tap_ = kAudioObjectUnknown;
}

}  // namespace sendspin_cli
