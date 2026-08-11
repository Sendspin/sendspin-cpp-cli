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

/// @file parse_harness.h
/// @brief One parse_options() call, with its argv and its diagnostics owned for you
///
/// Shared by the two suites that drive the parser -- the flag surface and the config file --
/// rather than copied into each, for scoped_env.h's reason: the hermeticity rule below is the
/// kind of thing that must not be fixed in only one of them.

#pragma once

#include "cli.h"
#include "control.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sendspin_cli {

/// @brief Runs parse_options() on a command line written as a plain list of words.
///
/// Three things this owns rather than the test: the argv array (getopt wants a mutable `char*[]`
/// and permutes it in place, so nothing may point at a literal), the diagnostics stream (a
/// tmpfile(), so a test can read the exact wording back and nothing reaches the runner's own
/// stderr), and which config file the parse reads.
///
/// **That last one is what keeps this suite runnable anywhere.** `parse_options()` searches
/// `$XDG_CONFIG_HOME`, `$HOME/.config` and `/etc/sendspin-cli.conf`, so a parse left to its own
/// devices would read whatever the machine running the tests happens to have -- and `/etc` is not
/// something a test can move out of the way. So every parse here names a config explicitly, and
/// the default is `/dev/null`: a file that always exists, always reads as valid, and always
/// contains nothing. Tests about the *search itself* go to config_search_paths() and
/// load_config_file() directly, which take the list to walk.
class Parse {
public:
    /// @param args The command line after argv[0]. A subcommand must still come first.
    /// @param config The config file to read. Defaults to a file with nothing in it; pass a real
    /// path to test what a config does.
    explicit Parse(std::vector<std::string> args, std::string config = "/dev/null")
        : words_(std::move(args)) {
        this->words_.insert(this->words_.begin(), "sendspin-cli");
        // Inserted at the front of the flags rather than appended, and the difference matters both
        // ways: a subcommand and its argument have to stay at argv[1..], and a test that
        // deliberately leaves a flag's value off would swallow `--config` as that value.
        size_t at = 1;
        if (this->words_.size() > 1 && this->words_[1][0] != '-') {
            // argv[1] is the subcommand *position* whether or not the word there is a real
            // subcommand, so the insert goes after it either way -- otherwise a bare word would be
            // diagnosed as a stray argument rather than as the subcommand typo it is.
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
