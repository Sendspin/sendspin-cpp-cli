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

/// @file cli_test.cpp
/// @brief parse_options() and parse_server_url(): what the flag surface accepts and rejects

#include "cli.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sendspin_cli {
namespace {

using sendspin::LogLevel;

/// Runs parse_options() on a command line written as a plain list of words.
///
/// Two things this owns rather than the test: the argv array (getopt wants a mutable
/// `char*[]` and permutes it in place, so nothing may point at a literal), and the
/// diagnostics stream. Diagnostics go to a tmpfile() so a test can read back the exact
/// wording -- nothing the parser says reaches the test runner's own stderr.
class Parse {
public:
    explicit Parse(std::vector<std::string> args) : words_(std::move(args)) {
        this->words_.insert(this->words_.begin(), "sendspin-cli");
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

// ---------------------------------------------------------------------------
// Every flag reaches its field
// ---------------------------------------------------------------------------

TEST(ParseOptions, EachFlagSetsItsField) {
    Parse parse({"-o", "null", "-n", "kitchen", "-s", "192.168.12.2", "-z", "-P", "/run/x.pid",
                 "-d", "debug", "-f", "/var/log/x.log", "--port", "9000"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().device, "null");
    EXPECT_EQ(parse.options().name, "kitchen");
    EXPECT_EQ(parse.options().server, "192.168.12.2");
    EXPECT_TRUE(parse.options().daemonize);
    EXPECT_EQ(parse.options().pidfile, "/run/x.pid");
    EXPECT_EQ(parse.options().log_level, LogLevel::DEBUG);
    EXPECT_EQ(parse.options().logfile, "/var/log/x.log");
    EXPECT_EQ(parse.options().port, 9000);
}

TEST(ParseOptions, ListDevicesIsReportedNotHandled) {
    Parse parse({"-l"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().list_devices);
}

TEST(ParseOptions, DefaultsWhenNothingIsGiven) {
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().device, DEFAULT_OUTPUT_DEVICE);
    EXPECT_EQ(parse.options().log_level, LogLevel::INFO);
    EXPECT_EQ(parse.options().port, sendspin::SendspinClientConfig::DEFAULT_SERVER_PORT);
    EXPECT_FALSE(parse.options().daemonize);
    EXPECT_FALSE(parse.options().list_devices);
    EXPECT_TRUE(parse.options().server.empty());
    EXPECT_TRUE(parse.options().server_url.empty());
    // -n falls back to the hostname, so the one thing promised is that it is not empty.
    EXPECT_FALSE(parse.options().name.empty());
}

TEST(ParseOptions, RepeatedFlagsTakeTheLastValue) {
    Parse parse({"-o", "stdout", "-o", "null", "--port", "9000", "--port", "9100"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().device, "null");
    EXPECT_EQ(parse.options().port, 9100);
}

TEST(ParseOptions, ShortAndLongHelpAgree) {
    Parse short_form({"-h"});
    Parse long_form({"--help"});

    ASSERT_TRUE(short_form.ok());
    ASSERT_TRUE(long_form.ok());
    EXPECT_TRUE(short_form.options().show_help);
    EXPECT_TRUE(long_form.options().show_help);
}

// ---------------------------------------------------------------------------
// --help / --version short-circuit
// ---------------------------------------------------------------------------

TEST(ParseOptions, HelpWinsOverAnInvalidFlagAfterIt) {
    // Asking for the flag list is exactly what someone does after getting one wrong, so
    // --help must not be the thing that fails.
    Parse parse({"--help", "--port", "0"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().show_help);
}

TEST(ParseOptions, VersionWinsOverAnInvalidFlagAfterIt) {
    Parse parse({"--version", "-s", "::1"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().show_version);
}

// ---------------------------------------------------------------------------
// Empty values
// ---------------------------------------------------------------------------

TEST(ParseOptions, EmptyValuesAreRejected) {
    // -n "" used to land silently on the hostname, which reads as the flag being ignored.
    for (const char* flag : {"-o", "-n", "-P", "-f"}) {
        Parse parse({flag, ""});

        EXPECT_FALSE(parse.ok()) << flag << " accepted an empty value";
        EXPECT_NE(parse.diagnostics().find("error:"), std::string::npos) << flag;
        EXPECT_NE(parse.diagnostics().find(flag), std::string::npos) << flag;
    }
}

// ---------------------------------------------------------------------------
// --port
// ---------------------------------------------------------------------------

TEST(ParseOptions, PortBounds) {
    for (const char* value : {"0", "65536", "abc", "12x", "", "-1", " 80"}) {
        Parse parse({"--port", value});

        EXPECT_FALSE(parse.ok()) << "--port accepted '" << value << "'";
        EXPECT_NE(parse.diagnostics().find("error: invalid --port"), std::string::npos) << value;
    }
}

TEST(ParseOptions, PortEdgesAreAccepted) {
    Parse low({"--port", "1"});
    Parse high({"--port", "65535"});

    ASSERT_TRUE(low.ok()) << low.diagnostics();
    ASSERT_TRUE(high.ok()) << high.diagnostics();
    EXPECT_EQ(low.options().port, 1);
    EXPECT_EQ(high.options().port, 65535);
}

// ---------------------------------------------------------------------------
// -d
// ---------------------------------------------------------------------------

TEST(ParseOptions, LogLevelNames) {
    const std::pair<const char*, LogLevel> cases[] = {
        {"none", LogLevel::NONE},   {"off", LogLevel::NONE},
        {"error", LogLevel::ERROR}, {"err", LogLevel::ERROR},
        {"warn", LogLevel::WARN},   {"warning", LogLevel::WARN},
        {"info", LogLevel::INFO},   {"debug", LogLevel::DEBUG},
        {"verbose", LogLevel::VERBOSE},
        // squeezelite's own name for the loudest level.
        {"sdebug", LogLevel::VERBOSE},
    };

    for (const auto& [name, level] : cases) {
        Parse parse({"-d", name});

        ASSERT_TRUE(parse.ok()) << "-d " << name << ": " << parse.diagnostics();
        EXPECT_EQ(parse.options().log_level, level) << name;
    }
}

TEST(ParseOptions, LogCategoryIsAcceptedAndWarnedAbout) {
    // squeezelite's -d <category>=<level> shape. One global level for now (roadmap item 6),
    // so the category is parsed, ignored, and said out loud.
    Parse parse({"-d", "slimproto=info"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().log_level, LogLevel::INFO);
    EXPECT_NE(parse.diagnostics().find("slimproto"), std::string::npos);
    EXPECT_NE(parse.diagnostics().find("ignored"), std::string::npos);
}

TEST(ParseOptions, UnknownLogLevelIsRejected) {
    for (const char* value : {"nonsense", "slimproto=nonsense", ""}) {
        Parse parse({"-d", value});

        EXPECT_FALSE(parse.ok()) << "-d accepted '" << value << "'";
        EXPECT_NE(parse.diagnostics().find("error: unknown log level"), std::string::npos) << value;
    }
}

// ---------------------------------------------------------------------------
// Malformed command lines
// ---------------------------------------------------------------------------

TEST(ParseOptions, PositionalArgumentIsRejected) {
    Parse parse({"extra"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("error: unexpected argument 'extra'"), std::string::npos);
}

TEST(ParseOptions, UnknownOptionIsRejectedInOurOwnWords) {
    Parse short_form({"-Q"});
    Parse long_form({"--bogus"});

    EXPECT_FALSE(short_form.ok());
    EXPECT_NE(short_form.diagnostics().find("error: unknown option '-Q'"), std::string::npos);

    EXPECT_FALSE(long_form.ok());
    EXPECT_NE(long_form.diagnostics().find("error: unknown option '--bogus'"), std::string::npos);
}

TEST(ParseOptions, MissingValueIsRejected) {
    Parse parse({"-o"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("needs a value"), std::string::npos);
}

// ---------------------------------------------------------------------------
// -s, through the parser
// ---------------------------------------------------------------------------

TEST(ParseOptions, ServerIsResolvedDuringParsing) {
    Parse parse({"-s", "192.168.12.2"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().server, "192.168.12.2");
    EXPECT_EQ(parse.options().server_url, "ws://192.168.12.2:8927/sendspin");
}

TEST(ParseOptions, BadServerFailsTheWholeParse) {
    Parse parse({"-s", "host:abc"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("error:"), std::string::npos);
    EXPECT_NE(parse.diagnostics().find("host:abc"), std::string::npos);
}

// ---------------------------------------------------------------------------
// -s, as a matrix over parse_server_url()
// ---------------------------------------------------------------------------

TEST(ParseServerUrl, Accepted) {
    const std::pair<const char*, const char*> cases[] = {
        // A bare host takes the port a Sendspin *server* listens on -- 8927, not the 8928
        // this player serves on.
        {"192.168.12.2", "ws://192.168.12.2:8927/sendspin"},
        {"192.168.12.2:8927", "ws://192.168.12.2:8927/sendspin"},
        {"music.local:9000", "ws://music.local:9000/sendspin"},
        // A full URL is the caller's to get right, path and all.
        {"ws://host:9000/sendspin", "ws://host:9000/sendspin"},
        {"wss://host/sendspin", "wss://host/sendspin"},
        // A bracketed IPv6 literal keeps its brackets in the URL.
        {"[::1]:8927", "ws://[::1]:8927/sendspin"},
        {"[::1]", "ws://[::1]:8927/sendspin"},
        {"[2001:db8::1]:9000", "ws://[2001:db8::1]:9000/sendspin"},
    };

    for (const auto& [input, expected] : cases) {
        std::string url;
        std::string error;

        ASSERT_TRUE(parse_server_url(input, url, error)) << input << ": " << error;
        EXPECT_EQ(url, expected) << input;
    }
}

TEST(ParseServerUrl, Rejected) {
    const char* cases[] = {
        "",              // nothing at all
        "host:abc",      // not a port
        "host:",         // a truncated line, not a request for the default
        ":8927",         // no host
        "host:0",        // ports are 1-65535
        "host:70000",    // ditto
        "::1",           // an IPv6 literal must be bracketed to be told from host:port
        "[::1",          // unterminated bracket
        "[::1]junk",     // trailing text where a port belongs
        "[]",            // no host
        "[]:8927",       // ditto
        "http://host",   // Sendspin is WebSocket only
        "https://host",  // ditto
    };

    for (const char* input : cases) {
        std::string url;
        std::string error;

        EXPECT_FALSE(parse_server_url(input, url, error)) << "accepted '" << input << "'";
        EXPECT_FALSE(error.empty()) << "no reason given for '" << input << "'";
    }
}

// ---------------------------------------------------------------------------
// Config-file precedence hooks (roadmap item 8)
// ---------------------------------------------------------------------------

TEST(ParseOptions, TracksWhichOptionsWereExplicitlyGiven) {
    Parse parse({"-o", "null"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().was_given(Opt::Device));
    // -n and --port hold values, but the parser put them there, not the user. A config
    // file has to be able to tell those apart to layer under the command line.
    EXPECT_FALSE(parse.options().was_given(Opt::Name));
    EXPECT_FALSE(parse.options().was_given(Opt::Port));
    EXPECT_FALSE(parse.options().was_given(Opt::Server));
    EXPECT_FALSE(parse.options().was_given(Opt::LogLevel));
}

TEST(ParseOptions, ExplicitlyGivenTracksEveryOption) {
    Parse parse({"-o", "null", "-l", "-n", "kitchen", "-s", "host", "-z", "-P", "/run/x.pid", "-d",
                 "debug", "-f", "/var/log/x.log", "--port", "9000"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    for (const Opt opt : {Opt::Device, Opt::ListDevices, Opt::Name, Opt::Server, Opt::Daemonize,
                          Opt::Pidfile, Opt::Logfile, Opt::LogLevel, Opt::Port}) {
        EXPECT_TRUE(parse.options().was_given(opt))
            << "option " << static_cast<unsigned>(opt) << " not recorded as given";
    }
}

TEST(ParseOptions, AValueEqualToTheDefaultStillCountsAsGiven) {
    // The point of the hook: -o with the default's own value is still the user's choice,
    // and must outrank a config file that says otherwise.
    Parse parse({"-o", DEFAULT_OUTPUT_DEVICE});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().was_given(Opt::Device));
}

// ---------------------------------------------------------------------------
// getopt's global state
// ---------------------------------------------------------------------------

TEST(ParseOptions, ParsingTwiceInOneProcessGivesTheSameAnswers) {
    // getopt's scan position is process-global. Without the reset in parse_options() the
    // second call would resume wherever the first stopped -- and every other test in this
    // binary is already a second call.
    {
        Parse first({"-o", "stdout", "--port", "9000"});
        ASSERT_TRUE(first.ok()) << first.diagnostics();
        EXPECT_EQ(first.options().device, "stdout");
        EXPECT_EQ(first.options().port, 9000);
    }
    {
        Parse second({"-n", "kitchen", "-d", "debug"});
        ASSERT_TRUE(second.ok()) << second.diagnostics();
        EXPECT_EQ(second.options().name, "kitchen");
        EXPECT_EQ(second.options().log_level, LogLevel::DEBUG);
        // The first parse's values must not leak into the second.
        EXPECT_EQ(second.options().device, DEFAULT_OUTPUT_DEVICE);
        EXPECT_EQ(second.options().port, sendspin::SendspinClientConfig::DEFAULT_SERVER_PORT);
    }
}

TEST(ParseOptions, AFailedParseDoesNotStrandTheNextOne) {
    // A parse that bails mid-line leaves getopt stopped partway through argv. The next
    // caller must still see its own arguments from the start.
    {
        Parse failed({"-o", "null", "-Q", "-n", "ignored"});
        ASSERT_FALSE(failed.ok());
    }
    {
        Parse after({"-n", "kitchen"});
        ASSERT_TRUE(after.ok()) << after.diagnostics();
        EXPECT_EQ(after.options().name, "kitchen");
    }
}

}  // namespace
}  // namespace sendspin_cli
