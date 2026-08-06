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

#include <memory>
#include <string>

namespace sendspin_cli {

std::unique_ptr<AudioSink> make_audio_sink(const std::string& device, std::string& error) {
    if (device == "null") {
        return std::make_unique<NullAudioSink>(NullSinkOutput::Discard);
    }
    if (device == "stdout" || device == "-") {
        return std::make_unique<NullAudioSink>(NullSinkOutput::Stdout);
    }

    error = "unknown output device '" + device + "' -- run with -l to list what this build has";
    return nullptr;
}

void print_audio_devices(std::FILE* out) {
    std::fprintf(out, "Output devices (-o):\n");
    std::fprintf(out, "  null      discard audio; needs no sound card at all (default)\n");
    std::fprintf(out, "  stdout    raw interleaved PCM on stdout, e.g. | aplay -f cd\n");
    std::fprintf(out, "  -         alias for stdout\n");
    std::fprintf(out, "\n");
    std::fprintf(out, "ALSA and PortAudio backends are not in this build yet.\n");
    std::fprintf(out, "See docs/ROADMAP.md for the tasks that add them.\n");
}

}  // namespace sendspin_cli
