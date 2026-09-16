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

/// Config search (via injected paths) and precedence (via parse_options() with --config).

#include "config_file.h"

#include "cli.h"
#include "control.h"
#include "parse_harness.h"
#include "scoped_env.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace sendspin_cli {
namespace {

using sendspin::LogLevel;

/// A scratch directory under the build tree, per test, with its config files removed afterwards.
class ScratchDir {
public:
    ScratchDir() {
        this->path_ =
            "config-test-" + std::to_string(getpid()) + "-" + std::to_string(ScratchDir::next_id());
        this->created_ = ::mkdir(this->path_.c_str(), 0700) == 0;
    }

    ~ScratchDir() {
        for (const std::string& written : this->written_) {
            std::remove(written.c_str());
        }
        // Deepest first: write() creates `sendspin-cli/` under whichever base it was handed.
        ::rmdir((this->path_ + "/sendspin-cli").c_str());
        ::rmdir((this->path_ + "/.config/sendspin-cli").c_str());
        ::rmdir((this->path_ + "/.config").c_str());
        ::rmdir(this->path_.c_str());
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    const std::string& path() const {
        return this->path_;
    }

    bool created() const {
        return this->created_;
    }

    /// Writes `text` to `leaf`, creating one parent level, and returns the path.
    std::string write(const std::string& leaf, const std::string& text) {
        const std::string path = this->path_ + "/" + leaf;
        const size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            const std::string parent = path.substr(0, slash);
            // One level at a time, so `.config/sendspin-cli/config` works too.
            const size_t inner = parent.find_last_of('/');
            if (inner != std::string::npos) {
                ::mkdir(parent.substr(0, inner).c_str(), 0700);
            }
            ::mkdir(parent.c_str(), 0700);
        }
        std::FILE* file = std::fopen(path.c_str(), "w");
        EXPECT_NE(file, nullptr) << path;
        if (file != nullptr) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
        this->written_.push_back(path);
        return path;
    }

private:
    static int next_id() {
        static int id = 0;
        return ++id;
    }

    std::string path_;
    std::vector<std::string> written_;
    bool created_{false};
};

// Where a config file is looked for

TEST(ConfigSearchPaths, RunsXdgThenHomeThenEtc) {
    const ScopedEnv xdg("XDG_CONFIG_HOME", "/xdg/config");
    const ScopedEnv home("HOME", "/home/someone");

    const std::vector<std::string> paths = config_search_paths();
    ASSERT_EQ(paths.size(), 3U);
    EXPECT_EQ(paths[0], "/xdg/config/sendspin-cli/config");
    EXPECT_EQ(paths[1], "/home/someone/.config/sendspin-cli/config");
    EXPECT_EQ(paths[2], SYSTEM_CONFIG_PATH);
}

TEST(ConfigSearchPaths, AnEmptyOrUnsetVariableContributesNothing) {
    const ScopedEnv xdg("XDG_CONFIG_HOME", "");
    const ScopedEnv home("HOME", nullptr);

    // An empty variable counts as unset.
    const std::vector<std::string> paths = config_search_paths();
    ASSERT_EQ(paths.size(), 1U);
    EXPECT_EQ(paths[0], SYSTEM_CONFIG_PATH);
}

TEST(LoadConfigFile, TakesTheFirstFileFoundAndStopsThere) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string first = scratch.write("first", "port = 9001\n");
    const std::string second = scratch.write("second", "port = 9002\n");

    ConfigFile config;
    std::string error;
    ASSERT_TRUE(load_config_file("", {first, second}, config, error)) << error;
    EXPECT_EQ(config.path, first);
    // The first file is used whole, with nothing merged over it.
    ASSERT_EQ(config.entries.size(), 1U);
    EXPECT_EQ(config.entries[0].value, "9001");
}

TEST(LoadConfigFile, SkipsAMissingLayerAndCarriesOn) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string real = scratch.write("real", "port = 9003\n");

    ConfigFile config;
    std::string error;
    ASSERT_TRUE(load_config_file("", {scratch.path() + "/absent", real}, config, error)) << error;
    EXPECT_EQ(config.path, real);
}

TEST(LoadConfigFile, FindingNothingIsSilentAndNormal) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    ConfigFile config;
    std::string error;
    EXPECT_TRUE(load_config_file("", {scratch.path() + "/absent"}, config, error)) << error;
    EXPECT_TRUE(config.path.empty());
    EXPECT_TRUE(config.entries.empty());
    EXPECT_TRUE(error.empty());
}

TEST(LoadConfigFile, ARequestedFileThatCannotBeReadIsFatalAndNamed) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());

    ConfigFile config;
    std::string error;
    // An absent named file is fatal, unlike an absent search layer.
    EXPECT_FALSE(load_config_file(scratch.path() + "/absent", {}, config, error));
    EXPECT_NE(error.find("--config"), std::string::npos) << error;
    EXPECT_NE(error.find(scratch.path() + "/absent"), std::string::npos) << error;
}

TEST(LoadConfigFile, ABrokenFileIsRefusedRatherThanSkipped) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string broken = scratch.write("broken", "port = 9000\nnonsense\n");
    const std::string below = scratch.write("below", "port = 9999\n");

    ConfigFile config;
    std::string error;
    EXPECT_FALSE(load_config_file("", {broken, below}, config, error));
    EXPECT_NE(error.find(broken + ":2:"), std::string::npos) << error;
}

// Precedence

TEST(ConfigPrecedence, AConfigValueBeatsTheBuiltInDefault) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write(
        "config", "# a player in the kitchen\nname = kitchen\nport = 9100\nbuffer-ms = 250\n"
                  "output = null\nmdns-name = Kitchen Speaker\nno-mdns = true\n"
                  "id = kitchen-left\nmanufacturer = Acme Audio\nproduct-name = Acme Streamer\n");

    Parse parse({}, config);

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().name, "kitchen");
    EXPECT_EQ(parse.options().port, 9100);
    EXPECT_EQ(parse.options().buffer_ms, 250);
    EXPECT_EQ(parse.options().device, "null");
    // A value with a space in it survives: only the key is trimmed and the first '=' splits.
    EXPECT_EQ(parse.options().mdns_name, "Kitchen Speaker");
    EXPECT_TRUE(parse.options().no_mdns);
    EXPECT_EQ(parse.options().client_id, "kitchen-left");
    EXPECT_EQ(parse.options().manufacturer, "Acme Audio");
    EXPECT_EQ(parse.options().product_name, "Acme Streamer");
    EXPECT_EQ(parse.options().config_path, config);
}

TEST(ConfigPrecedence, TheCommandLineBeatsAConfigValue) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config =
        scratch.write("config", "name = kitchen\nport = 9100\nbuffer-ms = 250\n");

    Parse parse({"-n", "bathroom", "--port", "9200"}, config);

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().name, "bathroom");
    EXPECT_EQ(parse.options().port, 9200);
    // Precedence is per option, not per file.
    EXPECT_EQ(parse.options().buffer_ms, 250);
}

TEST(ConfigPrecedence, StaticDelayIsSettableAndLosesToTheCommandLine) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "static-delay = 375\n");

    Parse from_file({}, config);
    ASSERT_TRUE(from_file.ok()) << from_file.diagnostics();
    EXPECT_EQ(from_file.options().static_delay_ms, 375U);

    Parse overridden({"--static-delay", "120"}, config);
    ASSERT_TRUE(overridden.ok()) << overridden.diagnostics();
    EXPECT_EQ(overridden.options().static_delay_ms, 120U);
}

TEST(ConfigPrecedence, AudioFormatTakesTheSameOrderedListAsTheFlag) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config =
        scratch.write("config", "audio-format = pcm:44100:16:2,flac:48000:24:2\n");

    Parse from_file({}, config);
    ASSERT_TRUE(from_file.ok()) << from_file.diagnostics();
    const std::vector<sendspin::AudioSupportedFormatObject>& formats =
        from_file.options().audio_formats;
    ASSERT_EQ(formats.size(), 2U);
    EXPECT_EQ(formats[0].codec, sendspin::SendspinCodecFormat::PCM);
    EXPECT_EQ(formats[1].codec, sendspin::SendspinCodecFormat::FLAC);

    // One value, like every other key: the command line replaces the list whole.
    Parse overridden({"--audio-format", "opus:48000:16:2"}, config);
    ASSERT_TRUE(overridden.ok()) << overridden.diagnostics();
    ASSERT_EQ(overridden.options().audio_formats.size(), 1U);
    EXPECT_EQ(overridden.options().audio_formats.front().codec,
              sendspin::SendspinCodecFormat::OPUS);
}

TEST(ConfigPrecedence, ABooleanFlagOnTheCommandLineBeatsAFalseInTheFile) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "no-mdns = false\n");

    Parse parse({"--no-mdns"}, config);

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().no_mdns);
}

TEST(ConfigPrecedence, LastWinsWithinOneFile) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "port = 9100\nport = 9300\n");

    Parse parse({}, config);

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().port, 9300);
}

TEST(ConfigPrecedence, AcceptsEveryBooleanSpelling) {
    for (const char* yes : {"true", "yes", "on", "1"}) {
        ScratchDir scratch;
        ASSERT_TRUE(scratch.created());
        const std::string config =
            scratch.write("config", "no-control = " + std::string(yes) + "\n");
        Parse parse({}, config);
        ASSERT_TRUE(parse.ok()) << yes << ": " << parse.diagnostics();
        EXPECT_TRUE(parse.options().no_control) << yes;
    }
    for (const char* no : {"false", "no", "off", "0"}) {
        ScratchDir scratch;
        ASSERT_TRUE(scratch.created());
        const std::string config = scratch.write("config", "no-mdns = " + std::string(no) + "\n");
        Parse parse({}, config);
        ASSERT_TRUE(parse.ok()) << no << ": " << parse.diagnostics();
        EXPECT_FALSE(parse.options().no_mdns) << no;
    }
}

// The long aliases, so every config key is a flag name

TEST(LongAliases, EachBehavesExactlyLikeItsLetter) {
    std::vector<std::string> args = {"--output",    "null",       "--name",    "kitchen",
                                     "--pidfile",   "/run/x.pid", "--logfile", "/var/log/x.log",
                                     "--log-level", "debug"};
#ifdef SENDSPIN_CLI_HAVE_MDNS
    // --server only parses in a build that can discover a server.
    args.insert(args.end(), {"--server", "mdns:Living Room"});
#endif
    Parse parse(args);

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().device, "null");
    EXPECT_EQ(parse.options().name, "kitchen");
    EXPECT_EQ(parse.options().pidfile, "/run/x.pid");
    EXPECT_EQ(parse.options().logfile, "/var/log/x.log");
    EXPECT_EQ(parse.options().log_level, LogLevel::DEBUG);
#ifdef SENDSPIN_CLI_HAVE_MDNS
    EXPECT_EQ(parse.options().server, "mdns:Living Room");
    EXPECT_TRUE(parse.options().discover);
    EXPECT_EQ(parse.options().discover_name, "Living Room");
#endif
}

TEST(LongAliases, AreListedByHelpAlongsideTheConfigSearchPath) {
    const ScopedEnv xdg("XDG_CONFIG_HOME", "/xdg/config");
    const ScopedEnv home("HOME", "/home/someone");

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

    // --help is the config reference, so every alias and the search order must appear.
    for (const char* flag : {"--output", "--name", "--server", "--pidfile", "--logfile",
                             "--log-level", "--config", "--state-dir"}) {
        EXPECT_NE(text.find(flag), std::string::npos) << flag;
    }
    EXPECT_NE(text.find("/xdg/config/sendspin-cli/config"), std::string::npos);
    EXPECT_NE(text.find(SYSTEM_CONFIG_PATH), std::string::npos);
}

// A configured value is validated by the same code a typed one is

TEST(ConfigRefusals, ABadValueGetsTheFlagsOwnMessagePrefixedWithTheLine) {
    struct Case {
        const char* line;
        const char* expected;
    };
    for (const Case& test : std::vector<Case>{
             {"buffer-ms = 0", "invalid --buffer-ms '0' -- expected 10-2000"},
             {"static-delay = 5001", "invalid --static-delay '5001' -- expected 0-5000"},
             {"port = 99999", "invalid --port '99999' -- expected 1-65535"},
             {"server = music.local", "connecting to an address with -s was removed"},
             {"log-level = shouty", "unknown log level 'shouty'"},
             {"name =", "-n needs a non-empty value"},
             {"no-mdns = perhaps", "invalid --no-mdns 'perhaps'"},
         }) {
        ScratchDir scratch;
        ASSERT_TRUE(scratch.created());
        // On line 2, so the prefix has something to be wrong about.
        const std::string config =
            scratch.write("config", std::string("# a comment\n") + test.line + "\n");

        Parse parse({}, config);

        EXPECT_FALSE(parse.ok()) << test.line;
        const std::string diagnostics = parse.diagnostics();
        EXPECT_NE(diagnostics.find(test.expected), std::string::npos)
            << test.line << ": " << diagnostics;
        // Prefixed with where to go and look, which is the whole difference from a typed value.
        EXPECT_NE(diagnostics.find(config + ":2:"), std::string::npos)
            << test.line << ": " << diagnostics;
    }
}

TEST(ConfigRefusals, AnUnknownKeyIsFatalAndNamed) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "port = 9000\nvolme = 40\n");

    Parse parse({}, config);

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("unknown key 'volme'"), std::string::npos)
        << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find(config + ":2:"), std::string::npos) << parse.diagnostics();
}

TEST(ConfigRefusals, AMalformedLineIsFatalAndNamed) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "port = 9000\n\nthis is not a pair\n");

    Parse parse({}, config);

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find(config + ":3:"), std::string::npos) << parse.diagnostics();
}

TEST(ConfigRefusals, TheRunShapeCannotComeFromAFile) {
    // Run-shape flags are unknown keys to a config file.
    for (const char* key : {"daemonize", "list-devices", "help", "version", "config"}) {
        ScratchDir scratch;
        ASSERT_TRUE(scratch.created());
        const std::string config = scratch.write("config", std::string(key) + " = true\n");

        Parse parse({}, config);

        EXPECT_FALSE(parse.ok()) << key;
        EXPECT_NE(parse.diagnostics().find("unknown key '" + std::string(key) + "'"),
                  std::string::npos)
            << key << ": " << parse.diagnostics();
    }
}

// A configured value reaches every resolution a typed one does

TEST(ConfigMerge, AConfiguredAddressIsRefusedWithoutQuotingIt) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config =
        scratch.write("config", "server = ws://u:s3cr3t@music.local:8927/sendspin\n");

    Parse parse({}, config);

    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("connecting to an address with -s was removed"),
              std::string::npos)
        << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find(config + ":1:"), std::string::npos) << parse.diagnostics();
    EXPECT_EQ(parse.diagnostics().find("s3cr3t"), std::string::npos) << parse.diagnostics();
}

TEST(ConfigMerge, AConfiguredDiscoverySpecIsReadAsOne) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "server = mdns:Living Room\n");

    Parse parse({}, config);

#ifdef SENDSPIN_CLI_HAVE_MDNS
    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_TRUE(parse.options().discover);
    EXPECT_EQ(parse.options().discover_name, "Living Room");
    // Marked as supplied, or the player would dial and advertise at once.
    EXPECT_TRUE(parse.options().was_given(Opt::Server));
    EXPECT_FALSE(parse.options().advertises());
#else
    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("no mDNS support"), std::string::npos)
        << parse.diagnostics();
#endif
}

TEST(ConfigMerge, AConfiguredControlSocketIsAbsolutizedUnderDaemonize) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config =
        scratch.write("config", "control-socket = sendspin.sock\nlogfile = /var/log/x.log\n");

    Parse parse({"-z"}, config);

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    // Absolutized under -z, which is gated on was_given().
    EXPECT_EQ(parse.options().control_socket.front(), '/');
    EXPECT_NE(parse.options().control_socket.find("/sendspin.sock"), std::string::npos)
        << parse.options().control_socket;
}

TEST(ConfigMerge, AnOverLongConfiguredControlSocketIsRefused) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string too_long = "/tmp/" + std::string(control_socket_path_limit() + 8, 'x');
    const std::string config = scratch.write("config", "control-socket = " + too_long + "\n");

    Parse parse({}, config);

    // Refused before anything binds.
    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("Unix socket address holds at most"), std::string::npos)
        << parse.diagnostics();
}

TEST(ConfigMerge, AConfiguredStdoutSinkStillContradictsDaemonize) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "output = stdout\n");

    Parse parse({"-z"}, config);

    // Needs -o and -z to meet across the merge.
    EXPECT_FALSE(parse.ok());
    EXPECT_NE(parse.diagnostics().find("-z cannot write PCM to stdout"), std::string::npos)
        << parse.diagnostics();
}

TEST(ConfigMerge, AConfiguredStateDirIsAbsolutizedUnderDaemonize) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config =
        scratch.write("config", "state-dir = state\nlogfile = /var/log/x.log\n");

    Parse parse({"-z"}, config);

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().state_dir.front(), '/');
}

// What a broken config must not be able to stop

TEST(ConfigShortCircuit, HelpVersionAndListDevicesSurviveABrokenConfig) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "this file is nonsense\n");

    Parse help({"--help"}, config);
    EXPECT_TRUE(help.ok()) << help.diagnostics();
    EXPECT_TRUE(help.options().show_help);

    Parse version({"--version"}, config);
    EXPECT_TRUE(version.ok()) << version.diagnostics();
    EXPECT_TRUE(version.options().show_version);

    Parse list({"-l"}, config);
    EXPECT_TRUE(list.ok()) << list.diagnostics();
    EXPECT_TRUE(list.options().list_devices);
}

// Subcommand runs

TEST(ConfigSubcommand, ReadsThePortAndSocketSoItLooksInTheRightPlace) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "port = 9400\n");

    Parse parse({"status"}, config);

    // Otherwise `status` would look for the default port's socket.
    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_EQ(parse.options().port, 9400);
    EXPECT_EQ(parse.options().subcommand, "status");
}

TEST(ConfigSubcommand, DoesNotWarnAboutDaemonFlagsNobodyTyped) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config =
        scratch.write("config", "output = null\nname = kitchen\nstate-dir = /var/lib/x\n");

    Parse parse({"status"}, config);

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    // A config's daemon-only keys must not trigger the subcommand warning.
    EXPECT_EQ(parse.diagnostics().find("a subcommand reads only"), std::string::npos)
        << parse.diagnostics();
    // A broken config is still refused.
    const std::string broken = scratch.write("broken", "output = null\nbuffer-ms = 0\n");
    Parse refused({"status"}, broken);
    EXPECT_FALSE(refused.ok()) << refused.diagnostics();
}

TEST(ConfigSubcommand, StillWarnsAboutDaemonFlagsThatWereTyped) {
    ScratchDir scratch;
    ASSERT_TRUE(scratch.created());
    const std::string config = scratch.write("config", "port = 9400\n");

    Parse parse({"status", "-o", "null"}, config);

    ASSERT_TRUE(parse.ok()) << parse.diagnostics();
    EXPECT_NE(parse.diagnostics().find("a subcommand reads only"), std::string::npos)
        << parse.diagnostics();
}

}  // namespace
}  // namespace sendspin_cli
