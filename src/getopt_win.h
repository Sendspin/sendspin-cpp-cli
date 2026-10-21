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

/// getopt_long() for Windows, which has none: the subset cli.cpp uses, behaving as glibc's and
/// the BSDs' do (argv permuted so options come first, unique long-option prefixes, optreset).

#pragma once

#include <algorithm>
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

inline bool is_option(const char* word) {
    return word[0] == '-' && word[1] != '\0';
}

/// How many words the option word at `index` takes: 2 when its value is the next word.
inline int option_words(int argc, char* const argv[], int index, const char* optstring,
                        const option* longopts) {
    const char* word = argv[index];
    if (index + 1 >= argc) {
        return 1;
    }
    if (word[1] == '-') {
        const char* name = word + 2;
        if (*name == '\0' || std::strchr(name, '=') != nullptr) {
            return 1;
        }
        const option* opt = find_long(longopts, name, std::strlen(name));
        return opt != nullptr && opt->has_arg == required_argument ? 2 : 1;
    }
    for (const char* c = word + 1; *c != '\0'; ++c) {
        const char* spec = *c == ':' ? nullptr : std::strchr(optstring, *c);
        if (spec != nullptr && spec[1] == ':') {
            // The rest of the word is the value, or the next word is.
            return c[1] == '\0' ? 2 : 1;
        }
    }
    return 1;
}

/// Moves the next option (and its value word) in front of the non-options at optind.
/// @return false when no option is left, which leaves optind on the first non-option.
inline bool permute(int argc, char* const argv[], const char* optstring, const option* longopts) {
    int next = optind;
    while (next < argc && !is_option(argv[next])) {
        ++next;
    }
    if (next >= argc) {
        return false;
    }
    const int words = std::strcmp(argv[next], "--") == 0
                          ? 1
                          : option_words(argc, argv, next, optstring, longopts);
    char** const words_start = const_cast<char**>(argv);
    std::rotate(words_start + optind, words_start + next, words_start + next + words);
    return true;
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
        if (optind >= argc) {
            return -1;
        }
        if (!getopt_win::is_option(argv[optind]) &&
            !getopt_win::permute(argc, argv, optstring, longopts)) {
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
