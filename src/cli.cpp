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

#include <getopt.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sendspin_cli {

using sendspin::LogLevel;
using sendspin::SendspinClientConfig;

namespace {

constexpr const char* SENDSPIN_PATH = "/sendspin";
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

/// Rejects an empty value for a flag whose empty case has no meaning.
///
/// `-n ""` used to fall through to the hostname, which looks like the flag was ignored;
/// `-P ""` and `-f ""` would try to open a file with no name. Saying so beats guessing.
bool require_value(std::FILE* err, const char* flag, const char* value) {
    if (value[0] != '\0') {
        return true;
    }
    std::fprintf(err, "error: %s needs a non-empty value\n", flag);
    return false;
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

/// Accepts squeezelite's `-d <category>=<level>` shape. The library exposes one global
/// level, so a category is parsed and ignored; per-category logging is a follow-up task.
bool parse_log_spec(const char* spec, LogLevel& level, std::FILE* err) {
    const char* eq = std::strchr(spec, '=');
    if (eq == nullptr) {
        return parse_log_level(spec, level);
    }
    if (!parse_log_level(eq + 1, level)) {
        return false;
    }
    std::fprintf(err, "warning: -d category '%.*s' ignored -- this build has one global log level\n",
                 static_cast<int>(eq - spec), spec);
    return true;
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
        {nullptr, 0, nullptr, 0},
    };

    reset_getopt();

    // The leading ':' is what separates "you left the value off" from "no such flag":
    // getopt then returns ':' for a missing argument instead of folding it into '?'.
    int opt = 0;
    while ((opt = getopt_long(argc, argv, ":o:ln:s:zP:d:f:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'o':
                if (!require_value(err, "-o", optarg)) {
                    return false;
                }
                out.device = optarg;
                out.mark_given(Opt::Device);
                break;
            case 'l':
                out.list_devices = true;
                out.mark_given(Opt::ListDevices);
                break;
            case 'n':
                if (!require_value(err, "-n", optarg)) {
                    return false;
                }
                out.name = optarg;
                out.mark_given(Opt::Name);
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
                if (!require_value(err, "-P", optarg)) {
                    return false;
                }
                out.pidfile = optarg;
                out.mark_given(Opt::Pidfile);
                break;
            case 'd':
                if (!parse_log_spec(optarg, out.log_level, err)) {
                    std::fprintf(err, "error: unknown log level '%s'\n", optarg);
                    return false;
                }
                out.mark_given(Opt::LogLevel);
                break;
            case 'f':
                if (!require_value(err, "-f", optarg)) {
                    return false;
                }
                out.logfile = optarg;
                out.mark_given(Opt::Logfile);
                break;
            case 'h':
                // Short-circuits: --help must work even alongside flags that would fail,
                // since asking for the flag list is what someone does when they got one wrong.
                out.show_help = true;
                return true;
            case OPT_VERSION:
                out.show_version = true;
                return true;
            case OPT_PORT:
                if (!parse_port(optarg, out.port)) {
                    std::fprintf(err, "error: invalid --port '%s' -- expected 1-65535\n", optarg);
                    return false;
                }
                out.mark_given(Opt::Port);
                break;
            case ':':
                std::fprintf(err, "error: option '%s' needs a value\n", argv[optind - 1]);
                return false;
            case '?':
            default:
                if (optopt != 0) {
                    std::fprintf(err, "error: unknown option '-%c'\n", optopt);
                } else {
                    std::fprintf(err, "error: unknown option '%s'\n", argv[optind - 1]);
                }
                return false;
        }
    }

    if (optind < argc) {
        std::fprintf(err, "error: unexpected argument '%s' -- this player takes flags only\n",
                     argv[optind]);
        return false;
    }

    if (out.was_given(Opt::Server)) {
        std::string error;
        if (!parse_server_url(out.server, out.server_url, error)) {
            std::fprintf(err, "error: %s\n", error.c_str());
            return false;
        }
    }

    if (out.name.empty()) {
        out.name = default_client_name();
    }
    return true;
}

void print_usage(std::FILE* out, const char* prog) {
    std::fprintf(out, "Usage: %s [options]\n\n", prog);
    std::fprintf(out, "A headless Sendspin audio player. Listens for a Sendspin server to\n");
    std::fprintf(out, "connect to it, or dials one with -s.\n\n");
    std::fprintf(out, "Options:\n");
    std::fprintf(out, "  -o <device>   Output device (default: %s). Either a reserved name\n",
                 DEFAULT_OUTPUT_DEVICE);
    std::fprintf(out, "                (null, stdout, -), a <backend>:<device> pair split on\n");
    std::fprintf(out, "                the first colon (-o alsa:hw:2,0), or an ALSA PCM name\n");
    std::fprintf(out, "                on its own (-o hw:2,0). -l lists them\n");
    std::fprintf(out, "  -l            List output devices with their capabilities, and exit\n");
    std::fprintf(out, "  -n <name>     Friendly name (default: this host's name)\n");
    std::fprintf(out, "  -s <server>   Connect out to <host>[:<port>] or a ws:// URL\n");
    std::fprintf(out, "                (the server's port defaults to %u)\n",
                 DEFAULT_REMOTE_SERVER_PORT);
    std::fprintf(out, "  -z            Daemonize (not implemented yet)\n");
    std::fprintf(out, "  -P <path>     Write the process id to <path>\n");
    std::fprintf(out, "  -d <level>    Log level: none, error, warn, info, debug, verbose\n");
    std::fprintf(out, "                Accepts squeezelite's <category>=<level> shape too\n");
    std::fprintf(out, "  -f <path>     Write log output to <path> instead of stderr\n");
    std::fprintf(out, "  --port <port> Port our own server listens on (default: %u)\n",
                 SendspinClientConfig::DEFAULT_SERVER_PORT);
    std::fprintf(out, "  -h, --help    Show this help\n");
    std::fprintf(out, "  --version     Show version information\n\n");
    std::fprintf(out, "This is an early scaffold; see docs/ROADMAP.md for what is still missing.\n");
}

void print_version(std::FILE* out) {
    std::fprintf(out, "sendspin-cli %s\n", SENDSPIN_CLI_VERSION);
    std::fprintf(out, "sendspin-cpp %s\n", SENDSPIN_CLI_LIB_TAG);
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
