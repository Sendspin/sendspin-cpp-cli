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

/// parse_options() and parse_server_url(): what the flag surface accepts and rejects.

#include "cli.h"

#include "control.h"
#include "log.h"
#include "parse_harness.h"
#include "scoped_env.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace sendspin_cli {
namespace {

using sendspin::LogLevel;

// Every flag reaches its field

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

// --help / --version short-circuit

TEST(ParseOptions, HelpWinsOverAnInvalidFlagAfterIt) {
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
    // Appending --help to any wrong line must still print the flag list.
    const std::vector<std::string> bad_prefixes[] = {
        {"--port", "0"},     // validated inline, as -s is not
        {"-o", ""},          // an empty value
        {"-d", "nonsense"},  // an unknown log level
        {"-Q"},              // an unknown flag entirely
        {"-s", "::1"},       // an address, refused after the loop
        {"extra"},           // a positional argument
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
    // Here --help is -n's value, not a flag.
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

// Empty values

TEST(ParseOptions, EmptyValuesAreRejected) {
    for (const char* flag : {"-o", "-n", "-P", "-f"}) {
        Parse parse({flag, ""});

        EXPECT_FALSE(parse.ok()) << flag << " accepted an empty value";
        EXPECT_NE(parse.diagnostics().find("error:"), std::string::npos) << flag;
        EXPECT_NE(parse.diagnostics().find(flag), std::string::npos) << flag;
    }
}

// Identity: --id, --manufacturer, --product-name

TEST(ParseOptions, IdentityFlagsSetTheirFields) {
    Parse parse({"--id", "kitchen-left", "--manufacturer", "Acme Audio", "--product-name",
                 "Acme Streamer"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().client_id, "kitchen-left");
    EXPECT_EQ(parse.options().manufacturer, "Acme Audio");
    EXPECT_EQ(parse.options().product_name, "Acme Streamer");
}

TEST(ParseOptions, IdentityDefaultsSayWhatThisReallyIs) {
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    // Empty, so the library derives the MAC-based id.
    EXPECT_TRUE(parse.options().client_id.empty());
    EXPECT_EQ(parse.options().manufacturer, "sendspin-cpp-cli");
    EXPECT_EQ(parse.options().product_name, "sendspin-cli");
}

TEST(ParseOptions, IdentityFlagsRejectEmptyValues) {
    for (const char* flag : {"--id", "--manufacturer", "--product-name"}) {
        Parse parse({flag, ""});

        EXPECT_FALSE(parse.ok()) << flag << " accepted an empty value";
        EXPECT_NE(parse.diagnostics().find("error:"), std::string::npos) << flag;
        EXPECT_NE(parse.diagnostics().find(flag), std::string::npos) << flag;
    }
}

// --audio-format

TEST(ParseOptions, AudioFormatIsParsedAtTheFlag) {
    Parse parse({"--audio-format", "flac:48000:24:2"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    ASSERT_EQ(parse.options().audio_formats.size(), 1U);
    const sendspin::AudioSupportedFormatObject& format = parse.options().audio_formats.front();
    EXPECT_EQ(format.codec, sendspin::SendspinCodecFormat::FLAC);
    EXPECT_EQ(format.sample_rate, 48000U);
    EXPECT_EQ(format.bit_depth, 24);
    EXPECT_EQ(format.channels, 2);
}

TEST(ParseOptions, AudioFormatTakesAnOrderedList) {
    Parse parse({"--audio-format", "flac:48000:24:2,pcm:48000:24:2,opus:48000:16:2"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    const std::vector<sendspin::AudioSupportedFormatObject>& formats =
        parse.options().audio_formats;
    ASSERT_EQ(formats.size(), 3U);
    EXPECT_EQ(formats[0].codec, sendspin::SendspinCodecFormat::FLAC);
    EXPECT_EQ(formats[1].codec, sendspin::SendspinCodecFormat::PCM);
    EXPECT_EQ(formats[2].codec, sendspin::SendspinCodecFormat::OPUS);
}

TEST(ParseOptions, ARepeatedAudioFormatReplacesTheListRatherThanExtendingIt) {
    Parse parse(
        {"--audio-format", "flac:48000:24:2,pcm:48000:24:2", "--audio-format", "pcm:44100:16:2"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    ASSERT_EQ(parse.options().audio_formats.size(), 1U);
    EXPECT_EQ(parse.options().audio_formats.front().sample_rate, 44100U);
}

TEST(ParseOptions, AudioFormatDefaultsToNoPin) {
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().audio_formats.empty());
}

TEST(ParseOptions, ABadAudioFormatIsRefusedWithTheShapeToCopy) {
    Parse parse({"--audio-format", "flac:48000"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("error: invalid --audio-format"), std::string::npos)
        << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("codec:rate:depth:channels"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, ABadAudioFormatListIsRefusedNamingTheEntry) {
    const std::pair<const char*, const char*> cases[] = {
        {"flac:48000:24:2,", "entry 2 is empty"},
        {",flac:48000:24:2", "entry 1 is empty"},
        {"flac:48000:24:2,,pcm:48000:16:2", "entry 2 is empty"},
        {"flac:48000:24:2,flac:48000:24:2", "'flac:48000:24:2' is listed more than once"},
        {"flac:48000:24:2,pcm:48000:20:2", "'pcm:48000:20:2'"},
    };
    for (const auto& [value, named] : cases) {
        Parse parse({"--audio-format", value});

        EXPECT_FALSE(parse.ok()) << value;
        EXPECT_NE(parse.diagnostics().find("error: invalid --audio-format"), std::string::npos)
            << value << ": " << parse.diagnostics();
        EXPECT_NE(parse.diagnostics().find(named), std::string::npos)
            << value << ": " << parse.diagnostics();
    }
}

// --port

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

// --buffer-ms

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

// --static-delay

TEST(ParseOptions, StaticDelayIsAccepted) {
    Parse parse({"--static-delay", "250"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().static_delay_ms, 250U);
    EXPECT_TRUE(parse.options().was_given(Opt::StaticDelay));
}

TEST(ParseOptions, StaticDelayDefaultsToNoDelay) {
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().static_delay_ms, 0U);
    EXPECT_FALSE(parse.options().was_given(Opt::StaticDelay));
}

TEST(ParseOptions, StaticDelayEdgesAreAccepted) {
    // Zero is legal: it turns the delay off.
    Parse low({"--static-delay", "0"});
    Parse high({"--static-delay", std::to_string(MAX_STATIC_DELAY_MS)});

    ASSERT_TRUE(low.ok()) << low.diagnostics();
    ASSERT_TRUE(high.ok()) << high.diagnostics();
    EXPECT_EQ(low.options().static_delay_ms, 0U);
    EXPECT_EQ(high.options().static_delay_ms, MAX_STATIC_DELAY_MS);
}

TEST(ParseOptions, StaticDelayBounds) {
    // Refused, not clamped: the library would silently clamp.
    for (const char* value :
         {"5001", "9000", "65536", "abc", "12x", "", "-1", " 250", "+250", "250.5"}) {
        Parse parse({"--static-delay", value});

        EXPECT_FALSE(parse.ok()) << "--static-delay accepted '" << value << "'";
        const std::string diagnostics = parse.diagnostics();
        EXPECT_NE(diagnostics.find("error: invalid --static-delay"), std::string::npos) << value;
        EXPECT_NE(diagnostics.find(std::string("'") + value + "'"), std::string::npos) << value;
        EXPECT_NE(diagnostics.find("0-" + std::to_string(MAX_STATIC_DELAY_MS)), std::string::npos)
            << value;
    }
}

TEST(ParseOptions, StaticDelayNeedsAValue) {
    Parse parse({"--static-delay"});

    ASSERT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("option '--static-delay' needs a value"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, StaticDelayIsListedByHelpAsAFirstRunDefault) {
    // --help must say a remembered delay beats the flag.
    std::FILE* out = std::tmpfile();
    ASSERT_NE(out, nullptr);
    print_usage(out, "sendspin-cli");
    std::rewind(out);
    std::string text;
    char buffer[512];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), out)) > 0) {
        text.append(buffer, read);
    }
    std::fclose(out);

    EXPECT_NE(text.find("--static-delay"), std::string::npos);
    EXPECT_NE(text.find("FIRST-RUN DEFAULT"), std::string::npos)
        << "the precedence is not in --help";
    // Uses the constant, so a changed bound without a --help update fails.
    EXPECT_NE(text.find("0-" + std::to_string(MAX_STATIC_DELAY_MS)), std::string::npos) << text;
    // The direction: audio is handed over earlier.
    EXPECT_NE(text.find("EARLIER"), std::string::npos)
        << "--help does not say which way the delay goes";
}

TEST(ParseOptions, BufferMsDoesNotClaimDashA) {
    // -a is deliberately unclaimed.
    Parse parse({"-a", "100"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("unknown option '-a'"), std::string::npos)
        << parse.diagnostics();
}

// -d

TEST(ParseOptions, LogLevelNames) {
    const std::pair<const char*, LogLevel> cases[] = {
        {"none", LogLevel::NONE},      {"off", LogLevel::NONE},    {"error", LogLevel::ERROR},
        {"err", LogLevel::ERROR},      {"warn", LogLevel::WARN},   {"warning", LogLevel::WARN},
        {"info", LogLevel::INFO},      {"debug", LogLevel::DEBUG}, {"verbose", LogLevel::VERBOSE},
        {"sdebug", LogLevel::VERBOSE},
    };

    for (const auto& [name, level] : cases) {
        Parse parse({"-d", name});

        ASSERT_TRUE(parse.ok()) << "-d " << name << ": " << parse.diagnostics();
        EXPECT_EQ(parse.options().log_level, level) << name;
    }
}

TEST(ParseOptions, LogCategoryIsAcceptedAndWarnedAboutWithSomethingToDoInstead) {
    // The category is parsed, ignored and warned about.
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

// -z

TEST(ParseOptions, DaemonizeRefusesToWritePcmToStdout) {
    // A detached daemon's stdout is /dev/null; both spellings of the stdout sink.
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
    // Without -z, -o stdout is fine.
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
    // -z chdir()s to /, so relative paths are made absolute.
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
        // In the foreground, relative paths stay as typed.
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

// Malformed command lines

TEST(ParseOptions, AFirstWordThatIsNotASubcommandIsRejectedAsOne) {
    // A bare first word is diagnosed as a subcommand typo.
    Parse parse({"extra"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("error: unknown subcommand 'extra'"), std::string::npos)
        << parse.diagnostics();
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
    // The cluster's option letter is named, not the argv word.
    Parse cluster({"-lo"});
    EXPECT_FALSE(cluster.ok());
    EXPECT_NE(cluster.diagnostics().find("option '-o' needs a value"), std::string::npos)
        << cluster.diagnostics();

    // A long option is named by its argv word.
    Parse long_form({"--port"});
    EXPECT_FALSE(long_form.ok());
    EXPECT_NE(long_form.diagnostics().find("option '--port' needs a value"), std::string::npos)
        << long_form.diagnostics();
}

// -s, through the parser

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

// -s, as a matrix over parse_server_url()

TEST(ParseServerUrl, Accepted) {
    const std::pair<const char*, const char*> cases[] = {
        // A bare host takes the server's port (8927), not this player's serve port (8928).
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

// redact_url_userinfo(): what a logged server URL is allowed to say

TEST(RedactUrlUserinfo, MasksTheSecretAndKeepsTheRest) {
    const std::pair<const char*, const char*> cases[] = {
        // A user:password pair keeps its username: the line names which endpoint was dialled.
        {"ws://alice:s3cr3t@host:8927/sendspin", "ws://alice:***@host:8927/sendspin"},
        {"wss://alice:s3cr3t@host/sendspin", "wss://alice:***@host/sendspin"},
        // One field with no colon could be a bearer token, so the whole of it goes.
        {"ws://s3cr3t@host:8927/sendspin", "ws://***@host:8927/sendspin"},
        // A password may hold colons, so the split is on the *first* one.
        {"ws://alice:s3c:r3t@host/sendspin", "ws://alice:***@host/sendspin"},
        // The authority splits on the *last* '@', a host being unable to contain one.
        {"ws://alice:s3c@r3t@host/sendspin", "ws://alice:***@host/sendspin"},
        // A bracketed IPv6 host keeps its brackets and its own colons.
        {"ws://alice:s3cr3t@[2001:db8::1]:8927/sendspin",
         "ws://alice:***@[2001:db8::1]:8927/sendspin"},
        {"ws://s3cr3t@[::1]:8927/sendspin", "ws://***@[::1]:8927/sendspin"},
        // No username is still a secret to hide.
        {"ws://:s3cr3t@host/sendspin", "ws://:***@host/sendspin"},
        // A rejected -s value never had a scheme, so a bare authority is read as one.
        {"alice:s3cr3t@host", "alice:***@host"},
        {"s3cr3t@host", "***@host"},
    };

    for (const auto& [input, expected] : cases) {
        EXPECT_EQ(redact_url_userinfo(input), expected) << input;
        EXPECT_EQ(redact_url_userinfo(input).find("s3cr3t"), std::string::npos)
            << "the secret survived in '" << input << "'";
    }
}

TEST(RedactUrlUserinfo, LeavesAloneWhatHoldsNoSecret) {
    const char* unchanged[] = {
        // Nothing to hide.
        "",
        "ws://host:8927/sendspin",
        "wss://[2001:db8::1]:8927/sendspin",
        "host:8927",
        "mdns:Living room",
        // A scheme naming nothing, and other fragments a rejected -s value arrives as.
        "ws://",
        "::1",
        "[::1",
        // An empty userinfo, and an empty password: masking either would invent a secret.
        "ws://@host:8927/sendspin",
        "ws://alice:@host:8927/sendspin",
        // The authority ends at the first '/', '?' or '#', so an '@' past it is not a separator.
        "ws://host:8927/sendspin@1",
        "ws://host:8927//alice:s3cr3t@evil/x",
        "ws://host:8927/sendspin?token=a@b",
        "ws://host:8927/sendspin#a@b",
        // An unencoded '/', '?' or '#' inside userinfo ends the authority early, so the value
        // comes back whole -- a malformed URL RFC 3986 requires percent-encoded, not masked.
        "ws://alice:aGVsbG8/d29ybGQ=@host:8927/sendspin",
    };

    for (const char* input : unchanged) {
        EXPECT_EQ(redact_url_userinfo(input), input);
    }
}

// A value that does not parse is the one most likely mistyped around a password, so assert over
// the reason rather than per message, covering a rejection added later without listing it here.
TEST(RedactUrlUserinfo, NoRejectionReasonQuotesACredential) {
    const char* cases[] = {
        "alice:s3cr3t@host",                  // userinfo lands in the port field
        "alice:s3cr3t@2001:db8::1",           // ...and in the bracket-it-yourself advice
        "http://alice:s3cr3t@host/sendspin",  // the wrong scheme
        "ws://",                              // a scheme and nothing else
        "[::1]alice:s3cr3t@host",             // a fragment after the closing bracket
        "[::1]:s3cr3t@host",                  // ...and one that reads as a port
        ":s3cr3t@host",                       // no host before the port
    };

    for (const char* input : cases) {
        std::string url;
        std::string error;

        ASSERT_FALSE(parse_server_url(input, url, error)) << "accepted '" << input << "'";
        ASSERT_FALSE(error.empty()) << "no reason given for '" << input << "'";
        EXPECT_EQ(error.find("s3cr3t"), std::string::npos)
            << "'" << input << "' was refused with: " << error;
    }
}

// -s mdns:, the discovery form

TEST(ParseDiscoverySpec, RecognisesBothDiscoveryForms) {
    std::string name = "stale";
    ASSERT_TRUE(parse_discovery_spec("mdns:", name));
    EXPECT_TRUE(name.empty());

    ASSERT_TRUE(parse_discovery_spec("mdns:Living room", name));
    EXPECT_EQ(name, "Living room");
}

TEST(ParseDiscoverySpec, SplitsOnTheFirstColonOnly) {
    // Everything after the prefix is the name, colons included.
    std::string name;
    ASSERT_TRUE(parse_discovery_spec("mdns:a:b", name));
    EXPECT_EQ(name, "a:b");
}

TEST(ParseDiscoverySpec, LeavesEveryOtherFormAlone) {
    // Only the exact `mdns:` prefix is the discovery form; a bare `mdns` is still an address.
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
    // The reserved prefix must not regress this: `hifi:8927` is a host and a port.
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

// The two connection modes are exclusive

TEST(ParseOptions, AdvertisesByDefault) {
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().advertises());
}

TEST(ParseOptions, AnyServerSuppressesTheAdvertisement) {
    // The spec's rule, so it holds for every -s form -- there is deliberately no flag that turns
    // the advertisement back on alongside one.
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

// --mdns-name

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
    // Inert with -s, so warned rather than refused.
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

// The control socket, and subcommands, through the parser

TEST(ParseOptions, TheControlSocketDefaultsUnderTheRuntimeDirectory) {
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, "/run/user/1000/sendspin-cli-8928.sock");
    EXPECT_TRUE(parse.options().control_absent_reason.empty());
}

TEST(ParseOptions, TheControlSocketDefaultFollowsThePort) {
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"--port", "9000"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, "/run/user/1000/sendspin-cli-9000.sock");
}

TEST(ParseOptions, ThePortIsReadBeforeTheDefaultPathIsBuiltWhicheverOrderItComesIn) {
    // --port after the default path still moves the socket.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse before({"--port", "9000", "-o", "null"});
    Parse after({"-o", "null", "--port", "9000"});

    ASSERT_TRUE(before.ok()) << before.diagnostics();
    ASSERT_TRUE(after.ok()) << after.diagnostics();
    EXPECT_EQ(before.options().control_socket, after.options().control_socket);
    EXPECT_EQ(after.options().control_socket, "/run/user/1000/sendspin-cli-9000.sock");
}

TEST(ParseOptions, NoRuntimeDirectoryFallsBackToThePlatformDirectory) {
    // Platform-dependent, so read off the function the parser uses rather than an #ifdef.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", nullptr);
    Parse parse({});
    ASSERT_TRUE(parse.ok()) << parse.diagnostics();

    std::string rejection;
    const std::string platform = control_platform_runtime_dir(rejection);
#ifdef __APPLE__
    // On macOS the default path must resolve.
    ASSERT_FALSE(platform.empty()) << "rejected because: " << rejection;
#endif
    if (platform.empty()) {
        EXPECT_TRUE(parse.options().control_socket.empty());
        EXPECT_FALSE(parse.options().control_absent_reason.empty());
        EXPECT_NE(parse.options().control_absent_reason.find("--control-socket"),
                  std::string::npos);
    } else {
        EXPECT_EQ(parse.options().control_socket, platform + "/sendspin-cli-8928.sock");
        EXPECT_TRUE(parse.options().control_absent_reason.empty());
    }

    // Never /tmp.
    EXPECT_NE(parse.options().control_socket.compare(0, 5, "/tmp/"), 0)
        << parse.options().control_socket;
    EXPECT_EQ(parse.options().control_absent_reason.find("/tmp"), std::string::npos);
}

TEST(ParseOptions, AnEmptyRuntimeDirectoryCountsAsUnset) {
    // An empty variable must behave exactly like an unset one.
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

TEST(ParseOptions, ANonPrivateRuntimeDirectoryIsUsedButWarnedAbout) {
    // A non-private $XDG_RUNTIME_DIR is honoured but warned about.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/tmp");
    Parse parse({});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().control_socket, "/tmp/sendspin-cli-8928.sock");
    EXPECT_NE(parse.diagnostics().find("warning:"), std::string::npos) << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("not private to this user"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, ASubcommandDoesNotWarnAboutTheDirectoryItOnlyConnectsTo) {
    // Only the daemon warns; a subcommand merely connects.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/tmp");
    Parse parse({"status"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.diagnostics().find("not private to this user"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, AnExplicitRuntimeDirectoryWinsOverThePlatformFallback) {
    // $XDG_RUNTIME_DIR must beat any platform default.
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
    // --no-control leaves no absent-reason to explain.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"--no-control"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().no_control);
    EXPECT_TRUE(parse.options().control_socket.empty());
    EXPECT_TRUE(parse.options().control_absent_reason.empty());
}

TEST(ParseOptions, NoControlWithAControlSocketIsRefused) {
    Parse parse({"--no-control", "--control-socket", "/tmp/mine.sock"});

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("contradict"), std::string::npos) << parse.diagnostics();
}

TEST(ParseOptions, ARelativeControlSocketIsMadeAbsoluteUnderZ) {
    // Relative socket paths are made absolute under -z, like -P and -f.
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
    // Flags after the subcommand must parse on glibc and the BSDs alike.
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
    // A bad argument fails at parse time, before any socket.
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
    // Daemon flags with a subcommand warn, not fail.
    ScopedEnv runtime_dir("XDG_RUNTIME_DIR", "/run/user/1000");
    Parse parse({"pause", "-o", "null"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("only --port and --control-socket"), std::string::npos)
        << parse.diagnostics();
}

TEST(ParseOptions, ASubcommandDoesNotDrawTheDaemonWarnings) {
    // The -z-without-f warning describes a daemon that is not starting.
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

// Config-file precedence hooks

TEST(ParseOptions, TracksWhichOptionsWereExplicitlyGiven) {
    Parse parse({"-o", "null"});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().was_given(Opt::Device));
    // Parser defaults are not "given".
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
    // -o with the default's value still counts as given.
    Parse parse({"-o", DEFAULT_OUTPUT_DEVICE});

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().was_given(Opt::Device));
}

// getopt's global state

TEST(ParseOptions, ParsingTwiceInOneProcessGivesTheSameAnswers) {
    // getopt's scan position is process-global, so parse_options() must reset it.
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
    // A parse that bailed mid-line must not affect the next.
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
