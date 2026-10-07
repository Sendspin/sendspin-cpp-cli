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

#include "coreaudio_permission.h"

#import <AVFoundation/AVFoundation.h>

#include <dispatch/dispatch.h>

#include <cstdint>
#include <string>

namespace sendspin_cli {

namespace {

/// Longest the first-use prompt is waited on; an unattended start must not hang on it.
constexpr int64_t PROMPT_TIMEOUT_S = 60;

const char* const HOW_TO_GRANT =
    "allow it in System Settings > Privacy & Security > Microphone, for the app sendspin-cli is "
    "started from (the terminal, or sendspin-cli itself under launchd), then start again";

}  // namespace

bool microphone_prompt_pending() {
    return [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] ==
           AVAuthorizationStatusNotDetermined;
}

bool request_microphone_access(std::string& error) {
    AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (status == AVAuthorizationStatusNotDetermined) {
        dispatch_semaphore_t answered = dispatch_semaphore_create(0);
        __block BOOL granted = NO;
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                 completionHandler:^(BOOL allowed) {
                                   granted = allowed;
                                   dispatch_semaphore_signal(answered);
                                 }];
        const dispatch_time_t deadline =
            dispatch_time(DISPATCH_TIME_NOW, PROMPT_TIMEOUT_S * static_cast<int64_t>(NSEC_PER_SEC));
        if (dispatch_semaphore_wait(answered, deadline) != 0) {
            error = std::string("macOS asked for microphone access and nobody answered -- ") +
                    HOW_TO_GRANT;
            return false;
        }
        status = granted ? AVAuthorizationStatusAuthorized : AVAuthorizationStatusDenied;
    }
    if (status == AVAuthorizationStatusAuthorized) {
        return true;
    }
    error = std::string("macOS denies sendspin-cli the microphone, so --input would capture "
                        "silence -- ") +
            HOW_TO_GRANT;
    return false;
}

}  // namespace sendspin_cli
