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

#include "log.h"
#include "null_sink.h"

#ifdef SENDSPIN_CLI_HAVE_ALSA
#include "alsa_sink.h"
#endif

#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
#include "portaudio_sink.h"
#endif

#ifdef SENDSPIN_CLI_HAVE_PULSE
#include "pulse_sink.h"
#endif

#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE
#include "pipewire_sink.h"
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
/// required because its equivalent is a PCM *named* `default`, reachable by rule 3. The two
/// sound-server backends are optional for PortAudio's reason: each server has a default of its
/// own, so a bare `-o pulse` plays wherever a bare `-o portaudio` would.
///
/// `pulse` and `pipewire` also shadow ALSA plugin PCMs that are live on the hosts they target --
/// see resolve_device_spec(), which is where that trade and its escape hatch are set out, and
/// alsa_pcm_is_reachable(), which is what keeps `-l` honest about it.
constexpr BuiltBackend BUILT_BACKENDS[] = {
    {"null", SinkBackend::Null, DeviceArg::None},
    {"stdout", SinkBackend::Stdout, DeviceArg::None},
#ifdef SENDSPIN_CLI_HAVE_ALSA
    {"alsa", SinkBackend::Alsa, DeviceArg::Required},
#endif
#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
    {"portaudio", SinkBackend::PortAudio, DeviceArg::Optional},
#endif
#ifdef SENDSPIN_CLI_HAVE_PULSE
    {"pulse", SinkBackend::Pulse, DeviceArg::Optional},
#endif
#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE
    {"pipewire", SinkBackend::PipeWire, DeviceArg::Optional},
#endif
};

/// A backend prefix -o still recognizes even though this build cannot serve it.
struct ReservedBackend {
    const char* name;
    const char* reason;
    /// The ALSA plugin PCM that reaches the same server, or nullptr where there is none.
    ///
    /// Only mentioned on a build that has the ALSA backend, which is why it is a field here rather
    /// than part of `reason`: without ALSA the advice would name a route this build does not have
    /// either. It matters because a message naming only the CMake flag would send someone off to
    /// rebuild for a path that already works on their host.
    const char* alsa_plugin;
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
     "with -DSENDSPIN_CLI_WITH_ALSA=OFF",
     nullptr},
    {"portaudio",
     "the PortAudio backend is not in this build -- libportaudio was missing, or it was "
     "configured with -DSENDSPIN_CLI_WITH_PORTAUDIO=OFF",
     nullptr},
    {"pulse",
     "the PulseAudio backend is not in this build -- libpulse was missing, or it was "
     "configured with -DSENDSPIN_CLI_WITH_PULSE=OFF",
     "pulse"},
    {"pipewire",
     "the PipeWire backend is not in this build -- libpipewire was missing, or it was "
     "configured with -DSENDSPIN_CLI_WITH_PIPEWIRE=OFF",
     "pipewire"},
};

/// Reports a prefix this build recognizes but cannot serve. Always names what it *can*
/// serve, so the message is actionable on its own -- and, where one exists, the ALSA plugin PCM
/// that reaches the same server without any rebuild at all.
std::string unavailable_error(const ReservedBackend& reserved) {
    std::string error = std::string(reserved.reason) + ". This build has: " + audio_backend_list();
#ifdef SENDSPIN_CLI_HAVE_ALSA
    if (reserved.alsa_plugin != nullptr) {
        error += ". That server is still reachable through ALSA's plugin PCM as -o alsa:";
        error += reserved.alsa_plugin;
    }
#endif
    return error;
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

bool alsa_pcm_is_reachable(const std::string& pcm) {
#ifdef SENDSPIN_CLI_HAVE_ALSA
    DeviceSpec spec;
    std::string error;
    if (!resolve_device_spec(pcm, spec, error)) {
        // -o <pcm> is refused outright, which `alsa` itself is: a backend name that needs a device.
        return false;
    }
    // Rule 3 is what makes a bare name an ALSA PCM, and it hands the name through unchanged.
    // Anything else means a backend prefix claimed the name first.
    return spec.backend == SinkBackend::Alsa && spec.device == pcm;
#else
    static_cast<void>(pcm);
    return false;
#endif
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
        if (prefix != reserved.name) {
            continue;
        }
#ifdef SENDSPIN_CLI_HAVE_ALSA
        if (colon == std::string::npos && reserved.alsa_plugin != nullptr) {
            // A bare `pulse` or `pipewire` on a build without that native backend. The name meant
            // ALSA's plugin PCM before the backend existed, rule 3 below still serves it, and this
            // build can play it -- so claiming the name here would break a working command line in
            // order to report a backend the user never asked for. With a device after it there is
            // no such reading, and the message below is the useful answer.
            break;
        }
#endif
        error = unavailable_error(reserved);
        return false;
    }

    // 3. Anything else is an ALSA PCM name, which is how a conventional `-o` behaves: there
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

namespace {

/// Says so, once at startup, when a bare `-o pulse` or `-o pipewire` has changed meaning.
///
/// The shadowing is documented in the README, in `-l` and in the message a build without the
/// backend gives -- and none of those reaches the person whose config file says `output = pulse`
/// and who upgraded without reading anything. This does: it is the one place that person is
/// certain to look, because it is in the log beside the line that says which device opened.
///
/// Only where the ALSA backend is also built, because only there was the name something else
/// first. Only for a *bare* name, because `-o pulse:<sink>` never meant the plugin PCM. And at
/// INFO rather than WARN: nothing is wrong, the player is doing the better thing.
void warn_if_shadowing_an_alsa_pcm([[maybe_unused]] const std::string& spec,
                                   [[maybe_unused]] const DeviceSpec& resolved) {
#ifdef SENDSPIN_CLI_HAVE_ALSA
    if (resolved.backend != SinkBackend::Pulse && resolved.backend != SinkBackend::PipeWire) {
        return;
    }
    if (spec.find(':') != std::string::npos) {
        return;  // -o pulse:<sink> asked for this backend by name; nothing changed under it
    }
    log_line(sendspin::LogLevel::INFO, LOG_TAG_AUDIO,
             "-o %s is the native %s backend, not ALSA's plugin PCM of the same name -- write "
             "-o alsa:%s for that",
             spec.c_str(), spec.c_str(), spec.c_str());
#endif
}

}  // namespace

// buffer_ms is read only by the device-backed branches below, so a build with neither
// backend compiled in genuinely has no use for it.
std::unique_ptr<AudioSink> make_audio_sink(const std::string& device,
                                           [[maybe_unused]] uint32_t buffer_ms,
                                           std::string& error) {
    DeviceSpec spec;
    if (!resolve_device_spec(device, spec, error)) {
        return nullptr;
    }
    warn_if_shadowing_an_alsa_pcm(device, spec);

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
        case SinkBackend::Pulse:
#ifdef SENDSPIN_CLI_HAVE_PULSE
            if (!PulseAudioSink::probe(spec.device, error)) {
                return nullptr;
            }
            return std::make_unique<PulseAudioSink>(spec.device, buffer_ms);
#else
            break;  // unreachable, for the same reason as Alsa above
#endif
        case SinkBackend::PipeWire:
#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE
            if (!PipeWireSink::probe(spec.device, error)) {
                return nullptr;
            }
            return std::make_unique<PipeWireSink>(spec.device, buffer_ms);
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
#ifdef SENDSPIN_CLI_HAVE_PULSE
    std::fprintf(out, "  pulse     the PulseAudio server's own default sink\n");
#endif
#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE
    std::fprintf(out, "  pipewire  wherever the PipeWire graph routes a playback stream\n");
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

    // Said here rather than only in the README, because this is where someone looks after typing
    // -o pulse and finding it no longer means the ALSA plugin PCM they were used to. Spelled out
    // per case rather than assembled from fragments: there are only three, and a sentence a
    // reader has to reassemble in their head is worse than three that each read straight through.
#if defined(SENDSPIN_CLI_HAVE_ALSA) && defined(SENDSPIN_CLI_HAVE_PULSE) && \
    defined(SENDSPIN_CLI_HAVE_PIPEWIRE)
    std::fprintf(out,
                 "\nALSA ships plugin PCMs called pulse and pipewire too, and on this build\n"
                 "rule 1 wins: -o pulse and -o pipewire reach the native backends instead. The\n"
                 "plugin PCMs are still there -- write -o alsa:pulse or -o alsa:pipewire -- and\n"
                 "they are left out of the list below, because -o cannot reach them bare.\n");
#elif defined(SENDSPIN_CLI_HAVE_ALSA) && defined(SENDSPIN_CLI_HAVE_PULSE)
    std::fprintf(out,
                 "\nALSA ships a plugin PCM called pulse too, and on this build rule 1 wins:\n"
                 "-o pulse reaches the native backend instead. The plugin PCM is still there --\n"
                 "write -o alsa:pulse -- and it is left out of the list below, because -o cannot\n"
                 "reach it bare.\n");
#elif defined(SENDSPIN_CLI_HAVE_ALSA) && defined(SENDSPIN_CLI_HAVE_PIPEWIRE)
    std::fprintf(out,
                 "\nALSA ships a plugin PCM called pipewire too, and on this build rule 1 wins:\n"
                 "-o pipewire reaches the native backend instead. The plugin PCM is still\n"
                 "there -- write -o alsa:pipewire -- and it is left out of the list below,\n"
                 "because -o cannot reach it bare.\n");
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

#ifdef SENDSPIN_CLI_HAVE_PULSE
    std::fprintf(out, "\nPulseAudio sinks on this host (-o pulse:<sink>):\n");
    PulseAudioSink::list_devices(out);
    std::fprintf(out,
                 "\nThe name on each sink's own line is what follows -o pulse:; the line under it\n"
                 "is the description the server shows in a mixer, and the one after that is what\n"
                 "the sink is currently running at. -o pulse with no sink at all follows\n"
                 "whichever sink the server calls default, resolved by the server at every\n"
                 "stream.\n");
#endif

#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE
    std::fprintf(out, "\nPipeWire audio sink nodes on this host (-o pipewire:<node>):\n");
    PipeWireSink::list_devices(out);
    std::fprintf(out,
                 "\nThe name on each node's own line is what follows -o pipewire:. Only\n"
                 "Audio/Sink nodes are listed, since -o cannot play through the others. No\n"
                 "node is marked as the default: where a playback stream lands with no target\n"
                 "named is the graph's own routing decision, taken per stream and changeable\n"
                 "while one is running.\n");
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
