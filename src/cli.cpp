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

#include "cli.h"

#include "audio_sink.h"
#include "config_file.h"
#include "control.h"
#include "log.h"
#include "mdns.h"
#include "supported_formats.h"

#include <getopt.h>
#include <limits.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;
using sendspin::SendspinClientConfig;

namespace {

constexpr const char* FALLBACK_NAME = "sendspin-cli";

/// The port a Sendspin *server* listens on, for -s to dial when none is given.
///
/// Deliberately not SendspinClientConfig::DEFAULT_SERVER_PORT (8928): that constant is
/// the port *this* process serves on, since a sendspin player is itself a WebSocket
/// server that a controller connects to. Using it as the outbound default pointed -s at
/// the wrong end of the protocol. Upstream documents the server side as
/// `ws://server.local:8927/sendspin` (include/sendspin/client.h, src/esp/client_connection.h).
constexpr uint16_t DEFAULT_REMOTE_SERVER_PORT = 8927U;

/// What stands in for a redacted secret in a logged URL, at a fixed width.
///
/// Fixed rather than one '*' per character: the length of a password is worth nothing to a
/// reader of the log and something to whoever else ends up holding it.
constexpr const char* USERINFO_MASK = "***";

/// Long-only option values, picked outside the short-option alphabet so `-V`/`-p` stay
/// unclaimed for squeezelite's own meanings.
enum LongOnly {
    OPT_VERSION = 0x100,
    OPT_PORT,
    OPT_BUFFER_MS,
    OPT_STATIC_DELAY,
    OPT_NO_MDNS,
    OPT_MDNS_NAME,
    OPT_CONTROL_SOCKET,
    OPT_NO_CONTROL,
    OPT_STATE_DIR,
    OPT_CONFIG,
    OPT_HOOK_START,
    OPT_HOOK_STOP,
    OPT_ID,
    OPT_MANUFACTURER,
    OPT_PRODUCT_NAME,
    OPT_AUDIO_FORMAT,
};

/// @brief An option a config file may set: its key, and how a diagnostic names it.
///
/// The key is the long flag name minus the dashes, which is what makes `--help` the config
/// reference rather than a second document to keep in step.
struct SettableOption {
    Opt opt;
    const char* key;

    /// The spelling every message about this option uses. Still the *short* form for the six that
    /// had one first, because those messages are written into parse_server_url() and the validators
    /// below -- and because a reader who typed `-s` should not be answered about `--server`.
    const char* flag;
};

/// The options a config file may set: the `Opt` enum minus the run shape.
///
/// `-l`, `-z`, `--config`, `--help` and `--version` are left out, and so is any subcommand.
/// Excluding is reversible; debugging a `daemonize` that came out of a file under systemd is not.
const std::vector<SettableOption>& settable_options() {
    static const std::vector<SettableOption> table = {
        {Opt::Device, "output", "-o"},
        {Opt::Name, "name", "-n"},
        {Opt::Server, "server", "-s"},
        {Opt::Pidfile, "pidfile", "-P"},
        {Opt::Logfile, "logfile", "-f"},
        {Opt::LogLevel, "log-level", "-d"},
        {Opt::Port, "port", "--port"},
        {Opt::BufferMs, "buffer-ms", "--buffer-ms"},
        {Opt::StaticDelay, "static-delay", "--static-delay"},
        {Opt::NoMdns, "no-mdns", "--no-mdns"},
        {Opt::MdnsName, "mdns-name", "--mdns-name"},
        {Opt::ControlSocket, "control-socket", "--control-socket"},
        {Opt::NoControl, "no-control", "--no-control"},
        {Opt::StateDir, "state-dir", "--state-dir"},
        {Opt::HookStart, "hook-start", "--hook-start"},
        {Opt::HookStop, "hook-stop", "--hook-stop"},
        {Opt::ClientId, "id", "--id"},
        {Opt::Manufacturer, "manufacturer", "--manufacturer"},
        {Opt::ProductName, "product-name", "--product-name"},
        {Opt::AudioFormat, "audio-format", "--audio-format"},
    };
    return table;
}

/// The entry for `opt`, or one apply_option() refuses when the table has none.
///
/// The fallback carries `Opt::Config`, which apply_option() answers with "cannot be set this way",
/// so an option wired into the getopt switch and forgotten in the table fails at its first use
/// instead of silently writing to whichever field happens to be first in the table.
const SettableOption& settable_option(Opt opt) {
    static const SettableOption unmapped{Opt::Config, "", "an option with no config key"};
    for (const SettableOption& option : settable_options()) {
        if (option.opt == opt) {
            return option;
        }
    }
    return unmapped;
}

bool is_all_digits(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    return value.find_first_not_of("0123456789") == std::string::npos;
}

/// Parses a TCP port: digits only, 1-65535.
///
/// The digits-only test is not redundant with strtoul's: strtoul would happily accept
/// " 8927" and "+8927", and read "-1" as a huge unsigned that only fails the range check
/// by accident. A port is a plain decimal number or it is a typo.
bool parse_port(const std::string& str, uint16_t& port) {
    if (!is_all_digits(str)) {
        return false;
    }
    const unsigned long value = std::strtoul(str.c_str(), nullptr, 10);
    if (value == 0 || value > 65535UL) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

/// Parses a buffer size in milliseconds: digits only, MIN_BUFFER_MS to MAX_BUFFER_MS.
///
/// Digits-only for parse_port()'s reason -- strtoul would take " 100" and "+100", and read
/// "-1" as a huge unsigned that fails the range check only by accident.
bool parse_buffer_ms(const std::string& str, uint32_t& buffer_ms) {
    if (!is_all_digits(str)) {
        return false;
    }
    const unsigned long value = std::strtoul(str.c_str(), nullptr, 10);
    if (value < MIN_BUFFER_MS || value > MAX_BUFFER_MS) {
        return false;
    }
    buffer_ms = static_cast<uint32_t>(value);
    return true;
}

/// Parses a static delay in milliseconds: digits only, 0 to MAX_STATIC_DELAY_MS.
///
/// Zero is legal and meaningful -- it is the value that turns the delay off -- so unlike
/// parse_buffer_ms() there is no floor. Digits-only for parse_port()'s reason, which matters
/// more here than elsewhere: the library clamps a value past the end rather than refusing it,
/// so anything this accepts loosely becomes a silently different delay.
bool parse_static_delay(const std::string& str, uint16_t& delay_ms) {
    if (!is_all_digits(str)) {
        return false;
    }
    const unsigned long value = std::strtoul(str.c_str(), nullptr, 10);
    if (value > MAX_STATIC_DELAY_MS) {
        return false;
    }
    delay_ms = static_cast<uint16_t>(value);
    return true;
}

/// Names the option that getopt just reported a problem with.
///
/// For a short option `optopt` is the precise answer -- the argv word would name the whole
/// cluster, so `-lo` would read as "-lo" rather than "-o". For a long option the argv word
/// is the precise answer, and `optopt` must not be used: it carries the option's `val`,
/// which for our long-only options is deliberately outside the char range (OPT_PORT is
/// 0x101), so casting it to a char would print garbage.
std::string offending_option(char* const argv[], int index) {
    const char* word = argv[index - 1];
    const bool is_long = word[0] == '-' && word[1] == '-';
    if (!is_long && optopt != 0) {
        return std::string("-") + static_cast<char>(optopt);
    }
    return word;
}

/// Maps a level name onto the library's LogLevel. Accepts squeezelite's vocabulary
/// (info, debug, sdebug) as well as the library's own names.
bool parse_log_level(const char* str, LogLevel& level) {
    if (std::strcmp(str, "none") == 0 || std::strcmp(str, "off") == 0) {
        level = LogLevel::NONE;
    } else if (std::strcmp(str, "error") == 0 || std::strcmp(str, "err") == 0) {
        level = LogLevel::ERROR;
    } else if (std::strcmp(str, "warn") == 0 || std::strcmp(str, "warning") == 0) {
        level = LogLevel::WARN;
    } else if (std::strcmp(str, "info") == 0) {
        level = LogLevel::INFO;
    } else if (std::strcmp(str, "debug") == 0) {
        level = LogLevel::DEBUG;
    } else if (std::strcmp(str, "verbose") == 0 || std::strcmp(str, "sdebug") == 0) {
        level = LogLevel::VERBOSE;
    } else {
        return false;
    }
    return true;
}

/// Accepts squeezelite's `-d <category>=<level>` shape.
///
/// The category is parsed and ignored, and stays that way: sendspin-cpp logs through
/// `fprintf(stderr)` macros gated on one global int with no sink or filter hook, so raising
/// the level for one of our categories would either flood the log with unrelated library
/// debug or show nothing at all from the library. Every line carries a tag instead, which is
/// per-category filtering after the fact -- and it works on the library's lines too.
/// @param err Where the ignored-category warning goes, or nullptr to suppress it -- which is what a
/// value being validated into a scratch `Options` wants, since nothing is going to act on it.
bool parse_log_spec(const char* spec, LogLevel& level, std::FILE* err) {
    const char* eq = std::strchr(spec, '=');
    if (eq == nullptr) {
        return parse_log_level(spec, level);
    }
    if (!parse_log_level(eq + 1, level)) {
        return false;
    }
    if (err == nullptr) {
        return true;
    }
    std::string tags;
    for (const char* tag : LOG_TAGS) {
        if (!tags.empty()) {
            tags += ", ";
        }
        tags += tag;
    }
    std::fprintf(err,
                 "warning: -d category '%.*s' ignored -- this build has one global log level. "
                 "Every line carries a tag (%s), so filter after the fact: "
                 "-d debug 2>&1 | grep ' %s:'\n",
                 static_cast<int>(eq - spec), spec, tags.c_str(), LOG_TAG_MDNS);
    return true;
}

/// Parses a flag that is a switch on the command line and needs a word in a config file.
///
/// Generous about spelling on purpose. `--no-mdns` carries its answer in the flag's own name, so a
/// file has to spell one out, and there is no honest way to guess which of `true`, `yes`, `on` and
/// `1` an operator will reach for. All four are unambiguous, so all four are taken.
bool parse_bool(const std::string& value, bool& result) {
    if (value == "true" || value == "yes" || value == "on" || value == "1") {
        result = true;
        return true;
    }
    if (value == "false" || value == "no" || value == "off" || value == "0") {
        result = false;
        return true;
    }
    return false;
}

/// Applies one option's value, and is the *only* place that turns a value into an `Options` field.
///
/// That single door is the whole of what layering a config file under the command line means: a
/// configured value is accepted, refused and normalized by exactly the code a typed one is, with
/// the same message. Two copies of these rules would drift, and the drift would show up as a
/// config file the flag surface disagreed with.
///
/// @param error Set to the diagnostic when false comes back, in the flag's own words and without
/// any mention of where the value came from -- the caller prefixes the file and line, because only
/// it knows whether there was one.
/// @param err Where parse_log_spec()'s ignored-category warning goes; nothing else writes here.
/// @return true when the value was accepted, in which case `opt` has also been marked as supplied.
bool apply_option(const SettableOption& option, const std::string& value, Options& out,
                  std::string& error, std::FILE* err) {
    // Refuses an empty value for a flag whose empty case has no meaning: `-n ""` used to fall
    // through to the hostname, which reads as the flag being ignored, and `-P ""` and `-f ""` would
    // try to open a file with no name. From a config file the same shape is a truncated `name =`.
    const auto empty_value = [&option, &error, &value]() {
        if (!value.empty()) {
            return false;
        }
        error = std::string(option.flag) + " needs a non-empty value";
        return true;
    };

    switch (option.opt) {
        case Opt::Device:
            if (empty_value()) {
                return false;
            }
            out.device = value;
            break;
        case Opt::Name:
            if (empty_value()) {
                return false;
            }
            out.name = value;
            break;
        case Opt::Server:
            // Not checked for emptiness here: parse_server_url() answers `-s ""` with a line that
            // says what a server looks like, which is more use than "needs a non-empty value".
            out.server = value;
            break;
        case Opt::Pidfile:
            if (empty_value()) {
                return false;
            }
            out.pidfile = value;
            break;
        case Opt::Logfile:
            if (empty_value()) {
                return false;
            }
            out.logfile = value;
            break;
        case Opt::LogLevel:
            if (!parse_log_spec(value.c_str(), out.log_level, err)) {
                error = "unknown log level '" + value + "'";
                return false;
            }
            break;
        case Opt::Port:
            if (!parse_port(value, out.port)) {
                error = "invalid --port '" + value + "' -- expected 1-65535";
                return false;
            }
            break;
        case Opt::BufferMs:
            if (!parse_buffer_ms(value, out.buffer_ms)) {
                error = "invalid --buffer-ms '" + value + "' -- expected " +
                        std::to_string(MIN_BUFFER_MS) + "-" + std::to_string(MAX_BUFFER_MS);
                return false;
            }
            break;
        case Opt::StaticDelay:
            if (!parse_static_delay(value, out.static_delay_ms)) {
                error = "invalid --static-delay '" + value + "' -- expected 0-" +
                        std::to_string(MAX_STATIC_DELAY_MS);
                return false;
            }
            break;
        case Opt::MdnsName:
            if (empty_value()) {
                return false;
            }
            out.mdns_name = value;
            break;
        case Opt::ControlSocket:
            if (empty_value()) {
                return false;
            }
            // Only stored here; the length check and the -z rewrite happen once the whole line and
            // the config file have been read and --port is known.
            out.control_socket = value;
            break;
        case Opt::StateDir:
            if (empty_value()) {
                return false;
            }
            out.state_dir = value;
            break;
        case Opt::HookStart:
            if (empty_value()) {
                return false;
            }
            out.hook_start = value;
            break;
        case Opt::HookStop:
            if (empty_value()) {
                return false;
            }
            out.hook_stop = value;
            break;
        case Opt::ClientId:
            if (empty_value()) {
                return false;
            }
            out.client_id = value;
            break;
        case Opt::Manufacturer:
            if (empty_value()) {
                return false;
            }
            out.manufacturer = value;
            break;
        case Opt::ProductName:
            if (empty_value()) {
                return false;
            }
            out.product_name = value;
            break;
        case Opt::AudioFormat: {
            sendspin::AudioSupportedFormatObject format;
            std::string reason;
            if (!parse_format_spec(value, format, reason)) {
                error = "invalid --audio-format '" + value + "': " + reason;
                return false;
            }
            out.audio_format = format;
            break;
        }
        case Opt::NoMdns:
            if (!parse_bool(value, out.no_mdns)) {
                error = "invalid --no-mdns '" + value + "' -- expected true or false";
                return false;
            }
            break;
        case Opt::NoControl:
            if (!parse_bool(value, out.no_control)) {
                error = "invalid --no-control '" + value + "' -- expected true or false";
                return false;
            }
            break;
        case Opt::ListDevices:
        case Opt::Daemonize:
        case Opt::Config:
            // Not in settable_options(), so no caller can reach these. Listed rather than
            // defaulted, so adding an option to the enum fails to compile here until it is either
            // handled or deliberately excluded.
            error = "internal: " + std::string(option.flag) + " cannot be set this way";
            return false;
    }
    out.mark_given(option.opt);
    return true;
}

/// Fills anything the command line did not supply from `config`, and refuses what it cannot read.
///
/// **Marks each one as supplied as well as setting it**, and that is load-bearing rather than
/// tidy: `Options::advertises()` is `!no_mdns && !was_given(Opt::Server)`, so a configured `server`
/// left unmarked would have this player dial *and* advertise `_sendspin._tcp` -- which the spec
/// forbids -- while the -s resolution never filled `server_url`, leaving the value inert as well
/// as non-compliant. `control-socket`'s absolutization and `sun_path` check are gated the same way.
///
/// @param subcommand_run True for `sendspin-cli <subcommand>`, which applies only the two options
/// it actually reads. A configured `output` is still *validated* on that path -- a broken config is
/// broken whichever way the binary was invoked -- but applying it would only make the "a subcommand
/// reads only --port and --control-socket" warning fire at every operator who has a config file.
/// @param origin Filled with `<file>:<line>: ` per option supplied, so the two resolutions that
/// run *after* this -- the -s URL parse and the socket path's length check -- can still say which
/// line to go and fix. Only diagnostics read it; no behaviour does, which is why it is not a second
/// `given_` bitmask.
/// @param error Set to the first problem, prefixed with the file and the line it is on.
/// @return false when there is an error to report.
bool merge_config(const ConfigFile& config, bool subcommand_run, Options& out,
                  std::map<Opt, std::string>& origin, std::string& error, std::FILE* err) {
    // Last wins within one file, matching the state store's reader. Resolved up front rather than
    // by letting the first occurrence mark the option as supplied, which would quietly make it
    // *first* wins instead.
    std::map<std::string, size_t> last_line;
    for (const KeyValueEntry& entry : config.entries) {
        last_line[entry.key] = entry.line;
    }

    for (const KeyValueEntry& entry : config.entries) {
        const std::string where = config.path + ":" + std::to_string(entry.line) + ": ";

        const SettableOption* option = nullptr;
        for (const SettableOption& candidate : settable_options()) {
            if (entry.key == candidate.key) {
                option = &candidate;
                break;
            }
        }
        if (option == nullptr) {
            // Fatal, and that includes a real flag that is deliberately not settable -- `daemonize`
            // is an unknown *key*. A silently ignored typo is the same failure mode a bad -s is
            // already refused for.
            error = where + "unknown key '" + entry.key + "'";
            return false;
        }
        if (entry.line != last_line[entry.key]) {
            continue;
        }
        // The command line wins outright: this is the whole precedence rule, and it is one line
        // because `was_given()` was built for it.
        if (out.was_given(option->opt)) {
            continue;
        }
        // A subcommand reads only these two, so only these two reach `out`. Everything else is
        // still validated -- a broken config is broken whichever way the binary was invoked -- but
        // into a scratch copy nothing reads, because *applying* it would make the "a subcommand
        // reads only --port and --control-socket" warning below fire at every operator who has a
        // config file at all.
        const bool applies =
            !subcommand_run || option->opt == Opt::Port || option->opt == Opt::ControlSocket;
        Options scratch;
        Options& target = applies ? out : scratch;

        std::string message;
        // No diagnostics stream for a value nothing will act on: `log-level = audio=debug` in a
        // config would otherwise print its ignored-category warning on every subcommand run, about
        // a flag nobody typed -- the very noise this whole branch exists to avoid.
        if (!apply_option(*option, entry.value, target, message, applies ? err : nullptr)) {
            error = where + message;
            return false;
        }
        if (applies) {
            origin[option->opt] = where;
        }
    }
    return true;
}

/// `path` made absolute against the current directory, unchanged if it already is.
///
/// Only -z needs this, and it needs it badly: the daemon chdir()s to / so it does not pin a
/// mount point, so a relative -P names the directory the operator typed it in to the parent's
/// probe and a file directly under / to the child that actually writes it -- two files, and
/// for a non-root user the second one fails after the terminal has already seen success. A
/// relative -f splits the same way on the SIGHUP reopen.
std::string absolute_path(const std::string& path) {
    if (!path.empty() && path.front() == '/') {
        return path;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        // Nothing better to offer than what was typed, and open() will report it either way.
        return path;
    }
    return std::string(cwd) + "/" + path;
}

/// The column --help wraps at, matching the width the hand-written flag lines already use.
constexpr size_t USAGE_WIDTH = 79;

/// Writes `text` word-wrapped, continuing at column `indent`, and ends the line.
///
/// The cursor is assumed to already be at `indent`, which is what the `%-20s` before each call
/// site guarantees. Only the subcommand table needs this: the flag lines below are wrapped by
/// hand, since each one's shape is part of how it reads.
void print_wrapped(std::FILE* out, const char* text, size_t indent) {
    size_t column = indent;
    const char* word = text;
    while (*word != '\0') {
        const char* end = std::strchr(word, ' ');
        const size_t length = end == nullptr ? std::strlen(word) : static_cast<size_t>(end - word);
        if (column > indent && column + 1 + length > USAGE_WIDTH) {
            std::fprintf(out, "\n%*s", static_cast<int>(indent), "");
            column = indent;
        } else if (column > indent) {
            std::fputc(' ', out);
            ++column;
        }
        std::fprintf(out, "%.*s", static_cast<int>(length), word);
        column += length;
        word = end == nullptr ? word + length : end + 1;
    }
    std::fputc('\n', out);
}

/// Rewinds getopt's process-global scan state so parse_options() can run more than once.
///
/// glibc and musl treat `optind = 0` as "re-initialise everything"; `optind = 1` only
/// rewinds the index and leaves internal state (the mid-cluster position, the argv
/// permutation bookkeeping) from the previous call. The BSDs spell the same request
/// `optreset`. Without this a second parse in one process reads from wherever the first
/// one stopped -- which is exactly what a test binary does dozens of times.
void reset_getopt() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    optreset = 1;
    optind = 1;
#else
    optind = 0;
#endif
    // getopt's own messages go to stderr and would both duplicate ours and bypass the
    // caller's diagnostics stream, so we take over reporting entirely.
    opterr = 0;
}

}  // namespace

bool parse_options(int argc, char* argv[], Options& out, std::FILE* err) {
    static const struct option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, OPT_VERSION},
        // Long aliases for the six squeezelite letters, routed to the same handlers. They exist so
        // every config key is a flag name: one vocabulary, and --help stays the config reference.
        {"output", required_argument, nullptr, 'o'},
        {"name", required_argument, nullptr, 'n'},
        {"server", required_argument, nullptr, 's'},
        {"pidfile", required_argument, nullptr, 'P'},
        {"logfile", required_argument, nullptr, 'f'},
        {"log-level", required_argument, nullptr, 'd'},
        {"config", required_argument, nullptr, OPT_CONFIG},
        {"port", required_argument, nullptr, OPT_PORT},
        {"buffer-ms", required_argument, nullptr, OPT_BUFFER_MS},
        {"static-delay", required_argument, nullptr, OPT_STATIC_DELAY},
        {"no-mdns", no_argument, nullptr, OPT_NO_MDNS},
        {"mdns-name", required_argument, nullptr, OPT_MDNS_NAME},
        {"control-socket", required_argument, nullptr, OPT_CONTROL_SOCKET},
        {"no-control", no_argument, nullptr, OPT_NO_CONTROL},
        {"state-dir", required_argument, nullptr, OPT_STATE_DIR},
        {"hook-start", required_argument, nullptr, OPT_HOOK_START},
        {"hook-stop", required_argument, nullptr, OPT_HOOK_STOP},
        {"id", required_argument, nullptr, OPT_ID},
        {"manufacturer", required_argument, nullptr, OPT_MANUFACTURER},
        {"product-name", required_argument, nullptr, OPT_PRODUCT_NAME},
        {"audio-format", required_argument, nullptr, OPT_AUDIO_FORMAT},
        {nullptr, 0, nullptr, 0},
    };

    // Taken off the front before getopt runs, so a subcommand's argument can look like a flag
    // (`seek-rel -5000`) and so the flags after it are seen on glibc and the BSDs alike.
    // Reported through the same deferral as every flag error below rather than immediately, so
    // that appending --help to a command line you got wrong still prints the flag list.
    ControlInvocation invocation;
    std::string subcommand_error;
    const bool split_ok = split_subcommand(argc, argv, invocation, subcommand_error);
    out.subcommand = invocation.name;
    out.subcommand_args = invocation.args;

    // getopt is handed a line with the subcommand words removed rather than being asked to skip
    // them, since the number to skip is not something optind can be told. Everything below
    // reads this line, so optind and offending_option() index the same array getopt scanned.
    std::vector<char*> flags;
    flags.push_back(argv[0]);
    for (int index = invocation.consumed == 0 ? 1 : invocation.consumed; index < argc; ++index) {
        flags.push_back(argv[index]);
    }
    // The array has to keep POSIX's `argv[argc] == NULL`, and not as a formality: the BSD
    // getopt_long behind `--port` with no value does `optarg = nargv[optind++]` unconditionally
    // and then tests `optarg == NULL`, so the sentinel is the *only* thing that tells it the
    // value is missing. Without it the read runs one past the end and a missing value is
    // accepted as whatever was next in memory.
    const int flag_argc = static_cast<int>(flags.size());
    flags.push_back(nullptr);
    char** const flag_argv = flags.data();

    reset_getopt();

    // The first thing that went wrong, reported only once the whole line has been read.
    //
    // Deferring it is what lets -h and --version win over an earlier bad flag: appending
    // --help to a command line you got wrong should print the flag list, not the same
    // error again. Deferred rather than pre-scanned for "--help", because only getopt
    // knows whether such a word is a flag or another flag's value -- `-n --help` names
    // the player "--help".
    std::string error;
    const auto fail = [&error](std::string message) {
        if (error.empty()) {
            error = std::move(message);
        }
    };
    // Every settable option goes through apply_option(), so nothing here can validate a typed
    // value differently from a configured one. This only adapts that function's plain error string
    // to the deferred, first-wins convention above.
    const auto apply = [&fail, &out, err](Opt opt, const char* value) {
        std::string message;
        if (!apply_option(settable_option(opt), value, out, message, err)) {
            fail(std::move(message));
        }
    };
    // Rejects an empty value for a flag whose empty case has no meaning. Only --config still needs
    // this on its own: every other value-taking flag is settable, so its emptiness rule lives in
    // apply_option() beside the rest of its validation.
    const auto require_value = [&fail](const char* flag, const char* value) {
        if (value[0] != '\0') {
            return true;
        }
        fail(std::string(flag) + " needs a non-empty value");
        return false;
    };

    // The subcommand's own two complaints, now that there is somewhere to defer them to: a name
    // that is not a subcommand, and an argument that is not what that subcommand takes. Both are
    // parse-time errors like any other, so a bad `vol 500` reads exactly like a bad --buffer-ms
    // rather than failing later, on the wire.
    if (!split_ok) {
        fail(std::move(subcommand_error));
    } else if (!out.subcommand.empty()) {
        ControlRequest request;
        std::string request_error;
        if (!parse_control_request(out.subcommand, out.subcommand_args, request, request_error)) {
            fail(std::move(request_error));
        }
    }

    // The leading ':' is what separates "you left the value off" from "no such flag":
    // getopt then returns ':' for a missing argument instead of folding it into '?'.
    int opt = 0;
    while ((opt = getopt_long(flag_argc, flag_argv, ":o:ln:s:zP:d:f:h", long_opts, nullptr)) !=
           -1) {
        switch (opt) {
            case 'o':
                apply(Opt::Device, optarg);
                break;
            case 'l':
                out.list_devices = true;
                out.mark_given(Opt::ListDevices);
                break;
            case 'n':
                apply(Opt::Name, optarg);
                break;
            case 's':
                // Only stored here; resolved once below, after the whole line parses.
                apply(Opt::Server, optarg);
                break;
            case 'z':
                out.daemonize = true;
                out.mark_given(Opt::Daemonize);
                break;
            case 'P':
                apply(Opt::Pidfile, optarg);
                break;
            case 'd':
                apply(Opt::LogLevel, optarg);
                break;
            case 'f':
                apply(Opt::Logfile, optarg);
                break;
            case 'h':
                // Nothing after --help can matter, so this is the one early return -- and it is
                // above the config file, so a broken config cannot stop --help explaining it.
                out.show_help = true;
                return true;
            case OPT_VERSION:
                out.show_version = true;
                return true;
            case OPT_CONFIG:
                if (require_value("--config", optarg)) {
                    out.config_path = optarg;
                    out.mark_given(Opt::Config);
                }
                break;
            case OPT_PORT:
                apply(Opt::Port, optarg);
                break;
            case OPT_BUFFER_MS:
                apply(Opt::BufferMs, optarg);
                break;
            case OPT_STATIC_DELAY:
                apply(Opt::StaticDelay, optarg);
                break;
            case OPT_NO_MDNS:
                // A switch on the command line carries its answer in its own name, so it supplies
                // the word a config file has to spell out.
                apply(Opt::NoMdns, "true");
                break;
            case OPT_MDNS_NAME:
                apply(Opt::MdnsName, optarg);
                break;
            case OPT_CONTROL_SOCKET:
                // Only stored here; the length check and the -z rewrite happen below, once --port
                // is known and the whole line has parsed.
                apply(Opt::ControlSocket, optarg);
                break;
            case OPT_NO_CONTROL:
                apply(Opt::NoControl, "true");
                break;
            case OPT_STATE_DIR:
                apply(Opt::StateDir, optarg);
                break;
            case OPT_HOOK_START:
                apply(Opt::HookStart, optarg);
                break;
            case OPT_HOOK_STOP:
                apply(Opt::HookStop, optarg);
                break;
            case OPT_ID:
                apply(Opt::ClientId, optarg);
                break;
            case OPT_MANUFACTURER:
                apply(Opt::Manufacturer, optarg);
                break;
            case OPT_PRODUCT_NAME:
                apply(Opt::ProductName, optarg);
                break;
            case OPT_AUDIO_FORMAT:
                apply(Opt::AudioFormat, optarg);
                break;
            case ':':
                fail("option '" + offending_option(flag_argv, optind) + "' needs a value");
                break;
            case '?':
            default:
                fail("unknown option '" + offending_option(flag_argv, optind) + "'");
                break;
        }
    }

    if (optind < flag_argc) {
        const std::string word = flag_argv[optind];
        if (find_control_subcommand(word) != nullptr) {
            // A real subcommand, just not where it can be read as one. Said outright rather
            // than as "unexpected argument", because the fix is to move one word.
            fail("a subcommand has to come first: '" + std::string(argv[0]) + " " + word +
                 " [flags]', not after the flags");
        } else {
            fail("unexpected argument '" + word + "' -- this player takes flags and one optional "
                 "subcommand (" + control_subcommand_list() + ")");
        }
    }

    // The config file is layered in **here**, and the position is the whole design. Above it,
    // `was_given()` still means exactly what the command line said, so the precedence rule is one
    // test per option. Below it, everything -- the -s URL resolution, the -z-with-stdout
    // contradiction, --no-control against --control-socket, and the socket path's absolutization
    // and `sun_path` length check -- runs over the merged options without knowing a file was
    // involved. That is the only ordering in which a configured value is validated identically to
    // a typed one, and it satisfies the socket path's "resolved before the length check" constraint
    // for free.
    //
    // Skipped once something has already failed, so the first complaint stays the useful one, and
    // skipped for -l: a broken config must not stop the device list. (-h and --version returned
    // above, so they never reach this.)
    // Where each merged value came from, for the two refusals below that happen after the merge.
    std::map<Opt, std::string> config_origin;
    if (error.empty() && !out.list_devices) {
        ConfigFile config;
        std::string reason;
        if (!load_config_file(out.was_given(Opt::Config) ? out.config_path : std::string(),
                              config_search_paths(), config, reason)) {
            fail(std::move(reason));
        } else {
            // Left here for the startup log to name, empty when nothing was found.
            out.config_path = config.path;
            if (!merge_config(config, !out.subcommand.empty(), out, config_origin, reason, err)) {
                fail(std::move(reason));
            }
        }
    }

    // Adds the config file and line to a message about one option, when that option came from a
    // file. A typed value keeps the bare message it always had, since there is nowhere to point at.
    const auto fail_for = [&fail, &config_origin](Opt opt, std::string message) {
        const auto found = config_origin.find(opt);
        fail(found == config_origin.end() ? std::move(message) : found->second + message);
    };

    // Skipped once something has already failed: the first complaint is the useful one,
    // and -s cannot be resolved from a line we are not going to act on anyway.
    if (error.empty() && out.was_given(Opt::Server)) {
        if (parse_discovery_spec(out.server, out.discover_name)) {
            out.discover = true;
#ifndef SENDSPIN_CLI_HAVE_MDNS
            // Refused here rather than at startup, for the reason -o gives a backend this
            // build lacks: a flag that parses and then quietly discovers nothing is worse
            // than one that says the build cannot do it.
            out.discover = false;
            // Redacted like every other message that quotes a -s value. A discovery spec is a
            // service instance name and has no userinfo to carry, so the helper is a no-op on
            // any sane one -- but `mdns:` is only reserved before the first colon, so what
            // follows it is whatever was typed, and this is the one message that prints it.
            fail_for(Opt::Server,
                     "-s '" + redact_url_userinfo(out.server) +
                         "': this build has no mDNS support, so it cannot discover a server. "
                         "Rebuild with dns_sd.h available (libavahi-compat-libdnssd-dev on "
                         "Debian/Ubuntu, avahi-compat-libdns_sd-devel on Fedora), or give -s an "
                         "address.");
#endif
        } else {
            std::string reason;
            if (!parse_server_url(out.server, out.server_url, reason)) {
                fail_for(Opt::Server, std::move(reason));
            }
        }
    }

    // Contradictory rather than inert, so it fails: -z points stdout at /dev/null, which
    // would turn the PCM sink into a second discard sink without saying so. Resolved through
    // resolve_device_spec() rather than by comparing strings, so a future spelling of the
    // stdout sink is covered too; a spec that does not resolve at all is left to
    // make_audio_sink() to report, as it always was.
    if (error.empty() && out.was_given(Opt::Daemonize) && out.was_given(Opt::Device)) {
        DeviceSpec spec;
        std::string reason;
        if (resolve_device_spec(out.device, spec, reason) && spec.backend == SinkBackend::Stdout) {
            fail("-z cannot write PCM to stdout: a daemon's stdout is /dev/null, so -o '" +
                 out.device + "' would discard every stream. Drop -z, or pick a real device.");
        }
    }

    // Contradictory rather than inert, like -z with -o stdout: one flag names where the control
    // socket goes and the other says there is not one, and guessing which the operator meant
    // would leave a player either unreachable or listening where they said it should not.
    if (error.empty() && out.was_given(Opt::NoControl) && out.was_given(Opt::ControlSocket)) {
        fail("--no-control and --control-socket '" + out.control_socket +
             "' contradict each other -- drop one");
    }

    // Resolved here, above the error report, because the path's own length is one of the things
    // that can fail: --control-socket is made absolute *first*, since a relative path grows
    // when the working directory is prepended and it is the resolved one that has to fit.
    if (error.empty()) {
        // --no-control only decides whether *this* process listens, so a subcommand run ignores
        // it and resolves the path anyway: the player it is talking to made its own decision, and
        // clearing the path here would have the subcommand blame a flag on the wrong command line.
        if (out.no_control && out.subcommand.empty()) {
            out.control_socket.clear();
        } else if (out.was_given(Opt::ControlSocket)) {
            if (out.was_given(Opt::Daemonize)) {
                out.control_socket = absolute_path(out.control_socket);
            }
            if (!control_socket_path_fits(out.control_socket)) {
                // Refused rather than truncated: a shortened path binds a socket nothing can
                // find, and every subcommand would then report "no daemon" against a daemon
                // that is running and healthy.
                fail_for(Opt::ControlSocket,
                         "--control-socket '" + out.control_socket + "' is " +
                             std::to_string(out.control_socket.size()) +
                             " bytes, and a Unix socket address holds at most " +
                             std::to_string(control_socket_path_limit() - 1) + " on this platform");
            }
        } else {
            const ControlRuntimeDir runtime = control_runtime_dir();
            out.control_socket = control_socket_path(runtime.path, out.port);
            out.control_absent_reason = control_socket_absent_reason(runtime, out.control_socket);
            if (!out.control_absent_reason.empty()) {
                // Non-fatal, and deliberately not a fallback to a shared directory: the player
                // is still a player without a control channel. main() warns once and carries on.
                out.control_socket.clear();
            }
            // A directory that is being *used* despite failing the privacy check. Said here
            // rather than in main(), because it is a property of the flags and the environment
            // rather than of the run -- and only for a daemon, which is the process that creates
            // the socket and so the one making the decision.
            if (!runtime.warning.empty() && out.subcommand.empty()) {
                std::fprintf(err, "warning: %s\n", runtime.warning.c_str());
            }
        }
    }

    if (!error.empty()) {
        std::fprintf(err, "error: %s\n", error.c_str());
        return false;
    }

    // A subcommand run starts no player, so the warnings differ: the ones below describe a
    // daemon that is not going to exist, and what is worth saying instead is that most of the
    // flags did nothing. Warned rather than refused, because the natural mistake is pasting a
    // daemon's whole flag line and appending a subcommand -- which should still work.
    if (!out.subcommand.empty()) {
        // --no-control is in here rather than treated as a contradiction the way it is alongside
        // --control-socket: it says nothing about *this* invocation, since a subcommand does not
        // listen on anything. Left it out and it would silently produce a "this player was
        // started with --no-control" message about the wrong process.
        static constexpr Opt DAEMON_ONLY[] = {
            Opt::Device,    Opt::Name,         Opt::Server,      Opt::Daemonize,   Opt::Pidfile,
            Opt::Logfile,   Opt::LogLevel,     Opt::BufferMs,    Opt::NoMdns,      Opt::MdnsName,
            Opt::NoControl, Opt::StateDir,     Opt::StaticDelay, Opt::HookStart,   Opt::HookStop,
            Opt::ClientId,  Opt::Manufacturer, Opt::ProductName, Opt::AudioFormat,
        };
        for (Opt opt : DAEMON_ONLY) {
            if (out.was_given(opt)) {
                std::fprintf(err,
                             "warning: a subcommand reads only --port and --control-socket -- the "
                             "other flags configure a player and do nothing here\n");
                break;
            }
        }
    } else {
        // Not refused: a daemon with nowhere to log is still a working player, and -z is often
        // paired with a supervisor that does not want a logfile. Warned about because the
        // alternative is a silence that reads exactly like a crash.
        if (out.was_given(Opt::Daemonize) && !out.was_given(Opt::Logfile)) {
            std::fprintf(err,
                         "warning: -z without -f discards all log output -- a detached daemon's "
                         "stderr is /dev/null. Add -f <path> to keep it.\n");
        }

        // Inert rather than contradictory, so it warns instead of failing: -s picks the outbound
        // mode, which the spec forbids advertising alongside, so there is no instance to name.
        if (out.was_given(Opt::MdnsName) && out.was_given(Opt::Server)) {
            std::fprintf(err,
                         "warning: --mdns-name is unused with -s -- a client that dials out must "
                         "not advertise %s, so there is no instance to name\n",
                         MDNS_CLIENT_SERVICE);
        }
    }

    // Normalized here, and only under -z, so one value means one file everywhere downstream
    // and a foreground run's paths and diagnostics read exactly as they always did.
    if (out.was_given(Opt::Daemonize)) {
        if (out.was_given(Opt::Pidfile)) {
            out.pidfile = absolute_path(out.pidfile);
        }
        if (out.was_given(Opt::Logfile)) {
            out.logfile = absolute_path(out.logfile);
        }
        // Same hazard as those two, and the state file is written for the life of the daemon rather
        // than once at startup: a relative --state-dir names the directory the operator typed it in
        // to this process and one directly under / to the child that does the writing.
        if (out.was_given(Opt::StateDir)) {
            out.state_dir = absolute_path(out.state_dir);
        }
    }

    if (out.name.empty()) {
        out.name = default_client_name();
    }
    if (out.mdns_name.empty()) {
        out.mdns_name = out.name;
    }
    return true;
}

void print_usage(std::FILE* out, const char* prog) {
    std::fprintf(out, "Usage: %s [options]\n", prog);
    std::fprintf(out, "       %s <subcommand> [args] [--port <port>] [--control-socket <path>]\n\n",
                 prog);
    std::fprintf(out, "A headless Sendspin audio player. Listens for a Sendspin server to\n");
    std::fprintf(out, "connect to it, or dials one with -s.\n\n");
    std::fprintf(out, "With a subcommand, it instead talks to a player already running on this\n");
    std::fprintf(out,
                 "host over its control socket, and exits. The subcommand must come first.\n\n");
    std::fprintf(out, "Subcommands:\n");
    for (const ControlSubcommand& subcommand : control_subcommands()) {
        // The name and its argument in one column so the shape is copyable, and the
        // description wrapped under it -- the two long ones do not fit beside the name.
        std::string invocation = subcommand.name;
        if (subcommand.argument != nullptr) {
            invocation += " ";
            invocation += subcommand.argument;
        }
        std::fprintf(out, "  %-20s", invocation.c_str());
        print_wrapped(out, subcommand.description, 22);
    }
    std::fprintf(out, "\n");
    std::fprintf(out, "  A subcommand needs the same --port as the player, or an explicit\n");
    std::fprintf(out, "  --control-socket: the default socket path carries the serve port, so\n");
    std::fprintf(out, "  a player on a non-default --port has its socket somewhere else.\n\n");
    std::fprintf(out, "  Exit status: 0 sent (or printed), 1 bad command line, 2 the player\n");
    std::fprintf(out, "  refused the argument, 3 no player listening there, 4 the player has\n");
    std::fprintf(out, "  no server connection, 5 the server does not offer that command,\n");
    std::fprintf(out, "  6 the exchange broke down.\n\n");
    std::fprintf(out, "Options:\n");
    std::fprintf(out, "  Every option below except -l, -z, --config, --help and --version can\n");
    std::fprintf(out, "  also be set in a config file, one 'key = value' per line, where the\n");
    std::fprintf(out, "  key is the long flag name without its dashes ('buffer-ms = 150').\n");
    std::fprintf(out, "  '#' starts a comment at the start of a line. The command line wins.\n");
    std::fprintf(out, "  The first of these that exists is read whole; none is fine:\n");
    for (const std::string& path : config_search_paths()) {
        std::fprintf(out, "    %s\n", path.c_str());
    }
    std::fprintf(out, "\n");
    std::fprintf(out, "  -o, --output <device>\n");
    std::fprintf(out, "                Output device (default: %s). Either a reserved name\n",
                 DEFAULT_OUTPUT_DEVICE);
    std::fprintf(out, "                (null, stdout, -), or a <backend>:<device> pair split on\n");
    std::fprintf(out, "                the first colon, where <backend> is one of: %s\n",
                 audio_backend_list().c_str());
#ifdef SENDSPIN_CLI_HAVE_ALSA
    std::fprintf(out, "                Anything else is an ALSA PCM name: -o hw:2,0, -o default\n");
#endif
    std::fprintf(out, "                -l lists this host's devices and what they accept\n");
    std::fprintf(out, "  -l            List output devices with their capabilities, and exit\n");
    std::fprintf(out, "  -n, --name <name>\n");
    std::fprintf(out, "                Friendly name (default: this host's name)\n");
    std::fprintf(out, "  --id <id>     Stable client id, which is what a server files this\n");
    std::fprintf(out, "                player's volume, group and pairing under -- -n is only\n");
    std::fprintf(out, "                what it displays. Defaults to an id derived from the\n");
    std::fprintf(out, "                network interface MAC, which two players on one host\n");
    std::fprintf(out, "                would share: give each its own --id (and its own\n");
    std::fprintf(out, "                --port and --state-dir)\n");
    std::fprintf(out, "  -s, --server <server>\n");
    std::fprintf(out, "                Connect out to <host>[:<port>] or a ws:// URL\n");
    std::fprintf(out, "                (the server's port defaults to %u), retrying until it\n",
                 DEFAULT_REMOTE_SERVER_PORT);
    std::fprintf(out, "                answers. Any -s turns off the mDNS advertisement: the\n");
    std::fprintf(out, "                spec forbids advertising %s while\n", MDNS_CLIENT_SERVICE);
    std::fprintf(out, "                the client is the one initiating the connection\n");
#ifdef SENDSPIN_CLI_HAVE_MDNS
    std::fprintf(out, "                -s %s<name> instead discovers a server over mDNS,\n",
                 DISCOVERY_PREFIX);
    std::fprintf(out, "                by its advertised name; -s %s takes any server.\n",
                 DISCOVERY_PREFIX);
    std::fprintf(out, "                '%s' is reserved before the first colon only, so a\n",
                 DISCOVERY_PREFIX);
    std::fprintf(out, "                bare -s mdns is still a host called mdns\n");
#else
    std::fprintf(out, "                (-s %s<name> discovery needs mDNS, which this build\n",
                 DISCOVERY_PREFIX);
    std::fprintf(out, "                does not have)\n");
#endif
    std::fprintf(out, "  -z            Fork into the background and detach from the terminal.\n");
    std::fprintf(out, "                Refuses -o stdout, whose output would go to /dev/null;\n");
    std::fprintf(out, "                warns without -f, which is where the log would go\n");
    std::fprintf(out, "  -P, --pidfile <path>\n");
    std::fprintf(out, "                Hold <path> as a locked pidfile, refusing to start if\n");
    std::fprintf(out, "                another instance already holds it. A file left by a\n");
    std::fprintf(out, "                crash needs no cleanup; keep it on a local filesystem\n");
    std::fprintf(out, "  -d, --log-level <level>\n");
    std::fprintf(out, "                Log level: none, error, warn, info, debug, verbose\n");
    std::fprintf(out, "                One level for this player and the sendspin library\n");
    std::fprintf(out, "                together. Accepts squeezelite's <category>=<level>\n");
    std::fprintf(out, "                shape, but the category is ignored: every line is\n");
    std::fprintf(out, "                '<L> <tag>: <message>', so filter it with grep\n");
    std::fprintf(out, "  -f, --logfile <path>\n");
    std::fprintf(out, "                Write log output to <path> instead of stderr, with a\n");
    std::fprintf(out, "                UTC timestamp on every line. SIGHUP reopens the path,\n");
    std::fprintf(out, "                so logrotate and newsyslog can rotate it\n");
    std::fprintf(out, "  --config <path>\n");
    std::fprintf(out, "                Read this config file instead of searching. Exits 1 if\n");
    std::fprintf(out, "                it cannot be read, since you named it -- falling back\n");
    std::fprintf(out, "                would start a player on options nobody chose\n");
    std::fprintf(out, "  --port <port> Port our own server listens on (default: %u)\n",
                 SendspinClientConfig::DEFAULT_SERVER_PORT);
    std::fprintf(out, "  --buffer-ms <ms>\n");
    std::fprintf(out, "                Audio the output backend keeps buffered, %u-%u\n",
                 MIN_BUFFER_MS, MAX_BUFFER_MS);
    std::fprintf(out, "                (default: %u). One figure for every backend; a\n",
                 DEFAULT_BUFFER_MS);
    std::fprintf(out, "                device-less sink ignores it, and a device that needs\n");
    std::fprintf(out, "                more than it asks for gets more\n");
    std::fprintf(out, "  --audio-format <codec:rate:depth:channels>\n");
    std::fprintf(out, "                Pin a preferred format, e.g. flac:48000:24:2 -- for the\n");
    std::fprintf(out, "                DAC that is only happy in one shape. Moves that entry to\n");
    std::fprintf(out, "                the front of the advertised list, where a server picks\n");
    std::fprintf(out, "                first; the rest of what the device takes is still\n");
    std::fprintf(out, "                offered behind it. A format the advertised list does\n");
    std::fprintf(out, "                not carry refuses to start -- run -l to see what the\n");
    std::fprintf(out, "                device reports -- not the same set as what goes out\n");
    std::fprintf(out, "  --static-delay <ms>\n");
    std::fprintf(out, "                How much latency this endpoint's hardware adds AFTER the\n");
    std::fprintf(out, "                audio port -- an amplifier, an external speaker, a DSP.\n");
    std::fprintf(out, "                0-%u, default 0. The player hands audio to the device\n",
                 MAX_STATIC_DELAY_MS);
    std::fprintf(out, "                that much EARLIER to compensate, so the sound lands in\n");
    std::fprintf(out, "                sync with the group rather than late. It does not push\n");
    std::fprintf(out, "                this speaker later than the others.\n");
    std::fprintf(out, "                A FIRST-RUN DEFAULT only: a delay a server or\n");
    std::fprintf(out, "                'sendspin-cli delay' has set is remembered, and the\n");
    std::fprintf(out, "                remembered one wins over this flag every run after.\n");
    std::fprintf(out, "                Use 'delay <ms>' to change a running player\n");
    std::fprintf(out, "  --no-mdns     Do not advertise over mDNS (no effect with -s, which\n");
    std::fprintf(out, "                already suppresses it)\n");
    std::fprintf(out, "  --mdns-name <name>\n");
    std::fprintf(out, "                Instance name to advertise (default: -n). Unused with -s\n");
    std::fprintf(out, "  --control-socket <path>\n");
    std::fprintf(out, "                Unix socket the subcommands above talk to, mode 0600.\n");
    std::fprintf(out, "                Defaults to %s<port>%s in\n", CONTROL_SOCKET_PREFIX,
                 CONTROL_SOCKET_SUFFIX);
    std::fprintf(out, "                $XDG_RUNTIME_DIR, where that is set. The <port> is\n");
    std::fprintf(out, "                --port, so two players on one host each get their own --\n");
    std::fprintf(out, "                and a subcommand needs the same --port or this flag\n");
#ifdef __APPLE__
    std::fprintf(out, "                Where it is not set, as on macOS, this host's own\n");
    std::fprintf(out, "                per-user directory is used instead. Never /tmp, which\n");
    std::fprintf(out, "                would let any local user drive this player\n");
#else
    std::fprintf(out, "                Where it is not set -- a systemd *system* unit has none,\n");
    std::fprintf(out, "                so pair RuntimeDirectory= with this flag -- there is no\n");
    std::fprintf(out, "                default and the player warns once. There is deliberately\n");
    std::fprintf(out, "                no /tmp fallback, which would let any local user drive\n");
    std::fprintf(out, "                this player\n");
#endif
    std::fprintf(out, "  --no-control  Do not listen on a control socket at all\n");
    std::fprintf(out, "  --state-dir <dir>\n");
    std::fprintf(out, "                Where this player keeps what it remembers across\n");
    std::fprintf(out, "                restarts -- the last server, the static delay a server\n");
    std::fprintf(out, "                set, and its volume and mute. Defaults to\n");
    std::fprintf(out, "                $XDG_STATE_HOME/sendspin-cli, or\n");
    std::fprintf(out, "                $HOME/.local/state/sendspin-cli. A systemd *system*\n");
    std::fprintf(out, "                unit has neither, so pair StateDirectory= with this\n");
    std::fprintf(out, "                flag; with none of the three the player still runs and\n");
    std::fprintf(out, "                simply remembers nothing\n");
    std::fprintf(out, "  --manufacturer <text>\n");
    std::fprintf(out, "  --product-name <text>\n");
    std::fprintf(out, "                The device info client/hello carries, shown in server\n");
    std::fprintf(out, "                device lists (defaults: sendspin-cpp-cli, sendspin-cli).\n");
    std::fprintf(out, "                For a product that embeds this player and should be\n");
    std::fprintf(out, "                listed as itself\n");
    std::fprintf(out, "  --hook-start <command>\n");
    std::fprintf(out, "  --hook-stop <command>\n");
    std::fprintf(out, "                Run <command> through /bin/sh when a stream starts or\n");
    std::fprintf(out, "                stops -- an amplifier relay, a light. The event's facts\n");
    std::fprintf(out, "                arrive as SENDSPIN_EVENT (start|stop) and, where known,\n");
    std::fprintf(out, "                SENDSPIN_SERVER_ID, SENDSPIN_SERVER_NAME,\n");
    std::fprintf(out, "                SENDSPIN_SERVER_URL (outbound only), SENDSPIN_CLIENT_ID\n");
    std::fprintf(out, "                and SENDSPIN_CLIENT_NAME. The hook runs without blocking\n");
    std::fprintf(out, "                playback; its output goes to the log, and a non-zero\n");
    std::fprintf(out, "                exit is a warning, not a player failure\n");
    std::fprintf(out, "  -h, --help    Show this help\n");
    std::fprintf(out, "  --version     Show version information\n\n");
    std::fprintf(out,
                 "This is an early scaffold; see docs/ROADMAP.md for what is still missing.\n");
}

void print_version(std::FILE* out) {
    std::fprintf(out, "sendspin-cli %s\n", SENDSPIN_CLI_VERSION);
    std::fprintf(out, "sendspin-cpp %s\n", SENDSPIN_CLI_LIB_TAG);
}

bool parse_discovery_spec(const std::string& server, std::string& name) {
    const std::string prefix = DISCOVERY_PREFIX;
    if (server.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    name = server.substr(prefix.size());
    return true;
}

bool parse_server_url(const std::string& server, std::string& url, std::string& error) {
    if (server.empty()) {
        error = "-s needs a server: <host>[:<port>], or a full ws:// URL";
        return false;
    }

    // Every message below quotes the value the operator typed back at them, and that value may
    // carry credentials -- so none of them quotes it raw, nor any fragment of it. A value with
    // no userinfo, which is every well-formed one, comes back exactly as written.
    const std::string shown = redact_url_userinfo(server);

    // A scheme means the caller is spelling out the whole URL -- path, port and all -- so
    // the only thing to check is that it is one we can actually speak.
    const size_t scheme_end = server.find("://");
    if (scheme_end != std::string::npos) {
        const std::string scheme = server.substr(0, scheme_end);
        if (scheme != "ws" && scheme != "wss") {
            error = "-s '" + shown + "': Sendspin runs over WebSocket, so the scheme must be " +
                    "ws:// or wss://, not " + scheme + "://";
            return false;
        }
        // The rest is the caller's to get right -- port, path and all -- but a scheme with
        // nothing after it names no server at all, and would fail far from here.
        if (scheme_end + 3 == server.size()) {
            error = "-s '" + shown + "': a scheme but no host";
            return false;
        }
        url = server;
        return true;
    }

    std::string host;
    std::string port_text;
    bool has_port = false;

    if (server.front() == '[') {
        // A bracketed IPv6 literal keeps its brackets in the URL; only what follows the
        // closing bracket can be a port.
        const size_t bracket = server.find(']');
        if (bracket == std::string::npos) {
            error = "-s '" + shown + "': unterminated '[' -- an IPv6 literal reads [::1]:8927";
            return false;
        }
        host = server.substr(0, bracket + 1);
        const std::string rest = server.substr(bracket + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                error = "-s '" + shown + "': expected ':<port>' after ']', got '" +
                        redact_url_userinfo(rest) + "'";
                return false;
            }
            port_text = rest.substr(1);
            has_port = true;
        }
    } else {
        const size_t colon = server.find(':');
        if (colon == std::string::npos) {
            host = server;
        } else if (server.find(':', colon + 1) != std::string::npos) {
            // More than one colon and no brackets: an IPv6 literal written bare, where
            // there is no way to tell the address's colons from a port separator.
            error = "-s '" + shown + "': an IPv6 literal must be bracketed -- try '[" + shown +
                    "]' or '[" + shown + "]:<port>'";
            return false;
        } else {
            host = server.substr(0, colon);
            port_text = server.substr(colon + 1);
            has_port = true;
        }
    }

    // "[]" is as empty a host as "".
    if (host.empty() || host == "[]") {
        error = "-s '" + shown + "': no host before the port";
        return false;
    }

    // Only when no ':' was written at all does the server's own default apply. A written
    // but empty port is a truncated line, not a request for the default.
    uint16_t port = DEFAULT_REMOTE_SERVER_PORT;
    if (has_port && !parse_port(port_text, port)) {
        error = "-s '" + shown + "': '" + redact_url_userinfo(port_text) +
                "' is not a port number (expected 1-65535)";
        return false;
    }

    url = "ws://" + host + ":" + std::to_string(port) + SENDSPIN_PATH;
    return true;
}

std::string redact_url_userinfo(const std::string& url) {
    // What can carry userinfo is the authority, and only the authority: it starts after the
    // scheme when there is one and at the front when there is not -- a rejected -s value is a
    // bare authority -- and ends at the first delimiter that closes it. Ending it there is what
    // keeps an '@' further along, in a path or a query, from reading as a credential separator.
    const size_t scheme_end = url.find("://");
    const size_t begin = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    const size_t end = url.find_first_of("/?#", begin);
    const std::string authority =
        url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);

    // The *last* '@' in it: a host cannot contain one and a userinfo field can, so anything
    // before the final '@' is userinfo however many it holds.
    const size_t at = authority.rfind('@');
    if (at == std::string::npos) {
        return url;
    }
    const std::string userinfo = authority.substr(0, at);

    // The *first* ':': everything after it is one password, colons and all, so splitting on the
    // last would print a prefix of the secret.
    const size_t colon = userinfo.find(':');
    std::string masked;
    if (colon == std::string::npos) {
        // Nothing to hide at all, or one field that could be a bearer token and so goes whole.
        if (userinfo.empty()) {
            return url;
        }
        masked = USERINFO_MASK;
    } else if (colon + 1 == userinfo.size()) {
        // A username and an empty password: masking would invent a secret that is not there.
        return url;
    } else {
        masked = userinfo.substr(0, colon + 1) + USERINFO_MASK;
    }

    return url.substr(0, begin) + masked + url.substr(begin + at);
}

std::string default_client_name() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return FALLBACK_NAME;
    }
    // POSIX does not promise termination when the name does not fit.
    hostname[sizeof(hostname) - 1] = '\0';
    return hostname[0] == '\0' ? FALLBACK_NAME : hostname;
}

}  // namespace sendspin_cli
