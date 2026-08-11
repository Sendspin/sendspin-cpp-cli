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

#include "control.h"
#include "log.h"
#include "scoped_env.h"

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

TEST(ParseOptions, HelpWinsOverAnInvalidFlagBeforeIt) {
    // The harder half, and the one worth a test per flag: appending --help to a line you
    // already got wrong is the most likely way to reach for it. Diagnostics are collected
    // rather than returned on, so a failure earlier in argv cannot pre-empt the flag list.
    const std::vector<std::string> bad_prefixes[] = {
        {"--port", "0"},      // validated inline, as -s is not
        {"-o", ""},           // an empty value
        {"-d", "nonsense"},   // an unknown log level
        {"-Q"},               // an unknown flag entirely
        {"-s", "::1"},        // resolved after the loop
        {"extra"},            // a positional argument
    };

    for (const std::vector<std::string>& prefix : bad_prefixes) {
        {
            std::vector<std::string> args = prefix;
            args.emplace_back("--help");
            Parse parse(args);
            EXPECT_TRUE(parse.ok()) << "--help lost to " << prefix.front();
            EXPECT_TRUE(parse.options().show_help) << prefix.front();
        }
        {
            std::vector<std::string> args = prefix;
            args.emplace_back("--version");
            Parse parse(args);
            EXPECT_TRUE(parse.ok()) << "--version lost to " << prefix.front();
            EXPECT_TRUE(parse.options().show_version) << prefix.front();
        }
        {
            // Without the short-circuit the same line must still fail.
            Parse parse(prefix);
            EXPECT_FALSE(parse.ok()) << prefix.front() << " should not parse on its own";
        }
    }
}

TEST(ParseOptions, AFlagValueThatLooksLikeHelpIsNotHelp) {
    // Why the short-circuit is deferred to getopt rather than pre-scanned for "--help":
    // here the word is -n's value, and naming a player "--help" must not print usage.
    Parse parse({"-n", "--help"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_FALSE(parse.options().show_help);
    EXPECT_EQ(parse.options().name, "--help");
}

TEST(ParseOptions, OnlyTheFirstProblemIsReported) {
    // Collecting diagnostics must not turn one bad line into a wall of them.
    Parse parse({"--port", "0", "-o", "", "-d", "nonsense"});

    ASSERT_FALSE(parse.ok());
    const std::string diagnostics = parse.diagnostics();
    EXPECT_NE(diagnostics.find("invalid --port"), std::string::npos) << diagnostics;
    EXPECT_EQ(diagnostics.find("non-empty"), std::string::npos)
        << "later problems should stay quiet: " << diagnostics;
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
// --buffer-ms
// ---------------------------------------------------------------------------

TEST(ParseOptions, BufferMsIsAccepted) {
    Parse parse({"--buffer-ms", "250"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().buffer_ms, 250U);
    EXPECT_TRUE(parse.options().was_given(Opt::BufferMs));
}

TEST(ParseOptions, BufferMsDefaultsWithoutBeingGiven) {
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().buffer_ms, DEFAULT_BUFFER_MS);
    EXPECT_FALSE(parse.options().was_given(Opt::BufferMs));
}

TEST(ParseOptions, BufferMsBounds) {
    // Zero, non-numeric, empty, signed and either side of the range all refuse to start --
    // nothing here warns and carries on with the default.
    for (const char* value : {"0", "9", "2001", "abc", "12x", "", "-1", " 100", "+100", "100.5"}) {
        Parse parse({"--buffer-ms", value});

        EXPECT_FALSE(parse.ok()) << "--buffer-ms accepted '" << value << "'";
        const std::string diagnostics = parse.diagnostics();
        EXPECT_NE(diagnostics.find("error: invalid --buffer-ms"), std::string::npos) << value;
        // The message names the value it refused, not just the flag.
        EXPECT_NE(diagnostics.find(std::string("'") + value + "'"), std::string::npos) << value;
    }
}

TEST(ParseOptions, BufferMsEdgesAreAccepted) {
    Parse low({"--buffer-ms", std::to_string(MIN_BUFFER_MS)});
    Parse high({"--buffer-ms", std::to_string(MAX_BUFFER_MS)});

    ASSERT_TRUE(low.ok()) << low.diagnostics();
    ASSERT_TRUE(high.ok()) << high.diagnostics();
    EXPECT_EQ(low.options().buffer_ms, MIN_BUFFER_MS);
    EXPECT_EQ(high.options().buffer_ms, MAX_BUFFER_MS);
}

TEST(ParseOptions, BufferMsNeedsAValue) {
    Parse parse({"--buffer-ms"});

    ASSERT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("option '--buffer-ms' needs a value"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, BufferMsDoesNotClaimDashA) {
    // squeezelite's -a is deliberately left unclaimed: its <b>:<p>:<f>:<m> grammar is
    // ALSA-only, and two of its four subfields are already fixed here.
    Parse parse({"-a", "100"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("unknown option '-a'"), std::string::npos)
        << parse.diagnostics();
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

TEST(ParseOptions, LogCategoryIsAcceptedAndWarnedAboutWithSomethingToDoInstead) {
    // squeezelite's -d <category>=<level> shape. sendspin-cpp gates every line on one global
    // int with no sink hook, so the category is parsed, ignored, and said out loud -- and the
    // warning has to leave the user somewhere to go, which is the per-line tag plus grep.
    Parse parse({"-d", "slimproto=info"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().log_level, LogLevel::INFO);
    const std::string diagnostics = parse.diagnostics();
    EXPECT_NE(diagnostics.find("slimproto"), std::string::npos) << diagnostics;
    EXPECT_NE(diagnostics.find("ignored"), std::string::npos) << diagnostics;
    EXPECT_NE(diagnostics.find("grep"), std::string::npos) << diagnostics;
    for (const char* tag : LOG_TAGS) {
        EXPECT_NE(diagnostics.find(tag), std::string::npos)
            << tag << " missing from: " << diagnostics;
    }
}

// ---------------------------------------------------------------------------
// -z
// ---------------------------------------------------------------------------

TEST(ParseOptions, DaemonizeRefusesToWritePcmToStdout) {
    // A detached daemon's stdout is /dev/null, so the PCM sink would become a second discard
    // sink without saying so. Both spellings of the sink, since -o accepts both.
    for (const char* device : {"stdout", "-"}) {
        Parse parse({"-z", "-o", device});

        EXPECT_FALSE(parse.ok()) << "-z -o " << device << " was accepted";
        EXPECT_NE(parse.diagnostics().find("-z cannot write PCM to stdout"), std::string::npos)
            << parse.diagnostics();
    }
}

TEST(ParseOptions, DaemonizeIsFineWithADeviceThatIsNotStdout) {
    Parse parse({"-z", "-o", "null"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().daemonize);
}

TEST(ParseOptions, StdoutWithoutDaemonizeIsStillFine) {
    // The refusal is about -z, not about the sink: piping PCM out of a foreground run is what
    // -o stdout is for.
    Parse parse({"-o", "stdout"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.diagnostics().find("error:"), std::string::npos) << parse.diagnostics();
}

TEST(ParseOptions, DaemonizeWithoutALogfileWarnsAndStillStarts) {
    Parse parse({"-z", "-o", "null"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("warning:"), std::string::npos) << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("-z without -f"), std::string::npos) << parse.diagnostics();
}

TEST(ParseOptions, DaemonizeWithALogfileSaysNothing) {
    Parse parse({"-z", "-o", "null", "-f", "/tmp/sendspin-cli-test.log"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.diagnostics().find("warning:"), std::string::npos) << parse.diagnostics();
}

TEST(ParseOptions, DaemonizeMakesRelativePidfileAndLogfilePathsAbsolute) {
    // -z chdir()s to /, so a relative path would name the operator's directory to the
    // parent's pidfile probe and a file directly under / to the child that writes it.
    Parse parse({"-z", "-o", "null", "-P", "sendspin.pid", "-f", "sendspin.log"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().pidfile.front(), '/') << parse.options().pidfile;
    EXPECT_EQ(parse.options().logfile.front(), '/') << parse.options().logfile;
    EXPECT_NE(parse.options().pidfile.find("/sendspin.pid"), std::string::npos)
        << parse.options().pidfile;
    EXPECT_NE(parse.options().logfile.find("/sendspin.log"), std::string::npos)
        << parse.options().logfile;
}

TEST(ParseOptions, DaemonizeLeavesAnAbsolutePathAloneAndAForegroundRunUntouched) {
    {
        Parse parse({"-z", "-o", "null", "-P", "/run/sendspin-cli.pid"});
        ASSERT_TRUE(parse.ok()) << parse.diagnostics();
        EXPECT_EQ(parse.options().pidfile, "/run/sendspin-cli.pid");
    }
    {
        // Nothing chdir()s in the foreground, so a relative path there means what it says and
        // is left exactly as typed.
        Parse parse({"-o", "null", "-P", "sendspin.pid", "-f", "sendspin.log"});
        ASSERT_TRUE(parse.ok()) << parse.diagnostics();
        EXPECT_EQ(parse.options().pidfile, "sendspin.pid");
        EXPECT_EQ(parse.options().logfile, "sendspin.log");
    }
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

TEST(ParseOptions, AFirstWordThatIsNotASubcommandIsRejectedAsOne) {
    // argv[1] is the subcommand position now, so a bare word there is diagnosed as a subcommand
    // typo -- which is what it almost always is -- rather than as an unexpected argument.
    Parse parse({"extra"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("error: unknown subcommand 'extra'"), std::string::npos)
        << parse.diagnostics();
    // Naming the alternatives is what makes it actionable rather than merely correct.
    EXPECT_NE(parse.diagnostics().find("pause"), std::string::npos) << parse.diagnostics();
}

TEST(ParseOptions, APositionalArgumentAfterFlagsIsRejected) {
    // Past argv[1] there is no subcommand position, so this really is a stray word.
    Parse parse({"-o", "null", "extra"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("error: unexpected argument 'extra'"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, ASubcommandAfterFlagsSaysToMoveIt) {
    // A real subcommand, just not where it can be read as one -- the fix is to move one word,
    // so the message says that instead of calling it junk.
    Parse parse({"--port", "9000", "status"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("has to come first"), std::string::npos)
        << parse.diagnostics();
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
    EXPECT_NE(parse.diagnostics().find("option '-o' needs a value"), std::string::npos);
}

TEST(ParseOptions, MissingValueNamesTheOptionNotTheCluster) {
    // `-lo` is -l followed by -o, and it is -o that is short a value. Naming the argv word
    // would report "-lo", which is not an option anyone typed.
    Parse cluster({"-lo"});
    EXPECT_FALSE(cluster.ok());
    EXPECT_NE(cluster.diagnostics().find("option '-o' needs a value"), std::string::npos)
        << cluster.diagnostics();

    // A long option has no cluster to disambiguate, and its getopt `val` is deliberately
    // outside the char range, so there the argv word is the only usable name.
    Parse long_form({"--port"});
    EXPECT_FALSE(long_form.ok());
    EXPECT_NE(long_form.diagnostics().find("option '--port' needs a value"), std::string::npos)
        << long_form.diagnostics();
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
        "ws://",         // a scheme naming no server
        "wss://",        // ditto
    };

    for (const char* input : cases) {
        std::string url;
        std::string error;

        EXPECT_FALSE(parse_server_url(input, url, error)) << "accepted '" << input << "'";
        EXPECT_FALSE(error.empty()) << "no reason given for '" << input << "'";
    }
}

// ---------------------------------------------------------------------------
// -s mdns:, the discovery form
// ---------------------------------------------------------------------------

TEST(ParseDiscoverySpec, RecognisesBothDiscoveryForms) {
    std::string name = "stale";
    ASSERT_TRUE(parse_discovery_spec("mdns:", name));
    EXPECT_TRUE(name.empty());

    ASSERT_TRUE(parse_discovery_spec("mdns:Living room", name));
    EXPECT_EQ(name, "Living room");
}

TEST(ParseDiscoverySpec, SplitsOnTheFirstColonOnly) {
    // Everything after the prefix is the name, colons and all -- a server may well be
    // called something with one in it.
    std::string name;
    ASSERT_TRUE(parse_discovery_spec("mdns:a:b", name));
    EXPECT_EQ(name, "a:b");
}

TEST(ParseDiscoverySpec, LeavesEveryOtherFormAlone) {
    // The reservation is the exact `mdns:` prefix. A host whose name merely starts with
    // those letters, or a bare `mdns`, is still an address.
    const char* addresses[] = {
        "hifi:8927", "mdns", "mdnsx:8927", "192.168.1.10", "ws://mdns:8927/sendspin", "",
    };

    for (const char* input : addresses) {
        std::string name;
        EXPECT_FALSE(parse_discovery_spec(input, name)) << "claimed '" << input << "'";
    }
}

TEST(ParseOptions, DiscoveryReachesTheOptions) {
    Parse parse({"-s", "mdns:Living room"});

#ifdef SENDSPIN_CLI_HAVE_MDNS
    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().discover);
    EXPECT_EQ(parse.options().discover_name, "Living room");
    // There is no URL until a server has actually been found.
    EXPECT_TRUE(parse.options().server_url.empty());
#else
    // A build with no mDNS refuses at parse time rather than discovering nothing forever.
    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("error:"), std::string::npos);
    EXPECT_NE(parse.diagnostics().find("mDNS"), std::string::npos) << parse.diagnostics();
#endif
}

TEST(ParseOptions, DiscoveryWithNoNameFilter) {
    Parse parse({"-s", "mdns:"});

#ifdef SENDSPIN_CLI_HAVE_MDNS
    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().discover);
    EXPECT_TRUE(parse.options().discover_name.empty());
#else
    EXPECT_FALSE(parse.ok());
#endif
}

TEST(ParseOptions, AHostWithAColonIsStillAHost) {
    // The regression the reserved prefix has to not cause: `hifi:8927` was a host and a
    // port before this flag existed, and still is.
    Parse parse({"-s", "hifi:8927"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_FALSE(parse.options().discover);
    EXPECT_EQ(parse.options().server_url, "ws://hifi:8927/sendspin");
}

TEST(ParseOptions, ABareMdnsIsStillAHost) {
    Parse parse({"-s", "mdns"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_FALSE(parse.options().discover);
    EXPECT_EQ(parse.options().server_url, "ws://mdns:8927/sendspin");
}

// ---------------------------------------------------------------------------
// The two connection modes are exclusive
// ---------------------------------------------------------------------------

TEST(ParseOptions, AdvertisesByDefault) {
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().advertises());
}

TEST(ParseOptions, AnyServerSuppressesTheAdvertisement) {
    // The spec's rule, so it holds for every -s form there is -- there is deliberately no
    // flag that turns the advertisement back on alongside one.
    const char* servers[] = {"192.168.1.10", "host:9000", "ws://host:9000/sendspin", "[::1]"};

    for (const char* server : servers) {
        Parse parse({"-s", server});
        ASSERT_TRUE(parse.ok()) << server << ": " << parse.diagnostics();
        EXPECT_FALSE(parse.options().advertises()) << server;
    }
}

TEST(ParseOptions, DiscoverySuppressesTheAdvertisementToo) {
    Parse parse({"-s", "mdns:"});

#ifdef SENDSPIN_CLI_HAVE_MDNS
    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
#endif
    EXPECT_FALSE(parse.options().advertises());
}

TEST(ParseOptions, NoMdnsSuppressesTheAdvertisementWithoutAServer) {
    Parse parse({"--no-mdns"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().no_mdns);
    EXPECT_FALSE(parse.options().advertises());
}

// ---------------------------------------------------------------------------
// --mdns-name
// ---------------------------------------------------------------------------

TEST(ParseOptions, MdnsNameDefaultsToTheFriendlyName) {
    Parse parse({"-n", "kitchen"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().mdns_name, "kitchen");
    EXPECT_FALSE(parse.options().was_given(Opt::MdnsName));
}

TEST(ParseOptions, MdnsNameFallsAllTheWayBackToTheHostname) {
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_FALSE(parse.options().mdns_name.empty());
    EXPECT_EQ(parse.options().mdns_name, parse.options().name);
}

TEST(ParseOptions, MdnsNameOverridesTheFriendlyName) {
    Parse parse({"-n", "kitchen", "--mdns-name", "Kitchen Player"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().name, "kitchen");
    EXPECT_EQ(parse.options().mdns_name, "Kitchen Player");
}

TEST(ParseOptions, MdnsNameNeedsAValue) {
    Parse parse({"--mdns-name", ""});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("--mdns-name needs a non-empty value"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, MdnsNameWithAServerWarnsButStillStarts) {
    // Inert rather than contradictory: the name names an advertisement that -s has already
    // ruled out, so refusing to start would be a worse trade than saying so.
    Parse parse({"-s", "192.168.1.10", "--mdns-name", "Kitchen"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("warning:"), std::string::npos) << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("--mdns-name is unused with -s"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, MdnsNameAloneDoesNotWarn) {
    Parse parse({"--mdns-name", "Kitchen"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.diagnostics().find("warning:"), std::string::npos) << parse.diagnostics();
}

// ---------------------------------------------------------------------------
// The control socket, and subcommands, through the parser
// ---------------------------------------------------------------------------

TEST(ParseOptions, TheControlSocketDefaultsUnderTheRuntimeDirectory) {
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, "/run/user/1000/sendspin-cli-8928.sock");
    EXPECT_TRUE(parse.options().control_absent_reason.empty());
}

TEST(ParseOptions, TheControlSocketDefaultFollowsThePort) {
    // The whole reason the port is in the leaf: two players on one host, and a subcommand able
    // to derive the same path from the same --port.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"--port", "9000"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, "/run/user/1000/sendspin-cli-9000.sock");
}

TEST(ParseOptions, ThePortIsReadBeforeTheDefaultPathIsBuiltWhicheverOrderItComesIn) {
    // The derivation happens after the whole line has parsed, so --port cannot arrive too late
    // to move the socket.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse before({"--port", "9000", "-o", "null"});
    Parse after({"-o", "null", "--port", "9000"});

    ASSERT_TRUE(before.ok()) << before.diagnostics();
    ASSERT_TRUE(after.ok()) << after.diagnostics();
    EXPECT_EQ(before.options().control_socket, after.options().control_socket);
    EXPECT_EQ(after.options().control_socket, "/run/user/1000/sendspin-cli-9000.sock");
}

TEST(ParseOptions, NoRuntimeDirectoryFallsBackToThePlatformDirectory) {
    // Two correct outcomes, and which one applies is a property of the platform -- so it is read
    // off the same function the parser uses rather than decided by an #ifdef here. On macOS,
    // where launchd sets no $XDG_RUNTIME_DIR at all, the fallback is what makes the default path
    // resolve; elsewhere an unset variable is a real absence.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", nullptr);
    Parse parse({});
    ASSERT_TRUE(parse.ok()) << parse.diagnostics();

    const std::string platform = control_platform_runtime_dir();
    if (platform.empty()) {
        // Non-fatal on purpose: a player with no control channel is still a player.
        EXPECT_TRUE(parse.options().control_socket.empty());
        EXPECT_FALSE(parse.options().control_absent_reason.empty());
        EXPECT_NE(parse.options().control_absent_reason.find("--control-socket"),
                  std::string::npos);
    } else {
        EXPECT_EQ(parse.options().control_socket, platform + "/sendspin-cli-8928.sock");
        EXPECT_TRUE(parse.options().control_absent_reason.empty());
    }

    // Either way, and this is the part that must hold on every platform: never /tmp. A
    // world-writable directory would let any local account drive the player.
    EXPECT_NE(parse.options().control_socket.compare(0, 5, "/tmp/"), 0)
        << parse.options().control_socket;
    EXPECT_EQ(parse.options().control_absent_reason.find("/tmp"), std::string::npos);
}

TEST(ParseOptions, AnEmptyRuntimeDirectoryCountsAsUnset) {
    // The XDG rule, and what last_server_path() already does with $XDG_STATE_HOME: the
    // alternative resolves to a path starting at the filesystem root. Asserted as equivalence to
    // the unset case rather than against a fixed outcome, so it holds on a platform with a
    // fallback and on one without.
    std::string unset_path;
    {
        ScopedEnv runtime_dir("XDG_RUNTIME_DIR", nullptr);
        Parse parse({});
        ASSERT_TRUE(parse.ok()) << parse.diagnostics();
        unset_path = parse.options().control_socket;
    }

    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "");
    Parse parse({});
    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, unset_path);
}

TEST(ParseOptions, AnExplicitRuntimeDirectoryWinsOverThePlatformFallback) {
    // The order matters and is not incidental: $XDG_RUNTIME_DIR is the user saying where their
    // runtime files go, so a platform default must never override it.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, "/run/user/1000/sendspin-cli-8928.sock");
}

TEST(ParseOptions, ControlSocketOverridesTheDefault) {
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"--control-socket", "/tmp/mine.sock"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, "/tmp/mine.sock");
    EXPECT_TRUE(parse.options().was_given(Opt::ControlSocket));
}

TEST(ParseOptions, ControlSocketNeedsANonEmptyValue) {
    Parse parse({"--control-socket", ""});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("--control-socket needs a non-empty value"),
              std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, AnOverLongControlSocketIsRefusedRatherThanTruncated) {
    // A truncated path binds a socket nothing can find, and every subcommand would then report
    // "no daemon" against a daemon that is running.
    const std::string too_long = "/" + std::string(control_socket_path_limit(), 'x');
    Parse parse({"--control-socket", too_long});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("--control-socket"), std::string::npos)
        << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find(std::to_string(control_socket_path_limit() - 1)),
              std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, AControlSocketAtTheLimitIsAccepted) {
    const std::string exact = "/" + std::string(control_socket_path_limit() - 2, 'x');
    Parse parse({"--control-socket", exact});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, exact);
}

TEST(ParseOptions, NoControlLeavesNoSocketAndNoReason) {
    // --no-control needs no explaining, unlike a missing $XDG_RUNTIME_DIR: the flag is the
    // reason, and main() logs it at info rather than warning about it.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"--no-control"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().no_control);
    EXPECT_TRUE(parse.options().control_socket.empty());
    EXPECT_TRUE(parse.options().control_absent_reason.empty());
}

TEST(ParseOptions, NoControlWithAControlSocketIsRefused) {
    // Contradictory rather than inert, like -z with -o stdout: one names where the socket goes
    // and the other says there is not one.
    Parse parse({"--no-control", "--control-socket", "/tmp/mine.sock"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("contradict"), std::string::npos) << parse.diagnostics();
}

TEST(ParseOptions, ARelativeControlSocketIsMadeAbsoluteUnderZ) {
    // The same split -P and -f have: the daemon chdir()s to /, so a relative path would name
    // one file before the fork and a different one after it.
    Parse parse({"-z", "-f", "/tmp/log", "--control-socket", "mine.sock"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket.front(), '/');
    EXPECT_NE(parse.options().control_socket.find("/mine.sock"), std::string::npos)
        << parse.options().control_socket;
}

TEST(ParseOptions, ARelativeControlSocketIsLeftAloneWithoutZ) {
    Parse parse({"--control-socket", "mine.sock"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, "mine.sock");
}

TEST(ParseOptions, ASubcommandIsReportedThroughOptions) {
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"vol", "50"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().subcommand, "vol");
    ASSERT_EQ(parse.options().subcommand_args.size(), 1U);
    EXPECT_EQ(parse.options().subcommand_args[0], "50");
    // It resolves the same socket a daemon on the same --port would bind.
    EXPECT_EQ(parse.options().control_socket, "/run/user/1000/sendspin-cli-8928.sock");
}

TEST(ParseOptions, FlagsAfterASubcommandAreStillParsed) {
    // The portability trap the pre-getopt split exists to close: glibc permutes a positional
    // argument out of the way and the BSDs stop at it, so relying on getopt's leftovers would
    // read --port on Linux and drop it on macOS.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"vol", "50", "--port", "9000"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().subcommand, "vol");
    EXPECT_EQ(parse.options().port, 9000);
    EXPECT_EQ(parse.options().control_socket, "/run/user/1000/sendspin-cli-9000.sock");
}

TEST(ParseOptions, ANegativeSubcommandArgumentIsNotReadAsFlags) {
    Parse parse({"seek-rel", "-5000", "--control-socket", "/tmp/mine.sock"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().subcommand, "seek-rel");
    ASSERT_EQ(parse.options().subcommand_args.size(), 1U);
    EXPECT_EQ(parse.options().subcommand_args[0], "-5000");
    EXPECT_EQ(parse.options().control_socket, "/tmp/mine.sock");
}

TEST(ParseOptions, ASubcommandArgumentIsValidatedAtParseTime) {
    // So a bad `vol 500` reads exactly like a bad --buffer-ms: one error: line at the terminal,
    // before a socket has been opened, rather than a failure out on the wire.
    Parse parse({"vol", "500"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("0 to 100"), std::string::npos) << parse.diagnostics();
}

TEST(ParseOptions, HelpWinsOverABadSubcommandArgument) {
    Parse parse({"vol", "500", "--help"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().show_help);
}

TEST(ParseOptions, DaemonFlagsAlongsideASubcommandWarnRatherThanFail) {
    // The natural mistake is pasting a daemon's whole flag line and appending a subcommand,
    // which should still work -- just without pretending the flags did anything.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"pause", "-o", "null"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("only --port and --control-socket"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, ASubcommandDoesNotDrawTheDaemonWarnings) {
    // -z without -f warns about a daemon's discarded log. A subcommand starts no daemon, so
    // that warning would be describing something that is not going to happen.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"pause", "-z"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.diagnostics().find("discards all log output"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, ADaemonRunHasNoSubcommand) {
    Parse parse({"-o", "null"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().subcommand.empty());
    EXPECT_TRUE(parse.options().subcommand_args.empty());
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
                 "debug", "-f", "/var/log/x.log", "--port", "9000", "--buffer-ms", "200"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    for (const Opt opt : {Opt::Device, Opt::ListDevices, Opt::Name, Opt::Server, Opt::Daemonize,
                          Opt::Pidfile, Opt::Logfile, Opt::LogLevel, Opt::Port, Opt::BufferMs}) {
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
