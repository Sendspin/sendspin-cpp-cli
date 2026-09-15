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

/// Long-only option values, outside the short-option alphabet.
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

/// An option a config file may set: its key (the long flag name) and how diagnostics name it.
struct SettableOption {
    Opt opt;
    const char* key;

    /// The spelling diagnostics use; the short form where one exists.
    const char* flag;
};

/// The options a config file may set: `Opt` minus -l, -z, --config, --help and --version.
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

/// The entry for `opt`, or a fallback apply_option() refuses when the table has none.
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
/// Digits only because strtoul accepts " 8927" and "+8927" and reads "-1" as a huge value.
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

/// Names the option getopt complained about: optopt for a short one, the argv word for a long one.
std::string offending_option(char* const argv[], int index) {
    const char* word = argv[index - 1];
    const bool is_long = word[0] == '-' && word[1] == '-';
    if (!is_long && optopt != 0) {
        return std::string("-") + static_cast<char>(optopt);
    }
    return word;
}

/// Maps a level name onto LogLevel, accepting common names (info, debug, sdebug) too.
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

/// Parses `-d [<category>=]<level>`; the category is ignored, as the library has one level.
/// @param err Where the ignored-category warning goes, or nullptr to suppress it.
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

/// Parses the word a config file gives a switch flag; generous about spelling.
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

/// Applies one option value to `out`: the single path for typed and configured values alike.
/// @param error Set to the diagnostic, without saying where the value came from.
/// @return true when accepted, in which case `opt` is also marked as supplied.
bool apply_option(const SettableOption& option, const std::string& value, Options& out,
                  std::string& error, std::FILE* err) {
    // Refuses an empty value for a flag where empty means nothing.
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
            // Emptiness is left to the -s resolution, which explains what -s takes.
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
            // Stored only; resolved once --port and the config file are known.
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
            std::string reason;
            if (!parse_format_list(value, out.audio_formats, reason)) {
                error = "invalid --audio-format '" + value + "': " + reason;
                return false;
            }
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
            // Listed, not defaulted, so a new Opt fails to compile until handled here.
            error = "internal: " + std::string(option.flag) + " cannot be set this way";
            return false;
    }
    out.mark_given(option.opt);
    return true;
}

/// Fills options the command line did not supply from `config`, marking each as supplied.
/// Marking matters: advertises() and the socket checks key off was_given().
/// @param subcommand_run Apply only --port and --control-socket, but still validate the rest.
/// @param origin Filled with `<file>:<line>: ` per supplied option, for later diagnostics.
/// @return false when there is an error to report.
bool merge_config(const ConfigFile& config, bool subcommand_run, Options& out,
                  std::map<Opt, std::string>& origin, std::string& error, std::FILE* err) {
    // Last wins within one file, resolved up front.
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
            // Unknown keys are fatal, including real flags that are not settable.
            error = where + "unknown key '" + entry.key + "'";
            return false;
        }
        if (entry.line != last_line[entry.key]) {
            continue;
        }
        // The command line wins.
        if (out.was_given(option->opt)) {
            continue;
        }
        // A subcommand applies only these two; the rest is validated into a scratch copy.
        const bool applies =
            !subcommand_run || option->opt == Opt::Port || option->opt == Opt::ControlSocket;
        Options scratch;
        Options& target = applies ? out : scratch;

        std::string message;
        // No warnings for values nothing will act on.
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

/// `path` made absolute against the cwd, for -z: the daemon chdir()s to /.
std::string absolute_path(const std::string& path) {
    if (!path.empty() && path.front() == '/') {
        return path;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        return path;
    }
    return std::string(cwd) + "/" + path;
}

/// The column --help wraps at.
constexpr size_t USAGE_WIDTH = 79;

/// Word-wraps `text` at column `indent`, where the cursor already is, and ends the line.
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

/// Resets getopt's global state so parse_options() can run more than once in a process.
void reset_getopt() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    optreset = 1;
    optind = 1;
#else
    optind = 0;
#endif
    // We report errors ourselves.
    opterr = 0;
}

}  // namespace

bool parse_options(int argc, char* argv[], Options& out, std::FILE* err) {
    static const struct option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, OPT_VERSION},
        // Long aliases for the short letters, so every config key is a flag name.
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

    // Split off before getopt, so `seek-rel -5000` parses; errors defer like any flag error.
    ControlInvocation invocation;
    std::string subcommand_error;
    const bool split_ok = split_subcommand(argc, argv, invocation, subcommand_error);
    out.subcommand = invocation.name;
    out.subcommand_args = invocation.args;

    // getopt scans a copy of argv without the subcommand words.
    std::vector<char*> flags;
    flags.push_back(argv[0]);
    for (int index = invocation.consumed == 0 ? 1 : invocation.consumed; index < argc; ++index) {
        flags.push_back(argv[index]);
    }
    // Keep argv[argc] == NULL: BSD getopt_long relies on it to detect a missing value.
    const int flag_argc = static_cast<int>(flags.size());
    flags.push_back(nullptr);
    char** const flag_argv = flags.data();

    reset_getopt();

    // First error, reported after the whole line is read so -h and --version still win.
    std::string error;
    const auto fail = [&error](std::string message) {
        if (error.empty()) {
            error = std::move(message);
        }
    };
    // Every settable option goes through apply_option(); this adapts its error to first-wins.
    const auto apply = [&fail, &out, err](Opt opt, const char* value) {
        std::string message;
        if (!apply_option(settable_option(opt), value, out, message, err)) {
            fail(std::move(message));
        }
    };
    // Only --config still needs its own emptiness check.
    const auto require_value = [&fail](const char* flag, const char* value) {
        if (value[0] != '\0') {
            return true;
        }
        fail(std::string(flag) + " needs a non-empty value");
        return false;
    };

    // The subcommand's own errors, deferred like any other.
    if (!split_ok) {
        fail(std::move(subcommand_error));
    } else if (!out.subcommand.empty()) {
        ControlRequest request;
        std::string request_error;
        if (!parse_control_request(out.subcommand, out.subcommand_args, request, request_error)) {
            fail(std::move(request_error));
        }
    }

    // The leading ':' makes getopt return ':' for a missing value instead of '?'.
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
                // Stored only; resolved below.
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
                // Above the config merge, so a broken config cannot stop --help.
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
                apply(Opt::NoMdns, "true");
                break;
            case OPT_MDNS_NAME:
                apply(Opt::MdnsName, optarg);
                break;
            case OPT_CONTROL_SOCKET:
                // Stored only; resolved below once --port is known.
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
            fail("a subcommand has to come first: '" + std::string(argv[0]) + " " + word +
                 " [flags]', not after the flags");
        } else {
            fail("unexpected argument '" + word +
                 "' -- this player takes flags and one optional "
                 "subcommand (" +
                 control_subcommand_list() + ")");
        }
    }

    // Merge the config here: every check below then treats configured and typed values alike.
    // Where each merged value came from, for the two refusals below that happen after the merge.
    std::map<Opt, std::string> config_origin;
    if (error.empty() && !out.list_devices) {
        ConfigFile config;
        std::string reason;
        if (!load_config_file(out.was_given(Opt::Config) ? out.config_path : std::string(),
                              config_search_paths(), config, reason)) {
            fail(std::move(reason));
        } else {
            out.config_path = config.path;
            if (!merge_config(config, !out.subcommand.empty(), out, config_origin, reason, err)) {
                fail(std::move(reason));
            }
        }
    }

    // Prefixes the config file and line when the option came from a file.
    const auto fail_for = [&fail, &config_origin](Opt opt, std::string message) {
        const auto found = config_origin.find(opt);
        fail(found == config_origin.end() ? std::move(message) : found->second + message);
    };

    if (error.empty() && out.was_given(Opt::Server)) {
        if (!parse_discovery_spec(out.server, out.discover_name)) {
            // Hard error; the value is not quoted, since an address can carry credentials.
            std::string message = "connecting to an address with -s was removed: the Sendspin "
                                  "spec only has a player connect to a server it has discovered.";
            if (out.server == "mdns") {
                message += " Did you mean -s mdns:?";
            }
#ifdef SENDSPIN_CLI_HAVE_MDNS
            message += " Use -s mdns: for any server or -s mdns:<name> for one, or drop -s and "
                       "let a server discover this player.";
#else
            message += " This build has no mDNS support, so it cannot discover one either: drop "
                       "-s and point a server at ws://<this-host>:" +
                       std::to_string(out.port) + SENDSPIN_PATH + ".";
#endif
            fail_for(Opt::Server, std::move(message));
        } else {
            out.discover = true;
#ifndef SENDSPIN_CLI_HAVE_MDNS
            // Refused at parse time rather than quietly discovering nothing.
            out.discover = false;
            fail_for(Opt::Server,
                     "-s mdns: needs mDNS, and this build has no mDNS support, so it cannot "
                     "discover a server. Rebuild with dns_sd.h available "
                     "(libavahi-compat-libdnssd-dev on Debian/Ubuntu, "
                     "avahi-compat-libdns_sd-devel on Fedora), or drop -s and point a server at "
                     "ws://<this-host>:" +
                         std::to_string(out.port) + SENDSPIN_PATH + ".");
#endif
        }
    }

    // -z sends stdout to /dev/null, so -o stdout under -z is a contradiction.
    if (error.empty() && out.was_given(Opt::Daemonize) && out.was_given(Opt::Device)) {
        DeviceSpec spec;
        std::string reason;
        if (resolve_device_spec(out.device, spec, reason) && spec.backend == SinkBackend::Stdout) {
            fail("-z cannot write PCM to stdout: a daemon's stdout is /dev/null, so -o '" +
                 out.device + "' would discard every stream. Drop -z, or pick a real device.");
        }
    }

    // --no-control with --control-socket is contradictory, so refuse it.
    if (error.empty() && out.was_given(Opt::NoControl) && out.was_given(Opt::ControlSocket)) {
        fail("--no-control and --control-socket '" + out.control_socket +
             "' contradict each other -- drop one");
    }

    // Absolutized before the length check, since the resolved path is what must fit.
    if (error.empty()) {
        // --no-control governs only a daemon; a subcommand still needs the path.
        if (out.no_control && out.subcommand.empty()) {
            out.control_socket.clear();
        } else if (out.was_given(Opt::ControlSocket)) {
            if (out.was_given(Opt::Daemonize)) {
                out.control_socket = absolute_path(out.control_socket);
            }
            if (!control_socket_path_fits(out.control_socket)) {
                // Refused, not truncated: a shortened path binds a socket nothing finds.
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
                // Non-fatal, and never a shared-directory fallback.
                out.control_socket.clear();
            }
            // Only the daemon creates the socket, so only it warns about the directory.
            if (!runtime.warning.empty() && out.subcommand.empty()) {
                std::fprintf(err, "warning: %s\n", runtime.warning.c_str());
            }
        }
    }

    if (!error.empty()) {
        std::fprintf(err, "error: %s\n", error.c_str());
        return false;
    }

    // A subcommand starts no player: warn that daemon-only flags do nothing.
    if (!out.subcommand.empty()) {
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
        // Warn: a detached daemon with no logfile is silent.
        if (out.was_given(Opt::Daemonize) && !out.was_given(Opt::Logfile)) {
            std::fprintf(err,
                         "warning: -z without -f discards all log output -- a detached daemon's "
                         "stderr is /dev/null. Add -f <path> to keep it.\n");
        }

        // Inert with -s, which never advertises.
        if (out.was_given(Opt::MdnsName) && out.was_given(Opt::Server)) {
            std::fprintf(err,
                         "warning: --mdns-name is unused with -s -- a client that dials out must "
                         "not advertise %s, so there is no instance to name\n",
                         MDNS_CLIENT_SERVICE);
        }
    }

    // Only under -z, where the daemon's chdir("/") would change what relative paths name.
    if (out.was_given(Opt::Daemonize)) {
        if (out.was_given(Opt::Pidfile)) {
            out.pidfile = absolute_path(out.pidfile);
        }
        if (out.was_given(Opt::Logfile)) {
            out.logfile = absolute_path(out.logfile);
        }
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
#ifdef SENDSPIN_CLI_HAVE_MDNS
    std::fprintf(out, "connect to it, or discovers one with -s %s and connects to it.\n\n",
                 DISCOVERY_PREFIX);
#else
    std::fprintf(out, "connect to it.\n\n");
#endif
    std::fprintf(out, "With a subcommand, it instead talks to a player already running on this\n");
    std::fprintf(out,
                 "host over its control socket, and exits. The subcommand must come first.\n\n");
    std::fprintf(out, "Subcommands:\n");
    for (const ControlSubcommand& subcommand : control_subcommands()) {
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
    std::fprintf(out, "  -s, --server %s[<name>]\n", DISCOVERY_PREFIX);
    std::fprintf(out, "                Discover a Sendspin server over mDNS and connect to it,\n");
    std::fprintf(out, "                retrying until it answers: -s %s<name> takes the one\n",
                 DISCOVERY_PREFIX);
    std::fprintf(out, "                advertised under <name>, -s %s takes any. Turns off\n",
                 DISCOVERY_PREFIX);
    std::fprintf(out, "                the mDNS advertisement: the spec forbids advertising\n");
    std::fprintf(out, "                %s while the client initiates the connection\n",
                 MDNS_CLIENT_SERVICE);
#ifndef SENDSPIN_CLI_HAVE_MDNS
    std::fprintf(out, "                (needs mDNS, which this build does not have)\n");
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
    std::fprintf(out, "                together. Accepts a <category>=<level> shape, but the\n");
    std::fprintf(out, "                category is ignored: every line is\n");
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
    std::fprintf(out, "  --audio-format <codec:rate:depth:channels>[,...]\n");
    std::fprintf(out, "                Preferred formats, offered first in the order given,\n");
    std::fprintf(out, "                e.g. flac:48000:24:2,pcm:48000:24:2. Everything else\n");
    std::fprintf(out, "                the player advertises is still offered behind them,\n");
    std::fprintf(out, "                and a server uses the first entry it can encode --\n");
    std::fprintf(out, "                so it may still pick a later one. Preferred, not\n");
    std::fprintf(out, "                exclusive. A listed format the advertised list does\n");
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
