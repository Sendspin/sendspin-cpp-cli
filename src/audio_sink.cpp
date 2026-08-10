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

#include "audio_sink.h"

#include "null_sink.h"

#ifdef SENDSPIN_CLI_HAVE_ALSA
#include "alsa_sink.h"
#endif

#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
#include "portaudio_sink.h"
#endif

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace sendspin_cli {

namespace {

/// Whether `-o <name>` wants a device after it.
enum class DeviceArg {
    None,      ///< `<name>` alone; `<name>:<device>` is an error
    Required,  ///< `<name>:<device>` only; a bare `<name>` is an error
    Optional,  ///< either, where a bare `<name>` means the backend's own default device
};

/// A backend prefix this build can resolve.
struct BuiltBackend {
    const char* name;
    SinkBackend backend;
    DeviceArg device_arg;
};

/// The `null` and `stdout` sinks exist on every build, so those two names mean the same
/// thing everywhere. ALSA ships a PCM called "null" too; ours wins.
///
/// PortAudio's device is optional because it has a meaningful default of its own -- the
/// host's default output -- which is what makes a bare `-o portaudio` play. ALSA's is
/// required because its equivalent is a PCM *named* `default`, reachable by rule 3.
constexpr BuiltBackend BUILT_BACKENDS[] = {
    {"null", SinkBackend::Null, DeviceArg::None},
    {"stdout", SinkBackend::Stdout, DeviceArg::None},
#ifdef SENDSPIN_CLI_HAVE_ALSA
    {"alsa", SinkBackend::Alsa, DeviceArg::Required},
#endif
#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
    {"portaudio", SinkBackend::PortAudio, DeviceArg::Optional},
#endif
};

/// A backend prefix -o still recognizes even though this build cannot serve it.
struct ReservedBackend {
    const char* name;
    const char* reason;
};

/// Every backend prefix this project knows, and why a build might not have it.
///
/// Consulted only after BUILT_BACKENDS misses, so an entry for a backend this build *does*
/// have is unreachable -- which is why the table needs no #ifdefs of its own. Without it a
/// spec would fall through to rule 3 and be handed to ALSA as a PCM name, so `-o portaudio:2`
/// on a build without PortAudio would fail with "Unknown PCM portaudio:2" rather than saying
/// what this build has and which flag turns the backend on.
constexpr ReservedBackend RESERVED_BACKENDS[] = {
    {"alsa",
     "the ALSA backend is not in this build -- libasound was missing, or it was configured "
     "with -DSENDSPIN_CLI_WITH_ALSA=OFF"},
    {"portaudio",
     "the PortAudio backend is not in this build -- libportaudio was missing, or it was "
     "configured with -DSENDSPIN_CLI_WITH_PORTAUDIO=OFF"},
};

/// Reports a prefix this build recognizes but cannot serve. Always names what it *can*
/// serve, so the message is actionable on its own.
std::string unavailable_error(const ReservedBackend& reserved) {
    return std::string(reserved.reason) + ". This build has: " + audio_backend_list();
}

/// Joins `values` with spaces, or yields `empty` when there are none.
template <typename T>
std::string join_or(const std::vector<T>& values, const char* empty) {
    if (values.empty()) {
        return empty;
    }
    std::string list;
    for (const T& value : values) {
        if (!list.empty()) {
            list += ' ';
        }
        list += std::to_string(static_cast<unsigned>(value));
    }
    return list;
}

}  // namespace

SinkCapabilities SinkCapabilities::permissive() {
    return {{PROBE_RATES.begin(), PROBE_RATES.end()},
            {PROBE_BIT_DEPTHS.begin(), PROBE_BIT_DEPTHS.end()},
            {PROBE_CHANNELS.begin(), PROBE_CHANNELS.end()}};
}

std::string audio_backend_list() {
    std::string list;
    for (const BuiltBackend& entry : BUILT_BACKENDS) {
        if (!list.empty()) {
            list += ", ";
        }
        list += entry.name;
    }
    return list;
}

void print_sink_capabilities(std::FILE* out, const SinkCapabilities& caps,
                             const std::array<const char*, PROBE_BIT_DEPTHS.size()>& depth_names) {
    std::string formats;
    for (size_t i = 0; i < PROBE_BIT_DEPTHS.size(); ++i) {
        if (std::find(caps.bit_depths.begin(), caps.bit_depths.end(), PROBE_BIT_DEPTHS[i]) !=
            caps.bit_depths.end()) {
            if (!formats.empty()) {
                formats += ' ';
            }
            formats += depth_names[i];
        }
    }

    // An empty list is meaningful: the device is there but takes nothing this player emits.
    std::fprintf(out, "      rates:    %s\n",
                 join_or(caps.rates, "(none of the probed rates)").c_str());
    std::fprintf(out, "      formats:  %s\n",
                 formats.empty() ? "(none sendspin-cli can emit)" : formats.c_str());
    std::fprintf(out, "      channels: %s\n",
                 join_or(caps.channels, "(none of the probed counts)").c_str());
}

bool resolve_device_spec(const std::string& spec, DeviceSpec& out, std::string& error) {
    if (spec.empty()) {
        error = "empty output device -- run with -l to list what this build has";
        return false;
    }

    // 1. A whole-string backend name: the device-less sinks, or one whose device is optional.
    if (spec == "-") {
        out = {SinkBackend::Stdout, ""};
        return true;
    }
    for (const BuiltBackend& entry : BUILT_BACKENDS) {
        if (spec != entry.name) {
            continue;
        }
        if (entry.device_arg == DeviceArg::Required) {
            // A bare backend name that needs one. Saying so beats handing "alsa" to ALSA as a
            // PCM name and reporting that no such PCM exists.
            error = "-o '" + spec + "' names a backend but no device -- write -o " + spec +
                    ":<device>, or -l to list them";
            return false;
        }
        // An empty device is what None and Optional both resolve to; the backend decides what
        // it means, which for PortAudio is this host's default output.
        out = {entry.backend, ""};
        return true;
    }

    // 2. <backend>:<device>. First colon only: ALSA device names carry their own, so
    //    alsa:hw:2,0 is the ALSA backend playing hw:2,0.
    const size_t colon = spec.find(':');
    const std::string prefix = spec.substr(0, colon);  // whole string when there is no colon
    if (colon != std::string::npos) {
        const std::string rest = spec.substr(colon + 1);
        for (const BuiltBackend& entry : BUILT_BACKENDS) {
            if (prefix != entry.name) {
                continue;
            }
            if (entry.device_arg == DeviceArg::None) {
                error = "the " + prefix + " backend takes no device, so -o '" + spec +
                        "' means nothing -- use -o " + prefix + " on its own";
                return false;
            }
            if (rest.empty()) {
                // A written-but-empty device is a truncated command line, not a request for
                // the default -- the same call parse_server_url() makes about a bare `host:`.
                error = "-o '" + spec + "' names no device -- write -o " + prefix + ":<device>";
                if (entry.device_arg == DeviceArg::Optional) {
                    error += ", or -o " + prefix + " on its own for this host's default";
                }
                error += ", or -l to list them";
                return false;
            }
            out = {entry.backend, rest};
            return true;
        }
    }
    for (const ReservedBackend& reserved : RESERVED_BACKENDS) {
        if (prefix == reserved.name) {
            error = unavailable_error(reserved);
            return false;
        }
    }

    // 3. Anything else is an ALSA PCM name, which is how squeezelite's -o behaves: there
    //    is no fixed device list to keep in sync with the host's hardware. PortAudio is
    //    deliberately not reachable this way -- it *does* enumerate its devices, so the
    //    justification does not carry over, and a bare name resolving per host is exactly
    //    what rule 1 keeps `null` safe from.
#ifdef SENDSPIN_CLI_HAVE_ALSA
    out = {SinkBackend::Alsa, spec};
    return true;
#else
    error = "unknown output device '" + spec + "' -- this build has: " + audio_backend_list();
#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
    // On a PortAudio-only build the commonest way to land here is a device name typed without
    // its prefix, so name the prefix rather than only the backend list.
    error += ". A PortAudio device needs its prefix: -o portaudio:" + spec;
#endif
    error += " (run with -l)";
    return false;
#endif
}

// buffer_ms is read only by the device-backed branches below, so a build with neither
// backend compiled in genuinely has no use for it.
std::unique_ptr<AudioSink> make_audio_sink(const std::string& device,
                                           [[maybe_unused]] uint32_t buffer_ms,
                                           std::string& error) {
    DeviceSpec spec;
    if (!resolve_device_spec(device, spec, error)) {
        return nullptr;
    }

    switch (spec.backend) {
        case SinkBackend::Null:
            // buffer_ms goes nowhere here on purpose: a sink with no device consumes every
            // write immediately, so there is nothing for it to size.
            return std::make_unique<NullAudioSink>(NullSinkOutput::Discard);
        case SinkBackend::Stdout:
            return std::make_unique<NullAudioSink>(NullSinkOutput::Stdout);
        case SinkBackend::Alsa:
#ifdef SENDSPIN_CLI_HAVE_ALSA
            // Probed now rather than at the first stream, so a typo fails while someone is
            // still watching the terminal instead of minutes later when a track starts.
            if (!AlsaAudioSink::probe(spec.device, error)) {
                return nullptr;
            }
            return std::make_unique<AlsaAudioSink>(spec.device, buffer_ms);
#else
            break;  // unreachable: resolve_device_spec() never yields Alsa without the backend
#endif
        case SinkBackend::PortAudio:
#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
            if (!PortAudioSink::probe(spec.device, error)) {
                return nullptr;
            }
            return std::make_unique<PortAudioSink>(spec.device, buffer_ms);
#else
            break;  // unreachable, for the same reason as Alsa above
#endif
    }

    error = "internal error: output device '" + device + "' resolved to a backend this build "
            "cannot construct";
    return nullptr;
}

void print_audio_devices(std::FILE* out) {
    std::fprintf(out, "Output devices (-o):\n");
    std::fprintf(out, "  null      discard audio; needs no sound card at all\n");
    std::fprintf(out, "  stdout    raw interleaved PCM on stdout, e.g. | aplay -f cd\n");
    std::fprintf(out, "  -         alias for stdout\n");
#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
    std::fprintf(out, "  portaudio this host's default output device, whatever it currently is\n");
#endif

    std::fprintf(out,
                 "\nHow -o reads its argument, in this order:\n"
                 "  1. one of the reserved names above;\n"
                 "  2. <backend>:<device>, split on the FIRST colon, where <backend> is one\n"
                 "     of: %s;\n",
                 audio_backend_list().c_str());
#ifdef SENDSPIN_CLI_HAVE_ALSA
    std::fprintf(out,
                 "     ALSA device names carry their own colons, which is why the split is on\n"
                 "     the first one -- -o alsa:hw:2,0 is the alsa backend playing hw:2,0;\n"
                 "  3. anything else is an ALSA PCM name, so -o hw:2,0 and -o default keep\n"
                 "     working with no prefix at all.\n");
#else
    std::fprintf(out,
                 "  3. anything else would be an ALSA PCM name, but this build has no ALSA\n"
                 "     backend, so only the forms above resolve here.\n");
#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
    std::fprintf(out,
                 "     A PortAudio device is reached through its prefix, never bare -- see the\n"
                 "     device list below.\n");
#endif
#endif

#ifdef SENDSPIN_CLI_HAVE_ALSA
    std::fprintf(out, "\nALSA PCMs on this host (any of these names can follow -o):\n");
    AlsaAudioSink::list_devices(out);
    std::fprintf(out,
                 "\nHardware PCMs also accept the short hw:<card>,<device> and\n"
                 "plughw:<card>,<device> forms -- plughw converts rates and formats the\n"
                 "device itself will not take.\n"
                 "\nThe rates, formats and channel counts above are what each PCM accepts\n"
                 "directly. A plug-style PCM -- default, plughw:, and most named PCMs from a\n"
                 "sound server -- reports nearly everything because the plug layer converts,\n"
                 "so its list says little about the hardware behind it. Only the formats\n"
                 "sendspin-cli can emit are shown: S8, S16_LE, S24_3LE, S32_LE.\n");
#else
    std::fprintf(out,
                 "\nThis build has no ALSA backend (libasound was missing, or it was configured\n"
                 "with -DSENDSPIN_CLI_WITH_ALSA=OFF), so it plays through: %s.\n",
                 audio_backend_list().c_str());
#endif

#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
    std::fprintf(out, "\nPortAudio output devices on this host (-o portaudio:<index|name>):\n");
    PortAudioSink::list_devices(out);
    std::fprintf(out,
                 "\nThe name is the form worth writing down. PortAudio numbers devices as it\n"
                 "walks each host API, so an index shifts as devices come and go; a name is\n"
                 "matched in full and case-insensitively, and one that matches more than one\n"
                 "device is refused rather than guessed at. -o portaudio with no device at all\n"
                 "follows this host's default output, resolved afresh at every stream.\n"
                 "\nInput-only devices are left out, since -o cannot play through them. The\n"
                 "rate on the device's own line is its default; the rates below it are what it\n"
                 "will actually take, asked for the same way a stream would ask. Only the four\n"
                 "formats sendspin-cli can emit are shown, and the host API converts on top of\n"
                 "them where it must.\n");
#endif
}

}  // namespace sendspin_cli
