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

/// macOS microphone (TCC) permission for the CoreAudio capture source.

#pragma once

#include <string>

namespace sendspin_cli {

/// True if macOS has not been asked yet, so request_microphone_access() will raise its prompt.
bool microphone_prompt_pending();

/// Checks microphone access, raising macOS's prompt on first use and waiting for the answer.
/// @return false with `error` saying how to grant it.
bool request_microphone_access(std::string& error);

}  // namespace sendspin_cli
