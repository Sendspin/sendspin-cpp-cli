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

/// One environment variable, set for the length of a test.

#pragma once

#include <cstdlib>
#include <string>

namespace sendspin_cli {

/// Sets an environment variable for a test and restores it: every suite shares one process.
class ScopedEnv {
public:
    /// @param value The value to set, or nullptr to unset the variable.
    ScopedEnv(const char* name, const char* value) : name_(name) {
        const char* previous = std::getenv(name);
        this->had_previous_ = previous != nullptr;
        if (this->had_previous_) {
            this->previous_ = previous;
        }
        if (value == nullptr) {
            ::unsetenv(name);
        } else {
            ::setenv(name, value, 1);
        }
    }

    ~ScopedEnv() {
        if (this->had_previous_) {
            ::setenv(this->name_, this->previous_.c_str(), 1);
        } else {
            ::unsetenv(this->name_);
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::string previous_;
    bool had_previous_{false};
};

}  // namespace sendspin_cli
