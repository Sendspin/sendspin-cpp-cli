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

/// Long-only option values, picked outside the short-option alphabet so `-V`/`-p` stay
/// unclaimed for squeezelite's own meanings.
enum LongOnly {
    OPT_VERSION = 0x100,
    OPT_PORT,
};

bool parse_port(const char* str, uint16_t& port) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(str, &end, 10);
    if (*str == '\0' || end == nullptr || *end != '\0' || value == 0 || value > 65535UL) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
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
bool parse_log_spec(const char* spec, LogLevel& level) {
    const char* eq = std::strchr(spec, '=');
    if (eq == nullptr) {
        return parse_log_level(spec, level);
    }
    if (!parse_log_level(eq + 1, level)) {
        return false;
    }
    std::fprintf(stderr,
                 "warning: -d category '%.*s' ignored -- this build has one global log level\n",
                 static_cast<int>(eq - spec), spec);
    return true;
}

bool is_all_digits(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    return value.find_first_not_of("0123456789") == std::string::npos;
}

}  // namespace

bool parse_options(int argc, char* argv[], Options& out) {
    static const struct option long_opts[] = {
        {"help", no_argument, nullptr, 'h'},
        {"version", no_argument, nullptr, OPT_VERSION},
        {"port", required_argument, nullptr, OPT_PORT},
        {nullptr, 0, nullptr, 0},
    };

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "o:ln:s:zP:d:f:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'o':
                out.device = optarg;
                break;
            case 'l':
                out.list_devices = true;
                break;
            case 'n':
                out.name = optarg;
                break;
            case 's':
                out.server = optarg;
                break;
            case 'z':
                out.daemonize = true;
                break;
            case 'P':
                out.pidfile = optarg;
                break;
            case 'd':
                if (!parse_log_spec(optarg, out.log_level)) {
                    std::fprintf(stderr, "error: unknown log level '%s'\n", optarg);
                    return false;
                }
                break;
            case 'f':
                out.logfile = optarg;
                break;
            case 'h':
                out.show_help = true;
                return true;
            case OPT_VERSION:
                out.show_version = true;
                return true;
            case OPT_PORT:
                if (!parse_port(optarg, out.port)) {
                    std::fprintf(stderr, "error: invalid port '%s'\n", optarg);
                    return false;
                }
                break;
            default:
                // getopt_long has already described the problem.
                return false;
        }
    }

    if (optind < argc) {
        std::fprintf(stderr, "error: unexpected argument '%s'\n", argv[optind]);
        return false;
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
    std::fprintf(out, "  -o <device>   Output device (default: null). -l lists them\n");
    std::fprintf(out, "  -l            List output devices and exit\n");
    std::fprintf(out, "  -n <name>     Friendly name (default: this host's name)\n");
    std::fprintf(out, "  -s <server>   Connect out to <host>[:<port>] or a ws:// URL\n");
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

std::string server_url(const std::string& server) {
    if (server.rfind("ws://", 0) == 0 || server.rfind("wss://", 0) == 0) {
        return server;
    }

    std::string host = server;
    std::string port = std::to_string(SendspinClientConfig::DEFAULT_SERVER_PORT);

    // Split off a trailing :port, but skip past a bracketed IPv6 literal's own colons.
    size_t search_from = 0;
    if (!host.empty() && host.front() == '[') {
        const size_t bracket = host.find(']');
        search_from = (bracket == std::string::npos) ? 0 : bracket;
    }
    const size_t colon = host.find(':', search_from);
    if (colon != std::string::npos) {
        const std::string given = host.substr(colon + 1);
        if (is_all_digits(given)) {
            port = given;
        } else {
            std::fprintf(stderr, "warning: '%s' is not a port number, using %s\n", given.c_str(),
                         port.c_str());
        }
        host = host.substr(0, colon);
    }

    return "ws://" + host + ":" + port + SENDSPIN_PATH;
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
