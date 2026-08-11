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
#include "control.h"
#include "log.h"
#include "mdns.h"

#include <getopt.h>
#include <limits.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/// Long-only option values, picked outside the short-option alphabet so `-V`/`-p` stay
/// unclaimed for squeezelite's own meanings.
enum LongOnly {
    OPT_VERSION = 0x100,
    OPT_PORT,
    OPT_BUFFER_MS,
    OPT_NO_MDNS,
    OPT_MDNS_NAME,
    OPT_CONTROL_SOCKET,
    OPT_NO_CONTROL,
};

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
bool parse_log_spec(const char* spec, LogLevel& level, std::FILE* err) {
    const char* eq = std::strchr(spec, '=');
    if (eq == nullptr) {
        return parse_log_level(spec, level);
    }
    if (!parse_log_level(eq + 1, level)) {
        return false;
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
        {"port", required_argument, nullptr, OPT_PORT},
        {"buffer-ms", required_argument, nullptr, OPT_BUFFER_MS},
        {"no-mdns", no_argument, nullptr, OPT_NO_MDNS},
        {"mdns-name", required_argument, nullptr, OPT_MDNS_NAME},
        {"control-socket", required_argument, nullptr, OPT_CONTROL_SOCKET},
        {"no-control", no_argument, nullptr, OPT_NO_CONTROL},
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
    // Rejects an empty value for a flag whose empty case has no meaning. `-n ""` used to
    // fall through to the hostname, which reads as the flag being ignored; `-P ""` and
    // `-f ""` would try to open a file with no name.
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
                if (require_value("-o", optarg)) {
                    out.device = optarg;
                    out.mark_given(Opt::Device);
                }
                break;
            case 'l':
                out.list_devices = true;
                out.mark_given(Opt::ListDevices);
                break;
            case 'n':
                if (require_value("-n", optarg)) {
                    out.name = optarg;
                    out.mark_given(Opt::Name);
                }
                break;
            case 's':
                // Only stored here; resolved once below, after the whole line parses.
                out.server = optarg;
                out.mark_given(Opt::Server);
                break;
            case 'z':
                out.daemonize = true;
                out.mark_given(Opt::Daemonize);
                break;
            case 'P':
                if (require_value("-P", optarg)) {
                    out.pidfile = optarg;
                    out.mark_given(Opt::Pidfile);
                }
                break;
            case 'd':
                if (parse_log_spec(optarg, out.log_level, err)) {
                    out.mark_given(Opt::LogLevel);
                } else {
                    fail("unknown log level '" + std::string(optarg) + "'");
                }
                break;
            case 'f':
                if (require_value("-f", optarg)) {
                    out.logfile = optarg;
                    out.mark_given(Opt::Logfile);
                }
                break;
            case 'h':
                // Nothing after --help can matter, so this is the one early return.
                out.show_help = true;
                return true;
            case OPT_VERSION:
                out.show_version = true;
                return true;
            case OPT_PORT:
                if (parse_port(optarg, out.port)) {
                    out.mark_given(Opt::Port);
                } else {
                    fail("invalid --port '" + std::string(optarg) + "' -- expected 1-65535");
                }
                break;
            case OPT_BUFFER_MS:
                if (parse_buffer_ms(optarg, out.buffer_ms)) {
                    out.mark_given(Opt::BufferMs);
                } else {
                    fail("invalid --buffer-ms '" + std::string(optarg) + "' -- expected " +
                         std::to_string(MIN_BUFFER_MS) + "-" + std::to_string(MAX_BUFFER_MS));
                }
                break;
            case OPT_NO_MDNS:
                out.no_mdns = true;
                out.mark_given(Opt::NoMdns);
                break;
            case OPT_MDNS_NAME:
                if (require_value("--mdns-name", optarg)) {
                    out.mdns_name = optarg;
                    out.mark_given(Opt::MdnsName);
                }
                break;
            case OPT_CONTROL_SOCKET:
                if (require_value("--control-socket", optarg)) {
                    // Only stored here; the length check and the -z rewrite happen below, once
                    // --port is known and the whole line has parsed.
                    out.control_socket = optarg;
                    out.mark_given(Opt::ControlSocket);
                }
                break;
            case OPT_NO_CONTROL:
                out.no_control = true;
                out.mark_given(Opt::NoControl);
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
            fail("-s '" + out.server +
                 "': this build has no mDNS support, so it cannot discover a server. Rebuild "
                 "with dns_sd.h available (libavahi-compat-libdnssd-dev on Debian/Ubuntu, "
                 "avahi-compat-libdns_sd-devel on Fedora), or give -s an address.");
#endif
        } else {
            std::string reason;
            if (!parse_server_url(out.server, out.server_url, reason)) {
                fail(std::move(reason));
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
                fail("--control-socket '" + out.control_socket + "' is " +
                     std::to_string(out.control_socket.size()) +
                     " bytes, and a Unix socket address holds at most " +
                     std::to_string(control_socket_path_limit() - 1) + " on this platform");
            }
        } else {
            const std::string runtime_dir = control_runtime_dir();
            out.control_socket = control_socket_path(runtime_dir, out.port);
            out.control_absent_reason =
                control_socket_absent_reason(runtime_dir, out.control_socket);
            if (!out.control_absent_reason.empty()) {
                // Non-fatal, and deliberately not a fallback to a shared directory: the player
                // is still a player without a control channel. main() warns once and carries on.
                out.control_socket.clear();
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
            Opt::Device,   Opt::Name,     Opt::Server,   Opt::Daemonize, Opt::Pidfile,
            Opt::Logfile,  Opt::LogLevel, Opt::BufferMs, Opt::NoMdns,    Opt::MdnsName,
            Opt::NoControl,
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
    std::fprintf(out, "  -o <device>   Output device (default: %s). Either a reserved name\n",
                 DEFAULT_OUTPUT_DEVICE);
    std::fprintf(out, "                (null, stdout, -), or a <backend>:<device> pair split on\n");
    std::fprintf(out, "                the first colon, where <backend> is one of: %s\n",
                 audio_backend_list().c_str());
#ifdef SENDSPIN_CLI_HAVE_ALSA
    std::fprintf(out, "                Anything else is an ALSA PCM name: -o hw:2,0, -o default\n");
#endif
    std::fprintf(out, "                -l lists this host's devices and what they accept\n");
    std::fprintf(out, "  -l            List output devices with their capabilities, and exit\n");
    std::fprintf(out, "  -n <name>     Friendly name (default: this host's name)\n");
    std::fprintf(out, "  -s <server>   Connect out to <host>[:<port>] or a ws:// URL\n");
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
    std::fprintf(out, "  -P <path>     Hold <path> as a locked pidfile, refusing to start if\n");
    std::fprintf(out, "                another instance already holds it. A file left by a\n");
    std::fprintf(out, "                crash needs no cleanup; keep it on a local filesystem\n");
    std::fprintf(out, "  -d <level>    Log level: none, error, warn, info, debug, verbose\n");
    std::fprintf(out, "                One level for this player and the sendspin library\n");
    std::fprintf(out, "                together. Accepts squeezelite's <category>=<level>\n");
    std::fprintf(out, "                shape, but the category is ignored: every line is\n");
    std::fprintf(out, "                '<L> <tag>: <message>', so filter it with grep\n");
    std::fprintf(out, "  -f <path>     Write log output to <path> instead of stderr, with a\n");
    std::fprintf(out, "                UTC timestamp on every line. SIGHUP reopens the path,\n");
    std::fprintf(out, "                so logrotate and newsyslog can rotate it\n");
    std::fprintf(out, "  --port <port> Port our own server listens on (default: %u)\n",
                 SendspinClientConfig::DEFAULT_SERVER_PORT);
    std::fprintf(out, "  --buffer-ms <ms>\n");
    std::fprintf(out, "                Audio the output backend keeps buffered, %u-%u\n",
                 MIN_BUFFER_MS, MAX_BUFFER_MS);
    std::fprintf(out, "                (default: %u). One figure for every backend; a\n",
                 DEFAULT_BUFFER_MS);
    std::fprintf(out, "                device-less sink ignores it, and a device that needs\n");
    std::fprintf(out, "                more than it asks for gets more\n");
    std::fprintf(out, "  --no-mdns     Do not advertise over mDNS (no effect with -s, which\n");
    std::fprintf(out, "                already suppresses it)\n");
    std::fprintf(out, "  --mdns-name <name>\n");
    std::fprintf(out, "                Instance name to advertise (default: -n). Unused with -s\n");
    std::fprintf(out, "  --control-socket <path>\n");
    std::fprintf(out, "                Unix socket the subcommands above talk to (default:\n");
    std::fprintf(out, "                $XDG_RUNTIME_DIR/%s<port>%s, mode 0600).\n",
                 CONTROL_SOCKET_PREFIX, CONTROL_SOCKET_SUFFIX);
    std::fprintf(out, "                The <port> is --port, so two players on one host each\n");
    std::fprintf(out, "                get their own -- and a subcommand needs the same --port\n");
    std::fprintf(out, "                or this flag. With no $XDG_RUNTIME_DIR (a systemd system\n");
    std::fprintf(out, "                unit has none) there is no default and the player warns\n");
    std::fprintf(out, "                once: there is deliberately no /tmp fallback, which\n");
    std::fprintf(out, "                would let any local user drive this player\n");
    std::fprintf(out, "  --no-control  Do not listen on a control socket at all\n");
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

    // A scheme means the caller is spelling out the whole URL -- path, port and all -- so
    // the only thing to check is that it is one we can actually speak.
    const size_t scheme_end = server.find("://");
    if (scheme_end != std::string::npos) {
        const std::string scheme = server.substr(0, scheme_end);
        if (scheme != "ws" && scheme != "wss") {
            error = "-s '" + server + "': Sendspin runs over WebSocket, so the scheme must be " +
                    "ws:// or wss://, not " + scheme + "://";
            return false;
        }
        // The rest is the caller's to get right -- port, path and all -- but a scheme with
        // nothing after it names no server at all, and would fail far from here.
        if (scheme_end + 3 == server.size()) {
            error = "-s '" + server + "': a scheme but no host";
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
            error = "-s '" + server + "': unterminated '[' -- an IPv6 literal reads [::1]:8927";
            return false;
        }
        host = server.substr(0, bracket + 1);
        const std::string rest = server.substr(bracket + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                error = "-s '" + server + "': expected ':<port>' after ']', got '" + rest + "'";
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
            error = "-s '" + server + "': an IPv6 literal must be bracketed -- try '[" + server +
                    "]' or '[" + server + "]:<port>'";
            return false;
        } else {
            host = server.substr(0, colon);
            port_text = server.substr(colon + 1);
            has_port = true;
        }
    }

    // "[]" is as empty a host as "".
    if (host.empty() || host == "[]") {
        error = "-s '" + server + "': no host before the port";
        return false;
    }

    // Only when no ':' was written at all does the server's own default apply. A written
    // but empty port is a truncated line, not a request for the default.
    uint16_t port = DEFAULT_REMOTE_SERVER_PORT;
    if (has_port && !parse_port(port_text, port)) {
        error = "-s '" + server + "': '" + port_text + "' is not a port number (expected 1-65535)";
        return false;
    }

    url = "ws://" + host + ":" + std::to_string(port) + SENDSPIN_PATH;
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
