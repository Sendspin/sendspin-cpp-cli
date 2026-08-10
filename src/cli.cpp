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
#include "mdns.h"

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
    std::fprintf(err,
                 "warning: -d category '%.*s' ignored -- this build has one global log level\n",
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
        {"buffer-ms", required_argument, nullptr, OPT_BUFFER_MS},
        {"no-mdns", no_argument, nullptr, OPT_NO_MDNS},
        {"mdns-name", required_argument, nullptr, OPT_MDNS_NAME},
        {nullptr, 0, nullptr, 0},
    };

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

    // The leading ':' is what separates "you left the value off" from "no such flag":
    // getopt then returns ':' for a missing argument instead of folding it into '?'.
    int opt = 0;
    while ((opt = getopt_long(argc, argv, ":o:ln:s:zP:d:f:h", long_opts, nullptr)) != -1) {
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
            case ':':
                fail("option '" + offending_option(argv, optind) + "' needs a value");
                break;
            case '?':
            default:
                fail("unknown option '" + offending_option(argv, optind) + "'");
                break;
        }
    }

    if (optind < argc) {
        fail("unexpected argument '" + std::string(argv[optind]) +
             "' -- this player takes flags only");
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

    if (!error.empty()) {
        std::fprintf(err, "error: %s\n", error.c_str());
        return false;
    }

    // Inert rather than contradictory, so it warns instead of failing: -s picks the outbound
    // mode, which the spec forbids advertising alongside, so there is no instance to name.
    if (out.was_given(Opt::MdnsName) && out.was_given(Opt::Server)) {
        std::fprintf(err,
                     "warning: --mdns-name is unused with -s -- a client that dials out must "
                     "not advertise %s, so there is no instance to name\n",
                     MDNS_CLIENT_SERVICE);
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
    std::fprintf(out, "Usage: %s [options]\n\n", prog);
    std::fprintf(out, "A headless Sendspin audio player. Listens for a Sendspin server to\n");
    std::fprintf(out, "connect to it, or dials one with -s.\n\n");
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
    std::fprintf(out, "  -z            Daemonize (not implemented yet)\n");
    std::fprintf(out, "  -P <path>     Write the process id to <path>\n");
    std::fprintf(out, "  -d <level>    Log level: none, error, warn, info, debug, verbose\n");
    std::fprintf(out, "                Accepts squeezelite's <category>=<level> shape too\n");
    std::fprintf(out, "  -f <path>     Write log output to <path> instead of stderr\n");
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
