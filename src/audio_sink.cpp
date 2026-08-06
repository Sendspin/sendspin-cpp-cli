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

#include <memory>
#include <string>

namespace sendspin_cli {

namespace {

/// A backend prefix this build can resolve.
struct BuiltBackend {
    const char* name;
    SinkBackend backend;
    bool takes_device;  ///< false for the device-less sinks, which reject `<name>:<device>`
};

/// The `null` and `stdout` sinks exist on every build, so those two names mean the same
/// thing everywhere. ALSA ships a PCM called "null" too; ours wins.
constexpr BuiltBackend BUILT_BACKENDS[] = {
    {"null", SinkBackend::Null, false},
    {"stdout", SinkBackend::Stdout, false},
#ifdef SENDSPIN_CLI_HAVE_ALSA
    {"alsa", SinkBackend::Alsa, true},
#endif
};

/// A backend prefix -o still recognizes even though this build cannot serve it.
struct ReservedBackend {
    const char* name;
    const char* reason;
};

/// Backend names reserved on purpose, so `-o <name>:<device>` gets a straight answer.
///
/// Without an entry here the spec would fall through to rule 3 and be handed to ALSA as a
/// PCM name -- so `-o portaudio:2` would fail with "Unknown PCM portaudio:2" rather than
/// saying which backends exist. Reserving the name is also the hook the PortAudio task
/// (docs/ROADMAP.md item 3) removes when it fills the backend in.
constexpr ReservedBackend RESERVED_BACKENDS[] = {
#ifndef SENDSPIN_CLI_HAVE_ALSA
    {"alsa",
     "the ALSA backend is not in this build -- libasound was missing, or it was configured "
     "with -DSENDSPIN_CLI_WITH_ALSA=OFF"},
#endif
    {"portaudio", "the PortAudio backend is not implemented yet -- see docs/ROADMAP.md item 3"},
};

/// Reports a prefix this build recognizes but cannot serve. Always names what it *can*
/// serve, so the message is actionable on its own.
std::string unavailable_error(const ReservedBackend& reserved) {
    return std::string(reserved.reason) + ". This build has: " + audio_backend_list();
}

}  // namespace

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

bool resolve_device_spec(const std::string& spec, DeviceSpec& out, std::string& error) {
    if (spec.empty()) {
        error = "empty output device -- run with -l to list what this build has";
        return false;
    }

    // 1. A reserved whole-string name for one of the device-less sinks.
    if (spec == "-") {
        out = {SinkBackend::Stdout, ""};
        return true;
    }
    for (const BuiltBackend& entry : BUILT_BACKENDS) {
        if (spec != entry.name) {
            continue;
        }
        if (!entry.takes_device) {
            out = {entry.backend, ""};
            return true;
        }
        // A bare backend name that needs one. Saying so beats handing "alsa" to ALSA as a
        // PCM name and reporting that no such PCM exists.
        error = "-o '" + spec + "' names a backend but no device -- write -o " + spec +
                ":<device>, or -l to list them";
        return false;
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
            if (!entry.takes_device) {
                error = "the " + prefix + " backend takes no device, so -o '" + spec +
                        "' means nothing -- use -o " + prefix + " on its own";
                return false;
            }
            if (rest.empty()) {
                error = "-o '" + spec + "' names no device -- write -o " + prefix +
                        ":<device>, or -l to list them";
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
    //    is no fixed device list to keep in sync with the host's hardware.
#ifdef SENDSPIN_CLI_HAVE_ALSA
    out = {SinkBackend::Alsa, spec};
    return true;
#else
    error = "unknown output device '" + spec + "' -- this build has: " + audio_backend_list() +
            " (run with -l)";
    return false;
#endif
}

std::unique_ptr<AudioSink> make_audio_sink(const std::string& device, std::string& error) {
    DeviceSpec spec;
    if (!resolve_device_spec(device, spec, error)) {
        return nullptr;
    }

    switch (spec.backend) {
        case SinkBackend::Null:
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
            return std::make_unique<AlsaAudioSink>(spec.device);
#else
            break;  // unreachable: resolve_device_spec() never yields Alsa without the backend
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
                 "     backend, so only the names above resolve here.\n");
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
    std::fprintf(out, "\nThis build has no ALSA backend (libasound was missing, or it was\n");
    std::fprintf(out, "configured with -DSENDSPIN_CLI_WITH_ALSA=OFF).\n");
#endif
}

}  // namespace sendspin_cli
