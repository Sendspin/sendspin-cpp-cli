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

/// @file control_test.cpp
/// @brief The control channel's decisions, exercised without binding a socket
///
/// Nothing here opens a socket, forks or touches a filesystem: everything under test is a pure
/// function over a struct a test can build by hand, which is what `src/control_common.cpp`
/// exists to make possible. The parts that genuinely need two processes -- the socket appearing
/// at the default path, a `status` round trip, stale-socket takeover, refusal of a second
/// instance -- are in `scripts/smoke_test.sh`, for the reason README.md gives: this suite has to
/// stay runnable on a bare machine.

#include "control.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sendspin_cli {
namespace {

using sendspin::SendspinControllerCommand;
using sendspin::SendspinRepeatMode;

/// Runs split_subcommand() over a literal command line, as argv really arrives.
///
/// The strings are held by the vector rather than pointed into a temporary, since
/// split_subcommand() takes `char* const[]` exactly as main() gets it.
bool split(const std::vector<std::string>& words, ControlInvocation& out, std::string& error) {
    std::vector<std::string> storage = words;
    std::vector<char*> argv;
    for (std::string& word : storage) {
        argv.push_back(word.data());
    }
    return split_subcommand(static_cast<int>(argv.size()), argv.data(), out, error);
}

/// A request built through the parser, for the tests that only care about the result.
ControlRequest parsed(const std::string& name, const std::vector<std::string>& args) {
    ControlRequest request;
    std::string error;
    EXPECT_TRUE(parse_control_request(name, args, request, error)) << error;
    return request;
}

/// A snapshot of a connected server that offers everything.
ControllerSnapshot everything_supported() {
    ControllerSnapshot snapshot;
    snapshot.connected = true;
    snapshot.supported_commands = {
        SendspinControllerCommand::PLAY,          SendspinControllerCommand::PAUSE,
        SendspinControllerCommand::STOP,          SendspinControllerCommand::NEXT,
        SendspinControllerCommand::PREVIOUS,      SendspinControllerCommand::VOLUME,
        SendspinControllerCommand::MUTE,          SendspinControllerCommand::SEEK,
        SendspinControllerCommand::SEEK_RELATIVE, SendspinControllerCommand::REPEAT_OFF,
        SendspinControllerCommand::REPEAT_ONE,    SendspinControllerCommand::REPEAT_ALL,
        SendspinControllerCommand::SHUFFLE,       SendspinControllerCommand::UNSHUFFLE,
        SendspinControllerCommand::SWITCH,
    };
    return snapshot;
}

/// A status snapshot describing a connected, playing player, for the formatter's tests to vary.
StatusSnapshot playing_snapshot() {
    StatusSnapshot snapshot;
    snapshot.name = "living-room";
    snapshot.connected = true;
    snapshot.server_name = "Music Assistant";
    snapshot.server_id = "OraobU4l";
    snapshot.playback_speed = 1000;
    snapshot.streaming = true;
    snapshot.format = StreamFormat{48000, 2, 16};
    snapshot.artist = "Nils Frahm";
    snapshot.title = "Says";
    snapshot.progress_ms = 125000;
    snapshot.duration_ms = 543000;
    snapshot.group_state_known = true;
    snapshot.group_volume = 55;
    snapshot.group_muted = false;
    snapshot.player_volume = 80;
    snapshot.player_muted = false;
    snapshot.player_volume_source = VolumeSource::Server;
    snapshot.static_delay_ms = 0;
    snapshot.output = "portaudio";
    return snapshot;
}

/// The value of one `key: value` line, or "" when the key is absent.
std::string field(const std::string& block, const std::string& key) {
    const std::string needle = key + ": ";
    size_t position = 0;
    while (position <= block.size()) {
        const size_t line_end = block.find('\n', position);
        const std::string line = block.substr(position, line_end == std::string::npos
                                                            ? std::string::npos
                                                            : line_end - position);
        if (line.compare(0, needle.size(), needle) == 0) {
            return line.substr(needle.size());
        }
        if (line_end == std::string::npos) {
            break;
        }
        position = line_end + 1;
    }
    return {};
}

// ==============================================================================
// The subcommand table
// ==============================================================================

TEST(ControlSubcommands, EveryCommandInTheEnumHasARow) {
    // The table is what --help prints, what the parser reads and what names a command in a
    // diagnostic, so a command reachable in one and missing from it would be invisible in the
    // other two. Checked by walking the enum's whole range rather than the table's own rows.
    for (uint8_t value = 0; value <= static_cast<uint8_t>(ControlCommand::Delay); ++value) {
        const auto command = static_cast<ControlCommand>(value);
        bool found = false;
        for (const ControlSubcommand& subcommand : control_subcommands()) {
            if (subcommand.command == command) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "no subcommand row for ControlCommand " << static_cast<int>(value);
    }
}

TEST(ControlSubcommands, ArityAndArgumentAgree) {
    for (const ControlSubcommand& subcommand : control_subcommands()) {
        // encode_control_request() and split_subcommand() both trust arity to say whether there
        // is an argument to read, and --help prints `argument` beside the name.
        EXPECT_LE(subcommand.arity, 1U) << subcommand.name;
        EXPECT_EQ(subcommand.arity == 1, subcommand.argument != nullptr) << subcommand.name;
    }
}

TEST(ControlSubcommands, EveryProtocolCommandIsReachable) {
    // The point of the item: `controller@v1`'s whole transport surface is drivable locally. A
    // library command with no subcommand mapping onto it would be a hole in that claim.
    std::vector<SendspinControllerCommand> reached;
    for (const ControlSubcommand& subcommand : control_subcommands()) {
        // Every legal argument, not just one: `repeat` and `shuffle` each cover more than one
        // protocol command, so a single sample would leave most of them unreached. The argument
        // is chosen off the table's own `argument` column, so a new subcommand is covered here
        // without this test having to learn its name.
        std::vector<std::vector<std::string>> arguments;
        if (subcommand.arity == 0) {
            arguments.push_back({});
        } else if (std::string(subcommand.argument) == "off|one|all") {
            arguments = {{"off"}, {"one"}, {"all"}};
        } else if (std::string(subcommand.argument) == "on|off") {
            arguments = {{"on"}, {"off"}};
        } else {
            arguments.push_back({"0"});
        }
        for (const std::vector<std::string>& args : arguments) {
            const std::optional<SendspinControllerCommand> command =
                protocol_command(parsed(subcommand.name, args));
            if (command.has_value()) {
                reached.push_back(*command);
            }
        }
    }

    // SEEK_RELATIVE is the library's last enumerator today, and this walk assumes it. A library
    // that appends a command would silently reduce this test's coverage rather than fail it --
    // there is no count to check against, since the enum is in a FetchContent'd header that
    // `SENDSPIN_GIT_TAG` moves. So the count is pinned here, and a tag bump that changes it
    // fails *this* line, which is where the reader will be told to extend the table.
    constexpr int LAST_PROTOCOL_COMMAND =
        static_cast<int>(SendspinControllerCommand::SEEK_RELATIVE);
    EXPECT_EQ(LAST_PROTOCOL_COMMAND, 14)
        << "the library's controller command set changed -- extend control_subcommands() to cover "
           "the new command, then update this count";

    for (int value = 0; value <= LAST_PROTOCOL_COMMAND; ++value) {
        const auto command = static_cast<SendspinControllerCommand>(value);
        EXPECT_NE(std::find(reached.begin(), reached.end(), command), reached.end())
            << "no subcommand sends SendspinControllerCommand " << value;
    }
}

// ==============================================================================
// Splitting argv
// ==============================================================================

TEST(SplitSubcommand, NoArgumentsIsADaemonRun) {
    ControlInvocation invocation;
    std::string error;
    ASSERT_TRUE(split({"sendspin-cli"}, invocation, error)) << error;
    EXPECT_TRUE(invocation.name.empty());
    EXPECT_EQ(invocation.consumed, 0);
}

TEST(SplitSubcommand, AFlagFirstIsADaemonRun) {
    ControlInvocation invocation;
    std::string error;
    ASSERT_TRUE(split({"sendspin-cli", "-o", "null", "--port", "9000"}, invocation, error))
        << error;
    EXPECT_TRUE(invocation.name.empty());
    EXPECT_EQ(invocation.consumed, 0);
}

TEST(SplitSubcommand, ANullaryCommandTakesOneWord) {
    ControlInvocation invocation;
    std::string error;
    ASSERT_TRUE(split({"sendspin-cli", "pause"}, invocation, error)) << error;
    EXPECT_EQ(invocation.name, "pause");
    EXPECT_TRUE(invocation.args.empty());
    EXPECT_EQ(invocation.consumed, 2);
}

TEST(SplitSubcommand, AUnaryCommandTakesItsArgument) {
    ControlInvocation invocation;
    std::string error;
    ASSERT_TRUE(split({"sendspin-cli", "vol", "50"}, invocation, error)) << error;
    EXPECT_EQ(invocation.name, "vol");
    ASSERT_EQ(invocation.args.size(), 1U);
    EXPECT_EQ(invocation.args[0], "50");
    EXPECT_EQ(invocation.consumed, 3);
}

TEST(SplitSubcommand, ANegativeOffsetIsAnArgumentRatherThanAFlag) {
    // The whole reason the split happens before getopt: `-5000` is indistinguishable from a
    // flag cluster to getopt_long(), so the argument has to leave argv before it is scanned.
    ControlInvocation invocation;
    std::string error;
    ASSERT_TRUE(split({"sendspin-cli", "seek-rel", "-5000"}, invocation, error)) << error;
    EXPECT_EQ(invocation.name, "seek-rel");
    ASSERT_EQ(invocation.args.size(), 1U);
    EXPECT_EQ(invocation.args[0], "-5000");
    EXPECT_EQ(invocation.consumed, 3);
}

TEST(SplitSubcommand, FlagsAfterASubcommandAreLeftForGetopt) {
    ControlInvocation invocation;
    std::string error;
    ASSERT_TRUE(split({"sendspin-cli", "vol", "50", "--port", "9000"}, invocation, error)) << error;
    // consumed is where the flags start, and it must not swallow them: glibc would permute
    // them into view and the BSDs would not, so the count is what makes the two agree.
    EXPECT_EQ(invocation.consumed, 3);
}

TEST(SplitSubcommand, AnUnknownFirstWordIsRefusedByName) {
    ControlInvocation invocation;
    std::string error;
    EXPECT_FALSE(split({"sendspin-cli", "paws"}, invocation, error));
    EXPECT_NE(error.find("paws"), std::string::npos);
    // The list is what makes the message actionable rather than merely correct.
    EXPECT_NE(error.find("pause"), std::string::npos);
}

TEST(SplitSubcommand, AMissingArgumentIsRefusedNamingTheShape) {
    ControlInvocation invocation;
    std::string error;
    EXPECT_FALSE(split({"sendspin-cli", "seek"}, invocation, error));
    EXPECT_NE(error.find("seek <ms>"), std::string::npos) << error;
}

TEST(SplitSubcommand, AFlagIsNotReadAsAMissingArgument) {
    // `vol --port 9000` has a word after `vol`, and it is not a volume. The split takes it
    // anyway -- by count, not by shape -- and the parse is what refuses it. That keeps one
    // place deciding what a legal argument is.
    ControlInvocation invocation;
    std::string error;
    ASSERT_TRUE(split({"sendspin-cli", "vol", "--port"}, invocation, error)) << error;
    ASSERT_EQ(invocation.args.size(), 1U);
    EXPECT_EQ(invocation.args[0], "--port");

    ControlRequest request;
    EXPECT_FALSE(parse_control_request(invocation.name, invocation.args, request, error));
}

// ==============================================================================
// Parsing each subcommand's argument
// ==============================================================================

TEST(ParseControlRequest, NullaryCommandsMapOntoTheirProtocolCommand) {
    EXPECT_EQ(protocol_command(parsed("play", {})), SendspinControllerCommand::PLAY);
    EXPECT_EQ(protocol_command(parsed("pause", {})), SendspinControllerCommand::PAUSE);
    EXPECT_EQ(protocol_command(parsed("stop", {})), SendspinControllerCommand::STOP);
    EXPECT_EQ(protocol_command(parsed("next", {})), SendspinControllerCommand::NEXT);
    EXPECT_EQ(protocol_command(parsed("prev", {})), SendspinControllerCommand::PREVIOUS);
    EXPECT_EQ(protocol_command(parsed("switch", {})), SendspinControllerCommand::SWITCH);
}

TEST(ParseControlRequest, StatusDispatchesNothing) {
    // Answered out of the daemon's own shadows, which is what makes it readable while
    // disconnected.
    EXPECT_FALSE(protocol_command(parsed("status", {})).has_value());
}

TEST(ParseControlRequest, VolumeTakesZeroToOneHundred) {
    EXPECT_EQ(parsed("vol", {"0"}).volume, 0);
    EXPECT_EQ(parsed("vol", {"100"}).volume, 100);
    EXPECT_EQ(parsed("vol", {"50"}).volume, 50);
    EXPECT_EQ(to_client_command(parsed("vol", {"50"})).volume, 50);
    EXPECT_EQ(protocol_command(parsed("vol", {"50"})), SendspinControllerCommand::VOLUME);
}

TEST(ParseControlRequest, VolumeRejectsOutOfRangeNamingTheRange) {
    ControlRequest request;
    std::string error;
    for (const char* value : {"101", "255", "-1", "1000", "", " 50", "+50", "50.0", "abc"}) {
        EXPECT_FALSE(parse_control_request("vol", {value}, request, error))
            << "accepted vol '" << value << "'";
        EXPECT_NE(error.find("0 to 100"), std::string::npos) << error;
        // The rejected value is quoted back, so the message names what was wrong.
        EXPECT_NE(error.find(std::string(value)), std::string::npos) << error;
    }
}

TEST(ParseControlRequest, DelayTakesZeroToTheSpecsMaximum) {
    EXPECT_EQ(parsed("delay", {"0"}).delay_ms, 0);
    EXPECT_EQ(parsed("delay", {"250"}).delay_ms, 250);
    EXPECT_EQ(parsed("delay", {std::to_string(MAX_STATIC_DELAY_MS)}).delay_ms, MAX_STATIC_DELAY_MS);
}

TEST(ParseControlRequest, DelayRejectsOutOfRangeRatherThanLettingItBeClamped) {
    // The reason this repo carries its own bound: PlayerRole::update_static_delay() would take
    // 9000 silently down to 5000 and report success for a delay nobody asked for.
    ControlRequest request;
    std::string error;
    for (const char* value : {"5001", "9000", "65536", "-1", "", " 250", "+250", "250.0", "abc"}) {
        EXPECT_FALSE(parse_control_request("delay", {value}, request, error))
            << "accepted delay '" << value << "'";
        // The message names the bound, so a caller learns the range from the refusal.
        EXPECT_NE(error.find("0 to " + std::to_string(MAX_STATIC_DELAY_MS)), std::string::npos)
            << error;
        EXPECT_NE(error.find(std::string(value)), std::string::npos) << error;
    }
}

TEST(ParseControlRequest, DelayDispatchesNothingAndIsNeverSentToTheServer) {
    // The static delay is this endpoint's own player-role state, so nothing goes out as a
    // controller command -- update_static_delay() republishes `client/state` by itself.
    EXPECT_FALSE(protocol_command(parsed("delay", {"250"})).has_value());

    // And the trap that makes the daemon's ordering load-bearing: to_client_command() falls back to
    // PLAY for a request with no protocol command, so a `delay` that ever reached it would start
    // playback. The dispatcher branches on Delay before send_command() precisely so it cannot.
    EXPECT_EQ(to_client_command(parsed("delay", {"250"})).command, SendspinControllerCommand::PLAY)
        << "the fallback changed -- the dispatcher's Delay branch is what keeps this unreachable";
}

TEST(ParseControlRequest, MuteAndShuffleTakeOnOff) {
    EXPECT_EQ(parsed("mute", {"on"}).flag, true);
    EXPECT_EQ(parsed("mute", {"off"}).flag, false);
    EXPECT_EQ(to_client_command(parsed("mute", {"on"})).muted, true);
    EXPECT_EQ(to_client_command(parsed("mute", {"off"})).muted, false);
    EXPECT_EQ(protocol_command(parsed("mute", {"on"})), SendspinControllerCommand::MUTE);

    // shuffle off is its own protocol command rather than a false-valued parameter.
    EXPECT_EQ(protocol_command(parsed("shuffle", {"on"})), SendspinControllerCommand::SHUFFLE);
    EXPECT_EQ(protocol_command(parsed("shuffle", {"off"})), SendspinControllerCommand::UNSHUFFLE);
    // ...and it must not arrive as a mute value.
    EXPECT_FALSE(to_client_command(parsed("shuffle", {"off"})).muted.has_value());
}

TEST(ParseControlRequest, MuteAndShuffleRejectAnythingElse) {
    ControlRequest request;
    std::string error;
    for (const char* name : {"mute", "shuffle"}) {
        for (const char* value : {"true", "1", "yes", "ON", "", "onn"}) {
            EXPECT_FALSE(parse_control_request(name, {value}, request, error))
                << "accepted " << name << " '" << value << "'";
            EXPECT_NE(error.find("'on' or 'off'"), std::string::npos) << error;
        }
    }
}

TEST(ParseControlRequest, RepeatMapsOntoThreeCommands) {
    EXPECT_EQ(protocol_command(parsed("repeat", {"off"})), SendspinControllerCommand::REPEAT_OFF);
    EXPECT_EQ(protocol_command(parsed("repeat", {"one"})), SendspinControllerCommand::REPEAT_ONE);
    EXPECT_EQ(protocol_command(parsed("repeat", {"all"})), SendspinControllerCommand::REPEAT_ALL);
}

TEST(ParseControlRequest, RepeatRejectsAnythingElse) {
    ControlRequest request;
    std::string error;
    for (const char* value : {"none", "single", "queue", "", "OFF"}) {
        EXPECT_FALSE(parse_control_request("repeat", {value}, request, error))
            << "accepted repeat '" << value << "'";
        EXPECT_NE(error.find("'off', 'one' or 'all'"), std::string::npos) << error;
    }
}

TEST(ParseControlRequest, SeekTakesANonNegativePosition) {
    EXPECT_EQ(parsed("seek", {"0"}).position_ms, 0U);
    EXPECT_EQ(parsed("seek", {"30000"}).position_ms, 30000U);
    // The whole uint32_t range the library's position_ms carries.
    EXPECT_EQ(parsed("seek", {"4294967295"}).position_ms, 4294967295U);
    EXPECT_EQ(to_client_command(parsed("seek", {"30000"})).position_ms, 30000U);
    EXPECT_FALSE(to_client_command(parsed("seek", {"30000"})).offset_ms.has_value());
}

TEST(ParseControlRequest, SeekRejectsNegativeAndOverflowing) {
    ControlRequest request;
    std::string error;
    for (const char* value : {"-1", "-30000", "4294967296", "99999999999999999999", "", "abc"}) {
        EXPECT_FALSE(parse_control_request("seek", {value}, request, error))
            << "accepted seek '" << value << "'";
        EXPECT_NE(error.find("non-negative"), std::string::npos) << error;
        // Points at the flag that does take a negative, rather than only refusing.
        EXPECT_NE(error.find("seek-rel"), std::string::npos) << error;
    }
}

TEST(ParseControlRequest, SeekRelativeTakesTheWholeInt32Range) {
    EXPECT_EQ(parsed("seek-rel", {"0"}).offset_ms, 0);
    EXPECT_EQ(parsed("seek-rel", {"10000"}).offset_ms, 10000);
    EXPECT_EQ(parsed("seek-rel", {"+10000"}).offset_ms, 10000);
    EXPECT_EQ(parsed("seek-rel", {"-10000"}).offset_ms, -10000);
    EXPECT_EQ(parsed("seek-rel", {"2147483647"}).offset_ms, 2147483647);
    // The negative end is one wider than the positive one, which is why the bound is applied
    // before the sign rather than by negating a value that has already had to fit.
    EXPECT_EQ(parsed("seek-rel", {"-2147483648"}).offset_ms, -2147483648LL);
    EXPECT_EQ(to_client_command(parsed("seek-rel", {"-10000"})).offset_ms, -10000);
    EXPECT_FALSE(to_client_command(parsed("seek-rel", {"-10000"})).position_ms.has_value());
}

TEST(ParseControlRequest, SeekRelativeRejectsPastInt32) {
    ControlRequest request;
    std::string error;
    for (const char* value : {"2147483648", "-2147483649", "", "abc", "--5", "5-"}) {
        EXPECT_FALSE(parse_control_request("seek-rel", {value}, request, error))
            << "accepted seek-rel '" << value << "'";
        EXPECT_NE(error.find("-2147483648 to 2147483647"), std::string::npos) << error;
    }
}

TEST(ParseControlRequest, TheWrongNumberOfArgumentsIsRefused) {
    ControlRequest request;
    std::string error;
    EXPECT_FALSE(parse_control_request("pause", {"now"}, request, error));
    EXPECT_FALSE(parse_control_request("vol", {}, request, error));
    EXPECT_FALSE(parse_control_request("vol", {"50", "60"}, request, error));
    EXPECT_FALSE(parse_control_request("nonsense", {}, request, error));
}

// ==============================================================================
// The wire form of a request
// ==============================================================================

TEST(ControlRequestWire, EveryRequestSurvivesARoundTrip) {
    // The daemon reads the line back through the same parser the subcommand used, so this is
    // what stops the two ends drifting into disagreeing about what `vol 50` means.
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases = {
        {"status", {}},         {"play", {}},              {"pause", {}},
        {"stop", {}},           {"next", {}},              {"prev", {}},
        {"switch", {}},         {"vol", {"0"}},            {"vol", {"100"}},
        {"mute", {"on"}},       {"mute", {"off"}},         {"shuffle", {"on"}},
        {"shuffle", {"off"}},   {"repeat", {"off"}},       {"repeat", {"one"}},
        {"repeat", {"all"}},    {"seek", {"0"}},           {"seek", {"4294967295"}},
        {"seek-rel", {"-2147483648"}}, {"seek-rel", {"2147483647"}},
        {"delay", {"0"}},
        {"delay", {"5000"}},
    };

    for (const auto& [name, args] : cases) {
        const ControlRequest sent = parsed(name, args);
        const std::string line = encode_control_request(sent);

        std::string decoded_name;
        std::vector<std::string> decoded_args;
        ASSERT_TRUE(split_control_line(line, decoded_name, decoded_args)) << line;
        ControlRequest received;
        std::string error;
        ASSERT_TRUE(parse_control_request(decoded_name, decoded_args, received, error))
            << line << ": " << error;

        EXPECT_EQ(received.command, sent.command) << line;
        EXPECT_EQ(received.volume, sent.volume) << line;
        EXPECT_EQ(received.flag, sent.flag) << line;
        EXPECT_EQ(received.position_ms, sent.position_ms) << line;
        EXPECT_EQ(received.offset_ms, sent.offset_ms) << line;
        EXPECT_EQ(received.repeat, sent.repeat) << line;
        EXPECT_EQ(received.delay_ms, sent.delay_ms) << line;
        EXPECT_EQ(protocol_command(received), protocol_command(sent)) << line;
    }
}

TEST(ControlRequestWire, ZeroValuedArgumentsSurvive) {
    // The encoder walks the optionals in order, so a `vol 0` -- a set field holding a falsy
    // value -- is the case a has_value()-less check would silently drop.
    EXPECT_EQ(encode_control_request(parsed("vol", {"0"})), "vol 0");
    EXPECT_EQ(encode_control_request(parsed("seek", {"0"})), "seek 0");
    EXPECT_EQ(encode_control_request(parsed("seek-rel", {"0"})), "seek-rel 0");
    EXPECT_EQ(encode_control_request(parsed("mute", {"off"})), "mute off");
    EXPECT_EQ(encode_control_request(parsed("repeat", {"off"})), "repeat off");
    EXPECT_EQ(encode_control_request(parsed("delay", {"0"})), "delay 0");
}

TEST(SplitControlLine, ReadsWordsAndRejectsAnEmptyLine) {
    std::string name;
    std::vector<std::string> args;

    ASSERT_TRUE(split_control_line("vol 50", name, args));
    EXPECT_EQ(name, "vol");
    ASSERT_EQ(args.size(), 1U);
    EXPECT_EQ(args[0], "50");

    // Extra whitespace is a peer's formatting rather than an argument.
    ASSERT_TRUE(split_control_line("  seek-rel \t -500  ", name, args));
    EXPECT_EQ(name, "seek-rel");
    ASSERT_EQ(args.size(), 1U);
    EXPECT_EQ(args[0], "-500");

    ASSERT_TRUE(split_control_line("pause", name, args));
    EXPECT_EQ(name, "pause");
    EXPECT_TRUE(args.empty());

    EXPECT_FALSE(split_control_line("", name, args));
    EXPECT_FALSE(split_control_line("   \t ", name, args));
}

// ==============================================================================
// Whether the daemon may dispatch
// ==============================================================================

TEST(ControlRefusal, ASupportedCommandOnAConnectedServerIsDispatched) {
    ControlStatus status = ControlStatus::Failed;
    std::string reason;
    EXPECT_FALSE(control_refusal(parsed("pause", {}), everything_supported(), status, reason));
    EXPECT_TRUE(reason.empty());
}

TEST(ControlRefusal, NotConnectedIsReportedAsNotConnected) {
    ControllerSnapshot snapshot;
    snapshot.connected = false;

    ControlStatus status = ControlStatus::Failed;
    std::string reason;
    ASSERT_TRUE(control_refusal(parsed("pause", {}), snapshot, status, reason));
    EXPECT_EQ(status, ControlStatus::NotConnected);
    EXPECT_NE(reason.find("not connected"), std::string::npos) << reason;
}

TEST(ControlRefusal, ADisconnectedDaemonDoesNotReportUnsupported) {
    // The failure this ordering exists to prevent. on_controller_state_clear() empties
    // supported_commands on a drop, so a gate that consulted it first would answer "pause is
    // not supported" when the truth is that nothing is connected -- and the operator would go
    // looking at their server's capabilities instead of at the connection.
    ControllerSnapshot snapshot;
    snapshot.connected = false;
    snapshot.supported_commands.clear();

    ControlStatus status = ControlStatus::Failed;
    std::string reason;
    ASSERT_TRUE(control_refusal(parsed("pause", {}), snapshot, status, reason));
    EXPECT_EQ(status, ControlStatus::NotConnected);
    EXPECT_EQ(reason.find("not offer"), std::string::npos) << reason;
    EXPECT_EQ(reason.find("supported_commands"), std::string::npos) << reason;
}

TEST(ControlRefusal, AnAbsentCommandIsReportedAsUnsupported) {
    ControllerSnapshot snapshot = everything_supported();
    snapshot.supported_commands = {SendspinControllerCommand::PLAY};

    ControlStatus status = ControlStatus::Failed;
    std::string reason;
    ASSERT_TRUE(control_refusal(parsed("pause", {}), snapshot, status, reason));
    EXPECT_EQ(status, ControlStatus::Unsupported);
    EXPECT_NE(reason.find("pause"), std::string::npos) << reason;
    EXPECT_NE(reason.find("supported_commands"), std::string::npos) << reason;
}

TEST(ControlRefusal, AConnectedServerWithNoStateYetSaysSo) {
    // Connected but no server/state has arrived. Still unsupported -- sending would be ignored
    // -- but the reason has to distinguish "not yet" from "never", because only one of them
    // resolves itself.
    ControllerSnapshot snapshot;
    snapshot.connected = true;

    ControlStatus status = ControlStatus::Failed;
    std::string reason;
    ASSERT_TRUE(control_refusal(parsed("pause", {}), snapshot, status, reason));
    EXPECT_EQ(status, ControlStatus::Unsupported);
    EXPECT_NE(reason.find("no commands at all yet"), std::string::npos) << reason;
}

TEST(ControlRefusal, StatusIsNeverRefused) {
    // Readable in every state, and most useful in the worst one: a disconnected daemon with no
    // controller state is exactly when an operator wants to know what the player thinks.
    ControllerSnapshot disconnected;
    ControlStatus status = ControlStatus::Failed;
    std::string reason;
    EXPECT_FALSE(control_refusal(parsed("status", {}), disconnected, status, reason));
    EXPECT_FALSE(control_refusal(parsed("status", {}), everything_supported(), status, reason));
}

TEST(ControlRefusal, DelayIsNeverRefusedEither) {
    // The other locally answered request, and the case that matters most: a speaker's own delay
    // does not become unsettable because nothing is currently connected to it. Exempted through
    // protocol_command() returning nothing rather than by name, so this and `status` share one
    // rule.
    ControllerSnapshot disconnected;
    ControlStatus status = ControlStatus::Failed;
    std::string reason;
    EXPECT_FALSE(control_refusal(parsed("delay", {"250"}), disconnected, status, reason));
    EXPECT_FALSE(control_refusal(parsed("delay", {"250"}), everything_supported(), status, reason));

    // And specifically not reported as unsupported: `set_static_delay` is a *player* command, so it
    // is not in the controller role's supported_commands and never should be looked for there.
    ControllerSnapshot connected_no_state;
    connected_no_state.connected = true;
    EXPECT_FALSE(control_refusal(parsed("delay", {"250"}), connected_no_state, status, reason));
}

TEST(ControlRefusal, SeekPastSeekMaxIsRefusedNamingTheBound) {
    ControllerSnapshot snapshot = everything_supported();
    snapshot.seek_max_ms = 240000;

    ControlStatus status = ControlStatus::Ok;
    std::string reason;
    EXPECT_FALSE(control_refusal(parsed("seek", {"240000"}), snapshot, status, reason));

    ASSERT_TRUE(control_refusal(parsed("seek", {"240001"}), snapshot, status, reason));
    EXPECT_EQ(status, ControlStatus::Usage);
    EXPECT_NE(reason.find("240000"), std::string::npos) << reason;
}

TEST(ControlRefusal, SeekIsUnboundedWhenTheServerPublishesNoMaximum) {
    // Absent for a live or unknown-duration stream, where the server itself has no bound to
    // apply -- so neither does this.
    ControllerSnapshot snapshot = everything_supported();
    snapshot.seek_max_ms.reset();

    ControlStatus status = ControlStatus::Ok;
    std::string reason;
    EXPECT_FALSE(control_refusal(parsed("seek", {"4294967295"}), snapshot, status, reason));
}

TEST(ControlRefusal, RelativeSeekIsNotBoundedBySeekMax) {
    // Deliberate, and the reason is worth keeping under test: the server does not bound a
    // relative seek, and this client's only view of the position is its own interpolated
    // shadow -- so bounding it here would refuse legitimate commands off stale data.
    ControllerSnapshot snapshot = everything_supported();
    snapshot.seek_max_ms = 1000;

    ControlStatus status = ControlStatus::Ok;
    std::string reason;
    EXPECT_FALSE(control_refusal(parsed("seek-rel", {"2147483647"}), snapshot, status, reason));
    EXPECT_FALSE(control_refusal(parsed("seek-rel", {"-2147483648"}), snapshot, status, reason));
}

// ==============================================================================
// The status block
// ==============================================================================

TEST(FormatStatus, EveryFieldIsPresentAndLabelled) {
    const std::string block = format_status(playing_snapshot());
    EXPECT_EQ(field(block, "name"), "living-room");
    EXPECT_NE(field(block, "server").find("Music Assistant"), std::string::npos);
    EXPECT_EQ(field(block, "state"), "playing");
    EXPECT_EQ(field(block, "stream"), "receiving");
    EXPECT_EQ(field(block, "track"), "Nils Frahm - Says");
    EXPECT_EQ(field(block, "position"), "2:05 / 9:03 (estimated)");
    EXPECT_EQ(field(block, "group volume"), "55");
    EXPECT_EQ(field(block, "player volume"), "80");
    EXPECT_EQ(field(block, "static delay"), "0 ms");
    EXPECT_EQ(field(block, "output"), "portaudio (48000 Hz / 2 ch / 16-bit)");
    // Every line is `key: value`, so `cut -d: -f2` and `grep` both reach a field.
    EXPECT_EQ(block.back(), '\n');
}

TEST(FormatStatus, GroupAndPlayerVolumeAreSeparateLines) {
    // The distinction the whole item turns on: `vol` moves the *group*, and the server clamps
    // it per player, so one ambiguous `volume:` would leave a reader unable to tell which
    // number their `vol 50` had moved.
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.group_volume = 55;
    snapshot.group_muted = true;
    snapshot.player_volume = 80;
    snapshot.player_muted = false;

    const std::string block = format_status(snapshot);
    EXPECT_EQ(field(block, "group volume"), "55 (muted)");
    EXPECT_EQ(field(block, "player volume"), "80");
    EXPECT_EQ(field(block, "volume"), "") << "there must be no unqualified volume line";
}

TEST(FormatStatus, AVolumeNoServerHasSetIsMarkedAsADefault) {
    // The bug this exists to stop coming back. The library's `PlayerRole` defaults its volume to 0
    // while every sink runs at DEFAULT_SINK_VOLUME from the first sample, so reporting the role's
    // number printed `player volume: 0` at a player that was audibly at full output. `status`
    // reports the gain the sink is applying, and says nobody chose it. (`main()` now also pushes
    // that figure into the role at startup, so the two agree on the wire -- but this line is about
    // which of them `status` asks, which is still the sink's side.)
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.player_volume = DEFAULT_SINK_VOLUME;
    snapshot.player_volume_source = VolumeSource::SinkDefault;

    const std::string line = field(format_status(snapshot), "player volume");
    EXPECT_NE(line.find(std::to_string(static_cast<unsigned>(DEFAULT_SINK_VOLUME))),
              std::string::npos)
        << line;
    EXPECT_NE(line.find("no server has set it"), std::string::npos) << line;
    // And the number itself must never be 0, which is the claim that sent people looking for a
    // dead player. Taken as the leading token rather than searched for, since "100" contains a 0.
    EXPECT_NE(line.substr(0, line.find(' ')), "0") << line;
}

TEST(FormatStatus, AVolumeAServerChoseIsNotMarkedAsADefault) {
    // The other half: a server that deliberately set full output must not be described as a
    // default, or the qualifier stops meaning anything.
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.player_volume = DEFAULT_SINK_VOLUME;
    snapshot.player_volume_source = VolumeSource::Server;

    const std::string line = field(format_status(snapshot), "player volume");
    EXPECT_EQ(line, std::to_string(static_cast<unsigned>(DEFAULT_SINK_VOLUME)));
}

TEST(FormatStatus, ARestoredVolumeIsNeitherADefaultNorAServersChoice) {
    // The third case, and the reason the flag stopped being a bool: a volume read back from the
    // state store is not DEFAULT_SINK_VOLUME, so calling it a default is false -- and no server
    // on this connection chose it either, so leaving it unqualified would be a claim one had.
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.player_volume = 42;
    snapshot.player_volume_source = VolumeSource::Restored;

    const std::string line = field(format_status(snapshot), "player volume");
    EXPECT_NE(line.find("42"), std::string::npos) << line;
    EXPECT_EQ(line.find("(default"), std::string::npos) << line;
    EXPECT_NE(line.find("remembered"), std::string::npos) << line;
    EXPECT_NE(line.find("no server has set it"), std::string::npos) << line;
}

TEST(FormatStatus, TheStaticDelayIsReportedSoItsCommandIsVisible) {
    // Reported for the reason `repeat` and `shuffle` are: `delay` can change it, and a setting
    // whose effect `status` cannot show leaves a user unable to see what they did or to put it
    // back.
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.static_delay_ms = 375;
    EXPECT_EQ(field(format_status(snapshot), "static delay"), "375 ms");

    // Zero is a real value -- the delay turned off -- and reads as such rather than as `unknown`.
    // The role always holds a figure, whether or not anybody chose it.
    snapshot.static_delay_ms = 0;
    EXPECT_EQ(field(format_status(snapshot), "static delay"), "0 ms");

    // Its own line rather than folded into the player volume, so `grep`/`cut -d:` reach it.
    snapshot.static_delay_ms = MAX_STATIC_DELAY_MS;
    EXPECT_EQ(field(format_status(snapshot), "static delay"), "5000 ms");
}

TEST(FormatStatus, TheStaticDelayIsReportedWhileDisconnected) {
    // It is this process's own state, so unlike every server-sourced field it is knowable with
    // nothing connected -- which is also when `delay` is most likely to have just been used.
    StatusSnapshot snapshot;
    snapshot.name = "kitchen";
    snapshot.output = "null";
    snapshot.static_delay_ms = 120;
    EXPECT_EQ(field(format_status(snapshot), "static delay"), "120 ms");
}

TEST(FormatStatus, TheQueueModesAreReportedSoTheirCommandsAreVisible) {
    // `repeat` and `shuffle` are the only subcommands whose effect status could not show, which
    // left a user unable to see what they had just changed -- or to put it back.
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.group_state_known = true;

    for (const auto& [mode, name] : std::vector<std::pair<SendspinRepeatMode, std::string>>{
             {SendspinRepeatMode::OFF, "off"},
             {SendspinRepeatMode::ONE, "one"},
             {SendspinRepeatMode::ALL, "all"}}) {
        snapshot.group_repeat = mode;
        EXPECT_EQ(field(format_status(snapshot), "repeat"), name);
    }

    snapshot.group_shuffle = true;
    EXPECT_EQ(field(format_status(snapshot), "shuffle"), "on");
    snapshot.group_shuffle = false;
    EXPECT_EQ(field(format_status(snapshot), "shuffle"), "off");
}

TEST(FormatStatus, UnknownQueueModesAreNotPrintedAsOff) {
    // Same trap the group volume has: a default-constructed controller object is OFF and
    // unshuffled, so printing it would be a claim about the group rather than an absence of one.
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.group_state_known = false;

    const std::string block = format_status(snapshot);
    EXPECT_EQ(field(block, "repeat"), "unknown");
    EXPECT_EQ(field(block, "shuffle"), "unknown");
}

TEST(FormatStatus, TheTransportStateComesFromPlaybackSpeed) {
    StatusSnapshot snapshot = playing_snapshot();

    snapshot.playback_speed = 0;
    EXPECT_EQ(field(format_status(snapshot), "state"), "paused");

    snapshot.playback_speed = 1000;
    EXPECT_EQ(field(format_status(snapshot), "state"), "playing");

    // A speed the protocol allows but no server sends today: named rather than rounded to
    // "playing", so an unusual value is visible instead of hidden.
    snapshot.playback_speed = 1500;
    EXPECT_EQ(field(format_status(snapshot), "state"), "playing (speed 1500/1000)");
}

TEST(FormatStatus, NoProgressReadsUnknownRatherThanGuessing) {
    // The server having sent no progress is not a state. Reporting it as "stopped" would be a
    // claim about the group made out of an absence of information.
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.playback_speed.reset();
    snapshot.progress_ms.reset();
    snapshot.duration_ms.reset();

    const std::string block = format_status(snapshot);
    EXPECT_EQ(field(block, "state"), "unknown");
    EXPECT_EQ(field(block, "position"), "unknown");
}

TEST(FormatStatus, ADisconnectedPlayerStillReportsWhatItKnows) {
    // The criterion: `status` against a daemon with no server connection prints, says it is not
    // connected, and still reports the local facts -- which is when it is most worth reading.
    StatusSnapshot snapshot;
    snapshot.name = "living-room";
    snapshot.connected = false;
    snapshot.player_volume = 80;
    snapshot.player_volume_source = VolumeSource::Server;
    snapshot.output = "null";

    const std::string block = format_status(snapshot);
    EXPECT_EQ(field(block, "name"), "living-room");
    EXPECT_EQ(field(block, "server"), "not connected");
    EXPECT_EQ(field(block, "state"), "unknown");
    EXPECT_EQ(field(block, "stream"), "idle");
    EXPECT_EQ(field(block, "output"), "null");
    // Local, so it is known; the group's is not.
    EXPECT_EQ(field(block, "player volume"), "80");
    EXPECT_EQ(field(block, "group volume"), "unknown");
}

TEST(FormatStatus, AnUnknownGroupVolumeIsNotPrintedAsZero) {
    // A default-constructed ServerStateControllerObject is 0 and unmuted, which would read as a
    // group turned all the way down rather than as a group nothing is known about.
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.group_state_known = false;
    snapshot.group_volume = 0;
    EXPECT_EQ(field(format_status(snapshot), "group volume"), "unknown");
}

TEST(FormatStatus, APlayingPositionIsMarkedAsAnEstimate) {
    // While the group plays, the library interpolates from the last progress the server sent, so
    // a server that does not resend it after a seek leaves the figure drifting by however far the
    // seek moved -- observed against a real server, where both seek forms moved the audio while
    // this number carried on climbing. Paused, it is the server's own snapshot and needs no mark.
    StatusSnapshot snapshot = playing_snapshot();

    snapshot.playback_speed = 1000;
    EXPECT_NE(field(format_status(snapshot), "position").find("(estimated)"), std::string::npos);

    snapshot.playback_speed = 0;
    EXPECT_EQ(field(format_status(snapshot), "position").find("(estimated)"), std::string::npos);

    // And an absent speed is already `unknown`, which is not a figure to qualify.
    snapshot.playback_speed.reset();
    snapshot.progress_ms.reset();
    EXPECT_EQ(field(format_status(snapshot), "position"), "unknown");
}

TEST(FormatStatus, AConnectedPlayerSaysWhichFieldsAreTheServersWord) {
    // The misreading this prevents cost real debugging time: `shuffle` and `repeat` read `off`
    // against a server that acts on them and never reports them back, and the position kept
    // climbing through seeks that audibly worked. A reader who has just changed something needs
    // to know which figures can lag before concluding the command failed.
    const std::string note = field(format_status(playing_snapshot()), "note");
    EXPECT_FALSE(note.empty());
    for (const char* field_name : {"position", "repeat", "shuffle"}) {
        EXPECT_NE(note.find(field_name), std::string::npos) << field_name << " not named: " << note;
    }

    // Absent when there is no server, since then nothing above it is the server's word.
    StatusSnapshot disconnected;
    disconnected.name = "x";
    EXPECT_EQ(field(format_status(disconnected), "note"), "");
}

TEST(FormatStatus, ALiveStreamsPositionNamesItsUnknownDuration) {
    // Paused, so the reading is the server's own snapshot and carries no `(estimated)` marker --
    // this test is about the clock, not about provenance.
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.playback_speed = 0;
    snapshot.progress_ms = 65000;
    snapshot.duration_ms = 0;
    EXPECT_EQ(field(format_status(snapshot), "position"), "1:05 / unknown");
}

TEST(FormatStatus, PositionsPastAnHourCarryTheHour) {
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.playback_speed = 0;
    snapshot.progress_ms = 3725000;
    snapshot.duration_ms = 7200000;
    EXPECT_EQ(field(format_status(snapshot), "position"), "1:02:05 / 2:00:00");
}

TEST(FormatStatus, APartlyKnownTrackDoesNotPrintADanglingSeparator) {
    StatusSnapshot snapshot = playing_snapshot();

    snapshot.artist.clear();
    EXPECT_EQ(field(format_status(snapshot), "track"), "Says");

    snapshot = playing_snapshot();
    snapshot.title.clear();
    EXPECT_EQ(field(format_status(snapshot), "track"), "Nils Frahm");

    snapshot.artist.clear();
    EXPECT_EQ(field(format_status(snapshot), "track"), "unknown");
}

TEST(FormatStatus, AnIdleStreamHasNoFormat) {
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.streaming = false;
    snapshot.format.reset();

    const std::string block = format_status(snapshot);
    EXPECT_EQ(field(block, "stream"), "idle");
    // The device is still named: it is what a stream would land on.
    EXPECT_EQ(field(block, "output"), "portaudio");
}

TEST(FormatStatus, AStreamWhoseFormatWasRefusedStillReadsAsStreaming) {
    // The combination `streaming && !format`, which is what PlayerListener reports when the
    // device refused the stream's format -- audio is arriving and being discarded. Reporting it
    // as idle would contradict the ERROR the player raises about exactly that, and would answer
    // "nothing is being sent to me" to an operator diagnosing "nothing is coming out".
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.streaming = true;
    snapshot.format.reset();

    const std::string block = format_status(snapshot);
    EXPECT_EQ(field(block, "stream"), "receiving");
    EXPECT_EQ(field(block, "output"), "portaudio");
}

TEST(FormatStatus, AConnectedServerWithNoNameStillReadsAsConnected) {
    StatusSnapshot snapshot = playing_snapshot();
    snapshot.server_name.clear();
    EXPECT_NE(field(format_status(snapshot), "server").find("connected"), std::string::npos);
    EXPECT_NE(field(format_status(snapshot), "server").find("OraobU4l"), std::string::npos);
}

// ==============================================================================
// The reply's status line
// ==============================================================================

TEST(ControlReply, OkRoundTripsWithItsPayload) {
    const std::string reply = encode_control_reply(ControlStatus::Ok, "", "name: x\n");
    EXPECT_EQ(reply, "ok\nname: x\n");

    ControlStatus status = ControlStatus::Failed;
    std::string reason;
    ASSERT_TRUE(decode_control_reply("ok", status, reason));
    EXPECT_EQ(status, ControlStatus::Ok);
    EXPECT_TRUE(reason.empty());
}

TEST(ControlReply, EveryErrorKindRoundTripsToItsOwnStatus) {
    // The exit statuses a script tells apart, so each kind has to survive the wire as itself
    // rather than collapsing into a generic failure.
    for (ControlStatus sent : {ControlStatus::Usage, ControlStatus::NotConnected,
                               ControlStatus::Unsupported, ControlStatus::Failed}) {
        const std::string reply = encode_control_reply(sent, "because: reasons", "");
        std::string first = reply.substr(0, reply.find('\n'));

        ControlStatus status = ControlStatus::Ok;
        std::string reason;
        ASSERT_TRUE(decode_control_reply(first, status, reason)) << first;
        EXPECT_EQ(status, sent) << first;
        // The reason survives intact, colons and all -- it is split on the *first* colon only.
        EXPECT_EQ(reason, "because: reasons") << first;
    }
}

TEST(ControlReply, ANonReplyIsRejectedRatherThanPrinted) {
    // Something is listening on that path and it is not a sendspin-cli daemon. Printing its
    // answer as though it were a status would be worse than saying so.
    ControlStatus status = ControlStatus::Ok;
    std::string reason;
    EXPECT_FALSE(decode_control_reply("", status, reason));
    EXPECT_FALSE(decode_control_reply("HTTP/1.1 200 OK", status, reason));
    EXPECT_FALSE(decode_control_reply("error", status, reason));
    EXPECT_FALSE(decode_control_reply("error no-colon here", status, reason));
    EXPECT_FALSE(decode_control_reply("OK", status, reason));
}

TEST(ControlReply, AnUnknownKindKeepsTheReasonAndDegrades) {
    // A newer daemon against an older subcommand. The reason is the useful half, so it is kept
    // rather than the whole reply being discarded.
    ControlStatus status = ControlStatus::Ok;
    std::string reason;
    ASSERT_TRUE(decode_control_reply("error future-thing: something new", status, reason));
    EXPECT_EQ(status, ControlStatus::Failed);
    EXPECT_EQ(reason, "something new");
}

// ==============================================================================
// Line framing
// ==============================================================================

TEST(LineAssembler, AWholeLineInOneReadIsReady) {
    LineAssembler assembler;
    EXPECT_EQ(assembler.feed("pause\n", 6), LineState::Ready);
    EXPECT_EQ(assembler.line(), "pause");
}

TEST(LineAssembler, APartialLineAcrossTwoReadsIsAssembled) {
    // What a non-blocking socket really does: hands over whatever has arrived, which is not
    // guaranteed to be a whole line or any of it.
    LineAssembler assembler;
    EXPECT_EQ(assembler.feed("se", 2), LineState::Incomplete);
    EXPECT_EQ(assembler.feed("ek 30", 5), LineState::Incomplete);
    EXPECT_EQ(assembler.feed("000\n", 4), LineState::Ready);
    EXPECT_EQ(assembler.line(), "seek 30000");
}

TEST(LineAssembler, NoTrailingNewlineIsTakenAtEndOfInput) {
    // `printf status | socat - UNIX-CONNECT:...` sends no newline and shuts its write side
    // down; end-of-input terminates a line as unambiguously as '\n'.
    LineAssembler assembler;
    EXPECT_EQ(assembler.feed("status", 6), LineState::Incomplete);
    EXPECT_EQ(assembler.finish(), LineState::Ready);
    EXPECT_EQ(assembler.line(), "status");
}

TEST(LineAssembler, AClosedConnectionWithNothingBufferedIsNotALine) {
    LineAssembler assembler;
    EXPECT_EQ(assembler.finish(), LineState::Incomplete);
}

TEST(LineAssembler, AnEmptyLineIsReadyAndEmpty) {
    // Ready at this layer and refused at the parse layer, which is the right split: framing
    // says a line arrived, the parser says whether it means anything.
    LineAssembler assembler;
    EXPECT_EQ(assembler.feed("\n", 1), LineState::Ready);
    EXPECT_EQ(assembler.line(), "");

    std::string name;
    std::vector<std::string> args;
    EXPECT_FALSE(split_control_line(assembler.line(), name, args));
}

TEST(LineAssembler, CarriageReturnBeforeTheNewlineIsStripped) {
    LineAssembler assembler;
    EXPECT_EQ(assembler.feed("pause\r\n", 7), LineState::Ready);
    EXPECT_EQ(assembler.line(), "pause");
}

TEST(LineAssembler, AnOverLongLineIsRefusedRatherThanBuffered) {
    LineAssembler assembler;
    const std::string flood(MAX_CONTROL_LINE_BYTES + 1, 'x');
    EXPECT_EQ(assembler.feed(flood.data(), flood.size()), LineState::TooLong);
    EXPECT_STRNE(line_state_reason(LineState::TooLong), "");
}

TEST(LineAssembler, ALineExactlyAtTheBoundIsAccepted) {
    // The bound is on the line, not on the line plus its newline: a request of exactly
    // MAX_CONTROL_LINE_BYTES bytes is legal, and one byte more is not.
    LineAssembler assembler;
    const std::string exact(MAX_CONTROL_LINE_BYTES, 'x');
    EXPECT_EQ(assembler.feed(exact.data(), exact.size()), LineState::Incomplete);
    EXPECT_EQ(assembler.feed("\n", 1), LineState::Ready);
    EXPECT_EQ(assembler.line().size(), MAX_CONTROL_LINE_BYTES);
}

TEST(LineAssembler, AnEmbeddedNulIsRefused) {
    // A text protocol, and a NUL is how a truncated C string or a binary peer arrives. Refused
    // so nothing downstream has to be NUL-safe.
    LineAssembler assembler;
    EXPECT_EQ(assembler.feed("pau\0se\n", 7), LineState::Invalid);
    EXPECT_STRNE(line_state_reason(LineState::Invalid), "");
}

TEST(LineAssembler, BytesAfterTheFirstNewlineAreDropped) {
    // One command per connection, so a second line is a peer talking out of turn rather than
    // a pipeline to honour.
    LineAssembler assembler;
    EXPECT_EQ(assembler.feed("pause\nplay\n", 11), LineState::Ready);
    EXPECT_EQ(assembler.line(), "pause");
}

// ==============================================================================
// The socket path
// ==============================================================================

/// A runtime-directory result naming `path`, for the absent-reason tests.
ControlRuntimeDir runtime_dir_of(const std::string& path) {
    ControlRuntimeDir runtime;
    runtime.path = path;
    return runtime;
}

TEST(ControlSocketPath, TheDefaultCarriesThePort) {
    // The port is what lets two players share a host, and what lets a subcommand derive the
    // same path from the same --port.
    EXPECT_EQ(control_socket_path("/run/user/1000", 8928),
              "/run/user/1000/sendspin-cli-8928.sock");
    EXPECT_EQ(control_socket_path("/run/user/1000", 9000),
              "/run/user/1000/sendspin-cli-9000.sock");
    EXPECT_NE(control_socket_path("/run/user/1000", 8928),
              control_socket_path("/run/user/1000", 8929));
}

TEST(ControlSocketPath, NoRuntimeDirectoryYieldsNoPathAndAReason) {
    // Deliberately not a /tmp fallback: a world-writable directory would let any local account
    // pause playback and switch this endpoint out of its group.
    const std::string path = control_socket_path("", 8928);
    EXPECT_TRUE(path.empty());

    const std::string reason = control_socket_absent_reason(runtime_dir_of(""), path);
    EXPECT_FALSE(reason.empty());
    EXPECT_NE(reason.find("XDG_RUNTIME_DIR"), std::string::npos) << reason;
    // Names the fix, since the player carries on without a control channel.
    EXPECT_NE(reason.find("--control-socket"), std::string::npos) << reason;
    EXPECT_EQ(reason.find("/tmp"), std::string::npos) << reason;
}

TEST(ControlSocketPath, AUsableDefaultHasNoAbsentReason) {
    const std::string path = control_socket_path("/run/user/1000", 8928);
    EXPECT_TRUE(control_socket_absent_reason(runtime_dir_of("/run/user/1000"), path).empty());
}

TEST(ControlSocketPath, AnOverLongPathDoesNotFit) {
    // 104 bytes on macOS, 108 on Linux, and read off the struct rather than written down --
    // truncating instead of refusing would bind a socket nothing can find.
    EXPECT_TRUE(control_socket_path_fits("/run/user/1000/sendspin-cli-8928.sock"));
    EXPECT_FALSE(control_socket_path_fits(""));
    EXPECT_FALSE(control_socket_path_fits(std::string(control_socket_path_limit(), 'x')));
    EXPECT_TRUE(control_socket_path_fits(std::string(control_socket_path_limit() - 1, 'x')));
}

// ==============================================================================
// Which directories are fit to hold the socket
// ==============================================================================
//
// These touch the filesystem, which the suite's own boundary allows -- what it forbids is opening
// an audio device, a socket or the mDNS daemon. `daemon_test.cpp` and `last_server_test.cpp`
// already create scratch files for the same reason.

/// A directory of its own per test, with a settable mode, removed again afterwards.
///
/// Under the test binary's working directory rather than /tmp, for the reason
/// `last_server_test.cpp` gives: a suite that scatters files outside the build tree is one that
/// leaves something behind when it fails.
class ScratchDir {
public:
    explicit ScratchDir(mode_t mode) {
        this->path_ = "control-test-" + std::to_string(::getpid()) + "-" +
                      std::to_string(ScratchDir::next_id());
        this->created_ = ::mkdir(this->path_.c_str(), mode) == 0;
        // mkdir() applies the umask, which would clear exactly the group/other bits one of these
        // tests is trying to set -- so the mode is applied again explicitly.
        if (this->created_) {
            this->created_ = ::chmod(this->path_.c_str(), mode) == 0;
        }
    }

    ~ScratchDir() {
        if (this->created_) {
            ::rmdir(this->path_.c_str());
        }
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    bool created() const {
        return this->created_;
    }

    const std::string& path() const {
        return this->path_;
    }

private:
    static int next_id() {
        static int id = 0;
        return ++id;
    }

    std::string path_;
    bool created_{false};
};

TEST(PrivateRuntimeDir, APrivateDirectoryThisUserOwnsIsAccepted) {
    const ScratchDir dir(0700);
    ASSERT_TRUE(dir.created());

    std::string reason;
    EXPECT_TRUE(is_private_runtime_dir(dir.path(), reason)) << reason;
    EXPECT_TRUE(reason.empty());
}

TEST(PrivateRuntimeDir, AGroupOrWorldWritableDirectoryIsRefused) {
    // The check that carries the security argument: a directory anyone else can write to is one
    // where the socket can be replaced or unlinked, whatever mode the socket itself carries. And
    // macOS and the BSDs do not enforce socket-inode permissions on connect() at all, so on those
    // platforms the directory is the *only* thing standing between another local account and this
    // player's transport controls.
    for (mode_t mode : {static_cast<mode_t>(0770), static_cast<mode_t>(0707),
                        static_cast<mode_t>(0777)}) {
        const ScratchDir dir(mode);
        ASSERT_TRUE(dir.created()) << "mode " << mode;

        std::string reason;
        EXPECT_FALSE(is_private_runtime_dir(dir.path(), reason)) << "mode " << mode;
        EXPECT_NE(reason.find("writable"), std::string::npos) << reason;
    }
}

TEST(PrivateRuntimeDir, SomethingThatIsNotADirectoryIsRefused) {
    std::string reason;
    EXPECT_FALSE(is_private_runtime_dir("/etc/hosts", reason));
    EXPECT_NE(reason.find("not a directory"), std::string::npos) << reason;
}

TEST(PrivateRuntimeDir, AMissingPathIsRefused) {
    std::string reason;
    EXPECT_FALSE(is_private_runtime_dir("/no/such/directory/here", reason));
    EXPECT_FALSE(reason.empty());
}

TEST(PrivateRuntimeDir, ADirectoryOwnedBySomeoneElseIsRefused) {
    // Skipped for root, which owns / and would legitimately pass. The same accommodation
    // `last_server_test.cpp` makes for its unwritable-directory case.
    if (::geteuid() == 0) {
        GTEST_SKIP() << "running as root, which owns /";
    }
    std::string reason;
    EXPECT_FALSE(is_private_runtime_dir("/", reason));
    EXPECT_NE(reason.find("not by this user"), std::string::npos) << reason;
}

TEST(PrivateRuntimeDir, ASymlinkIsJudgedByWhatItPointsAt) {
    // The documented direction, and the safe one: stat() follows, so a link into a world-writable
    // directory is refused for the target's mode rather than accepted for the link's. lstat()
    // would instead refuse a legitimately symlinked $XDG_RUNTIME_DIR for being a link at all.
    const ScratchDir target(0700);
    ASSERT_TRUE(target.created());

    const std::string safe_link = target.path() + "-link";
    const std::string unsafe_link = target.path() + "-tmplink";
    ASSERT_EQ(::symlink(target.path().c_str(), safe_link.c_str()), 0);
    ASSERT_EQ(::symlink("/tmp", unsafe_link.c_str()), 0);

    std::string reason;
    EXPECT_TRUE(is_private_runtime_dir(safe_link, reason)) << reason;
    EXPECT_FALSE(is_private_runtime_dir(unsafe_link, reason));

    ::unlink(safe_link.c_str());
    ::unlink(unsafe_link.c_str());
}

TEST(PrivateRuntimeDir, TmpIsRefused) {
    // Named explicitly because it is the fallback this design refuses, and the reason it does:
    // /tmp is 1777 on every platform this builds on.
    std::string reason;
    EXPECT_FALSE(is_private_runtime_dir("/tmp", reason)) << "/tmp must never be usable";
}

TEST(PlatformRuntimeDir, WhateverItReturnsIsPrivate) {
    // Empty where there is no platform convention to fall back on -- which is everywhere but
    // macOS -- and a verified directory where there is. Either way it must never hand back a path
    // that would fail the check the code applies before using it.
    std::string rejection;
    const std::string platform = control_platform_runtime_dir(rejection);
#ifdef __APPLE__
    // Asserted rather than tolerated: this is the platform the fallback exists for, and an empty
    // answer here is the acceptance criterion failing, not a configuration to skip past.
    ASSERT_FALSE(platform.empty())
        << "macOS must supply a per-user runtime directory; rejected because: " << rejection;
#else
    // And asserted the other way, which locks in "no third source": a future accidental fallback
    // on a platform with no convention for one would fail here rather than pass unnoticed.
    EXPECT_TRUE(platform.empty()) << platform;
    if (platform.empty()) {
        return;
    }
#endif

    std::string reason;
    EXPECT_TRUE(is_private_runtime_dir(platform, reason)) << platform << ": " << reason;
    // Absolute, and not the one directory that is always wrong.
    EXPECT_EQ(platform.front(), '/') << platform;
    EXPECT_NE(platform, "/tmp");
    // No trailing slash: confstr() supplies one, and a path built on it would carry a '//'.
    EXPECT_NE(platform.back(), '/') << platform;
    // And short enough to still leave room for the socket's own leaf.
    EXPECT_TRUE(control_socket_path_fits(control_socket_path(platform, 8928)))
        << control_socket_path(platform, 8928);
}

TEST(PlatformRuntimeDir, ARefusedCandidateExplainsItself) {
    // The whole point of carrying the rejection out: an empty answer *with* a candidate behind it
    // has to say what was wrong with it, or the operator has nothing to act on. Where the platform
    // has no candidate at all, nothing was tried, so there is nothing to explain.
    std::string rejection;
    const std::string platform = control_platform_runtime_dir(rejection);
    if (platform.empty() && !rejection.empty()) {
        // A real refusal: it must name the directory and the problem.
        EXPECT_NE(rejection.find('/'), std::string::npos) << rejection;
    }
    if (!platform.empty()) {
        EXPECT_TRUE(rejection.empty()) << "accepted a directory but also explained a refusal";
    }
}

TEST(ControlSocketAbsentReason, ARefusedPlatformDirectoryIsNamedRatherThanGeneralised) {
    // "not set" and "found it, and it is group-writable" are different problems with different
    // fixes, so the reason has to distinguish them rather than collapsing both into the first.
    ControlRuntimeDir refused;
    refused.rejection = "/var/folders/xx/T is writable by its group or by everyone";

    const std::string reason = control_socket_absent_reason(refused, "");
    EXPECT_NE(reason.find("writable by its group"), std::string::npos) << reason;
    EXPECT_NE(reason.find("--control-socket"), std::string::npos) << reason;
    // And it must not claim the variable is unset, which is a different and wrong diagnosis.
    EXPECT_EQ(reason.find("is not set"), std::string::npos) << reason;
}

TEST(ControlSocketPath, AnOverLongDefaultIsAnAbsentReasonRatherThanATruncation) {
    // A pathologically deep $XDG_RUNTIME_DIR is the same class of problem as an absent one: the
    // player carries on without a control channel and says which flag fixes it.
    const std::string deep = "/run/user/1000/" + std::string(control_socket_path_limit(), 'd');
    const std::string path = control_socket_path(deep, 8928);
    EXPECT_FALSE(path.empty());

    const std::string reason = control_socket_absent_reason(runtime_dir_of(deep), path);
    EXPECT_FALSE(reason.empty());
    EXPECT_NE(reason.find("--control-socket"), std::string::npos) << reason;
}

}  // namespace
}  // namespace sendspin_cli
