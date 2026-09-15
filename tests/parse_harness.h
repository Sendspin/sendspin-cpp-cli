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

/// One parse_options() call, with its argv and its diagnostics owned for you.

#pragma once

#include "cli.h"
#include "control.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sendspin_cli {

/// Runs parse_options() on a list of words, owning argv and a tmpfile() diagnostics stream.
/// Always names a config (default /dev/null) so no machine's real config leaks into a test.
class Parse {
public:
    /// @param args The command line after argv[0]. A subcommand must still come first.
    /// @param config The config file to read; defaults to an empty one.
    explicit Parse(std::vector<std::string> args, std::string config = "/dev/null")
        : words_(std::move(args)) {
        this->words_.insert(this->words_.begin(), "sendspin-cli");
        // Inserted after any subcommand, and before flags so a missing value cannot swallow it.
        size_t at = 1;
        if (this->words_.size() > 1 && this->words_[1][0] != '-') {
            // argv[1] is the subcommand position whether or not the word is a real subcommand.
            const ControlSubcommand* subcommand = find_control_subcommand(this->words_[1]);
            at = 2 + (subcommand == nullptr ? 0 : subcommand->arity);
        }
        at = std::min(at, this->words_.size());
        this->words_.insert(this->words_.begin() + static_cast<long>(at),
                            {"--config", std::move(config)});
        this->argv_.reserve(this->words_.size() + 1);
        for (std::string& word : this->words_) {
            this->argv_.push_back(word.data());
        }
        this->argv_.push_back(nullptr);

        this->err_ = std::tmpfile();
        this->ok_ = parse_options(static_cast<int>(this->words_.size()), this->argv_.data(),
                                  this->options_, this->err_);
    }

    ~Parse() {
        if (this->err_ != nullptr) {
            std::fclose(this->err_);
        }
    }

    Parse(const Parse&) = delete;
    Parse& operator=(const Parse&) = delete;

    bool ok() const {
        return this->ok_;
    }

    const Options& options() const {
        return this->options_;
    }

    /// Everything the parser wrote to its diagnostics stream, as one string.
    std::string diagnostics() {
        std::rewind(this->err_);
        std::string text;
        char buffer[512];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), this->err_)) > 0) {
            text.append(buffer, read);
        }
        return text;
    }

private:
    std::vector<std::string> words_;
    std::vector<char*> argv_;
    Options options_;
    std::FILE* err_{nullptr};
    bool ok_{false};
};

}  // namespace sendspin_cli
