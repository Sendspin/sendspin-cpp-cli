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

/// getopt_long() for Windows, which has none: the subset cli.cpp uses, with BSD semantics
/// (no argument permutation, unique long-option prefixes, optreset).

#pragma once

#include <cstring>

inline char* optarg = nullptr;
inline int optind = 1;
inline int opterr = 1;
inline int optopt = 0;
inline int optreset = 0;

inline constexpr int no_argument = 0;
inline constexpr int required_argument = 1;

struct option {
    const char* name;
    int has_arg;
    int* flag;
    int val;
};

namespace getopt_win {

/// Position inside a grouped short-option word such as -lz.
inline int short_pos = 1;

/// The long option `name` (up to `length`) names: exact, else a unique prefix; nullptr otherwise.
inline const option* find_long(const option* longopts, const char* name, size_t length) {
    const option* prefix_match = nullptr;
    int prefix_matches = 0;
    for (const option* opt = longopts; opt->name != nullptr; ++opt) {
        if (std::strncmp(opt->name, name, length) != 0) {
            continue;
        }
        if (std::strlen(opt->name) == length) {
            return opt;
        }
        prefix_match = opt;
        ++prefix_matches;
    }
    return prefix_matches == 1 ? prefix_match : nullptr;
}

/// '?' or ':' for a missing value, as the optstring's leading ':' asks.
inline int missing_value(const char* optstring) {
    return optstring[0] == ':' ? ':' : '?';
}

}  // namespace getopt_win

inline int getopt_long(int argc, char* const argv[], const char* optstring, const option* longopts,
                       int* longindex) {
    if (optreset != 0 || optind == 0) {
        optreset = 0;
        optind = 1;
        getopt_win::short_pos = 1;
    }
    optarg = nullptr;
    optopt = 0;

    if (getopt_win::short_pos == 1) {
        if (optind >= argc || argv[optind][0] != '-' || argv[optind][1] == '\0') {
            return -1;
        }
        if (std::strcmp(argv[optind], "--") == 0) {
            ++optind;
            return -1;
        }
        if (argv[optind][1] == '-') {
            const char* name = argv[optind] + 2;
            const char* equals = std::strchr(name, '=');
            const size_t length =
                equals != nullptr ? static_cast<size_t>(equals - name) : std::strlen(name);
            const option* opt = getopt_win::find_long(longopts, name, length);
            ++optind;
            if (opt == nullptr || (opt->has_arg == no_argument && equals != nullptr)) {
                return '?';
            }
            if (opt->has_arg == required_argument) {
                if (equals != nullptr) {
                    optarg = const_cast<char*>(equals + 1);
                } else if (optind < argc) {
                    optarg = argv[optind++];
                } else {
                    optopt = opt->val;
                    return getopt_win::missing_value(optstring);
                }
            }
            if (longindex != nullptr) {
                *longindex = static_cast<int>(opt - longopts);
            }
            if (opt->flag != nullptr) {
                *opt->flag = opt->val;
                return 0;
            }
            return opt->val;
        }
    }

    const char* word = argv[optind];
    const char c = word[getopt_win::short_pos];
    const char* spec = c == ':' ? nullptr : std::strchr(optstring, c);
    const bool last_in_word = word[getopt_win::short_pos + 1] == '\0';
    const auto next_word = [] {
        ++optind;
        getopt_win::short_pos = 1;
    };

    if (spec == nullptr) {
        optopt = c;
        if (last_in_word) {
            next_word();
        } else {
            ++getopt_win::short_pos;
        }
        return '?';
    }
    if (spec[1] != ':') {
        if (last_in_word) {
            next_word();
        } else {
            ++getopt_win::short_pos;
        }
        return c;
    }

    // Takes a value: the rest of this word, else the next word.
    if (!last_in_word) {
        optarg = const_cast<char*>(word + getopt_win::short_pos + 1);
        next_word();
        return c;
    }
    next_word();
    if (optind < argc) {
        optarg = argv[optind++];
        return c;
    }
    optopt = c;
    return getopt_win::missing_value(optstring);
}
