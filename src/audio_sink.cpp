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

std::unique_ptr<AudioSink> make_audio_sink(const std::string& device, std::string& error) {
    // These three names are reserved for the device-less sinks on every build, so they mean
    // the same thing everywhere. ALSA ships a PCM called "null" too; ours wins.
    if (device == "null") {
        return std::make_unique<NullAudioSink>(NullSinkOutput::Discard);
    }
    if (device == "stdout" || device == "-") {
        return std::make_unique<NullAudioSink>(NullSinkOutput::Stdout);
    }

#ifdef SENDSPIN_CLI_HAVE_ALSA
    // Everything else is an ALSA PCM name, which is how squeezelite's -o behaves: there is
    // no fixed device list to keep in sync with the host's hardware.
    if (device.empty()) {
        error = "empty output device -- run with -l to list what this build has";
        return nullptr;
    }
    // Probed now rather than at the first stream, so a typo fails while someone is still
    // watching the terminal instead of minutes later when the server starts a track.
    if (!AlsaAudioSink::probe(device, error)) {
        return nullptr;
    }
    return std::make_unique<AlsaAudioSink>(device);
#else
    error = "unknown output device '" + device +
            "' -- this build has no ALSA backend, so only null, stdout and - are available "
            "(run with -l)";
    return nullptr;
#endif
}

void print_audio_devices(std::FILE* out) {
    std::fprintf(out, "Output devices (-o):\n");
    std::fprintf(out, "  null      discard audio; needs no sound card at all\n");
    std::fprintf(out, "  stdout    raw interleaved PCM on stdout, e.g. | aplay -f cd\n");
    std::fprintf(out, "  -         alias for stdout\n");

#ifdef SENDSPIN_CLI_HAVE_ALSA
    std::fprintf(out, "\nALSA PCMs on this host (any of these names can follow -o):\n");
    AlsaAudioSink::list_devices(out);
    std::fprintf(out,
                 "\nHardware PCMs also accept the short hw:<card>,<device> and\n"
                 "plughw:<card>,<device> forms -- plughw converts rates and formats the\n"
                 "device itself will not take.\n");
#else
    std::fprintf(out, "\nThis build has no ALSA backend (libasound was missing, or it was\n");
    std::fprintf(out, "configured with -DSENDSPIN_CLI_WITH_ALSA=OFF).\n");
#endif
}

}  // namespace sendspin_cli
