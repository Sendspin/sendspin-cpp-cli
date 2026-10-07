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

/// The capture seam: how --input reads its argument, and SourceCapture's thread.

#include "audio_source.h"

#include "null_source.h"
#include "scoped_env.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sendspin_cli {
namespace {

InputSpec resolved(const std::string& spec) {
    InputSpec out;
    std::string error;
    EXPECT_TRUE(resolve_input_spec(spec, out, error)) << spec << ": " << error;
    return out;
}

std::string rejected(const std::string& spec) {
    InputSpec out;
    std::string error;
    EXPECT_FALSE(resolve_input_spec(spec, out, error)) << "accepted '" << spec << "'";
    EXPECT_FALSE(error.empty()) << "no reason given for '" << spec << "'";
    return error;
}

/// Records what the capture thread hands the role.
class RecordingWriter {
public:
    SourceCapture::Writer writer() {
        return [this](const uint8_t* /*data*/, size_t len, int64_t capture_time_us) {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->lengths_.push_back(len);
            this->times_.push_back(capture_time_us);
            this->threads_.push_back(std::this_thread::get_id());
            return true;
        };
    }

    size_t writes() {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->lengths_.size();
    }

    /// Polls until `count` writes have arrived, for up to two seconds.
    bool wait_for(size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (this->writes() < count) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    std::mutex mutex_;
    std::vector<size_t> lengths_;
    std::vector<int64_t> times_;
    std::vector<std::thread::id> threads_;
};

/// A device that never opens, counting the attempts.
class DeadSource final : public AudioSource {
public:
    std::string name() const override {
        return "dead";
    }
    bool negotiate(StreamFormat& /*format*/, std::string& /*error*/) override {
        return true;
    }
    bool open(const StreamFormat& /*format*/) override {
        ++this->opens;
        return false;
    }
    int read(uint8_t* /*data*/, size_t /*length*/, uint32_t /*timeout_ms*/,
             int64_t& /*capture_time_us*/) override {
        return -1;
    }
    void close() override {}

    std::atomic<int> opens{0};
};

/// A device with no clock: every read returns a full buffer at once.
class EagerSource final : public AudioSource {
public:
    std::string name() const override {
        return "eager";
    }
    bool negotiate(StreamFormat& /*format*/, std::string& /*error*/) override {
        return true;
    }
    bool open(const StreamFormat& /*format*/) override {
        return true;
    }
    int read(uint8_t* /*data*/, size_t length, uint32_t /*timeout_ms*/,
             int64_t& /*capture_time_us*/) override {
        return static_cast<int>(length);
    }
    void close() override {}
};

constexpr StreamFormat STEREO_16{48000, 2, 16};

// How --input reads its argument

TEST(ResolveInputSpec, DeviceLessNamesResolveWithoutADevice) {
    EXPECT_EQ(resolved("null").backend, SourceBackend::Null);
    EXPECT_EQ(resolved("tone").backend, SourceBackend::Tone);
    EXPECT_TRUE(resolved("tone").device.empty());
}

TEST(ResolveInputSpec, EmptyIsRejected) {
    EXPECT_NE(rejected("").find("empty"), std::string::npos);
}

TEST(ResolveInputSpec, DeviceLessNamesRefuseADevice) {
    EXPECT_NE(rejected("null:hw:0").find("takes no device"), std::string::npos);
    EXPECT_NE(rejected("tone:x").find("takes no device"), std::string::npos);
}

TEST(ResolveInputSpec, OutputOnlyBackendsAreRefusedByName) {
    for (const char* spec : {"portaudio", "portaudio:1"}) {
        const std::string error = rejected(spec);
        EXPECT_NE(error.find("not supported yet"), std::string::npos) << spec;
        EXPECT_NE(error.find(input_backend_list()), std::string::npos) << spec;
    }
}

#ifdef SENDSPIN_CLI_HAVE_COREAUDIO
TEST(ResolveInputSpec, BareCoreaudioMeansThisHostsDefaultInput) {
    EXPECT_EQ(resolved("coreaudio").backend, SourceBackend::CoreAudio);
    EXPECT_TRUE(resolved("coreaudio").device.empty());
}

TEST(ResolveInputSpec, CoreaudioTakesAnIndexOrAName) {
    EXPECT_EQ(resolved("coreaudio:2").backend, SourceBackend::CoreAudio);
    EXPECT_EQ(resolved("coreaudio:2").device, "2");
    EXPECT_EQ(resolved("coreaudio:MacBook Pro Microphone").device, "MacBook Pro Microphone");
    EXPECT_EQ(resolved("coreaudio:BlackHole 2ch: Aggregate").device, "BlackHole 2ch: Aggregate");
}

TEST(ResolveInputSpec, CoreaudioPrefixWithNothingAfterTheColonIsRejected) {
    EXPECT_NE(rejected("coreaudio:").find("--input coreaudio on its own"), std::string::npos);
}
#else
TEST(ResolveInputSpec, CoreaudioSaysItIsNotInThisBuild) {
    for (const char* spec : {"coreaudio", "coreaudio:2", "coreaudio:MacBook Pro Microphone"}) {
        const std::string error = rejected(spec);
        EXPECT_NE(error.find("CoreAudio backend is not in this build"), std::string::npos) << spec;
        EXPECT_NE(error.find(input_backend_list()), std::string::npos) << spec;
    }
}
#endif

#ifdef SENDSPIN_CLI_HAVE_PULSE
TEST(ResolveInputSpec, PulseTakesASourceOrTheServerDefault) {
    for (const char* spec : {"pulse", "pulse:", "pulse:default"}) {
        EXPECT_EQ(resolved(spec).backend, SourceBackend::Pulse) << spec;
        EXPECT_TRUE(resolved(spec).device.empty()) << spec;
    }
    EXPECT_EQ(resolved("pulse:alsa_input.usb-mic.analog-stereo").device,
              "alsa_input.usb-mic.analog-stereo");
    // Split on the first colon only: the rest is the source's name, colons and all.
    EXPECT_EQ(resolved("pulse:a:b").device, "a:b");
    EXPECT_NE(input_backend_list().find("pulse"), std::string::npos);
}

TEST(MakeAudioSource, AnUnreachablePulseServerIsRefusedNamingTheListing) {
    const ScopedEnv server("PULSE_SERVER", "unix:/nonexistent/sendspin-cli-test");
    StreamFormat format = STEREO_16;
    std::string error;
    EXPECT_EQ(make_audio_source("pulse", format, error), nullptr);
    EXPECT_NE(error.find("PulseAudio"), std::string::npos) << error;
    EXPECT_NE(error.find("-l"), std::string::npos) << error;
}
#else
TEST(ResolveInputSpec, WithoutPulseItsNameIsNotInThisBuild) {
    for (const char* spec : {"pulse", "pulse:mic"}) {
        const std::string error = rejected(spec);
        EXPECT_NE(error.find("PulseAudio backend is not in this build"), std::string::npos) << spec;
        EXPECT_NE(error.find(input_backend_list()), std::string::npos) << spec;
    }
}
#endif

#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE
TEST(ResolveInputSpec, PipeWireTakesANodeOrTheGraphDefault) {
    for (const char* spec : {"pipewire", "pipewire:", "pipewire:default"}) {
        EXPECT_EQ(resolved(spec).backend, SourceBackend::PipeWire) << spec;
        EXPECT_TRUE(resolved(spec).device.empty()) << spec;
    }
    EXPECT_EQ(resolved("pipewire:alsa_input.pci-0000_00_1f.3.analog-stereo").device,
              "alsa_input.pci-0000_00_1f.3.analog-stereo");
    EXPECT_NE(input_backend_list().find("pipewire"), std::string::npos);
}

TEST(MakeAudioSource, AnUnreachablePipeWireDaemonIsRefusedNamingTheListing) {
    const ScopedEnv remote("PIPEWIRE_REMOTE", "/nonexistent/sendspin-cli-test");
    StreamFormat format = STEREO_16;
    std::string error;
    EXPECT_EQ(make_audio_source("pipewire", format, error), nullptr);
    EXPECT_NE(error.find("PipeWire"), std::string::npos) << error;
    EXPECT_NE(error.find("-l"), std::string::npos) << error;
}
#else
TEST(ResolveInputSpec, WithoutPipeWireItsNameIsNotInThisBuild) {
    for (const char* spec : {"pipewire", "pipewire:node"}) {
        const std::string error = rejected(spec);
        EXPECT_NE(error.find("PipeWire backend is not in this build"), std::string::npos) << spec;
        EXPECT_NE(error.find(input_backend_list()), std::string::npos) << spec;
    }
}
#endif

TEST(SettleServerCaptureFormat, KeepsWhatAStreamCanCarry) {
    for (const uint8_t depth : CAPTURE_BIT_DEPTHS) {
        StreamFormat format{44100, 1, depth};
        settle_server_capture_format(format);
        EXPECT_EQ(format.sample_rate, 44100U);
        EXPECT_EQ(format.channels, 1);
        EXPECT_EQ(format.bit_depth, depth);
    }
}

TEST(SettleServerCaptureFormat, ReplacesWhatItCannot) {
    StreamFormat format{0, 0, 8};
    settle_server_capture_format(format);
    EXPECT_EQ(format.sample_rate, 48000U);
    EXPECT_EQ(format.channels, 2);
    EXPECT_EQ(format.bit_depth, 16);
}

#ifdef SENDSPIN_CLI_HAVE_ALSA
TEST(ResolveInputSpec, BareNamesAreAlsaPcmsColonsAndAll) {
    EXPECT_EQ(resolved("default").backend, SourceBackend::Alsa);
    EXPECT_EQ(resolved("default").device, "default");
    EXPECT_EQ(resolved("hw:1,0").device, "hw:1,0");
    EXPECT_EQ(resolved("plughw:CARD=Device,DEV=0").device, "plughw:CARD=Device,DEV=0");
}

TEST(ResolveInputSpec, TheAlsaPrefixSplitsOnTheFirstColon) {
    EXPECT_EQ(resolved("alsa:hw:1,0").backend, SourceBackend::Alsa);
    EXPECT_EQ(resolved("alsa:hw:1,0").device, "hw:1,0");
    EXPECT_EQ(resolved("alsa:null").device, "null");
}

TEST(ResolveInputSpec, TheAlsaPrefixNeedsADevice) {
    EXPECT_NE(rejected("alsa").find("alsa:<device>"), std::string::npos);
    EXPECT_NE(rejected("alsa:").find("alsa:<device>"), std::string::npos);
}

TEST(ResolveInputSpec, OnlyUnshadowedPcmNamesAreReachableBare) {
    EXPECT_TRUE(input_pcm_is_reachable("hw:1,0"));
    EXPECT_FALSE(input_pcm_is_reachable("null"));
    EXPECT_FALSE(input_pcm_is_reachable("pulse"));
}

TEST(MakeAudioSource, AnUnopenableDeviceIsRefusedNamingItAndTheListing) {
    StreamFormat format = STEREO_16;
    std::string error;
    EXPECT_EQ(make_audio_source("sendspin-cli-no-such-pcm", format, error), nullptr);
    EXPECT_NE(error.find("'sendspin-cli-no-such-pcm'"), std::string::npos) << error;
    EXPECT_NE(error.find("-l"), std::string::npos) << error;
}
#else
TEST(ResolveInputSpec, WithoutAlsaABareNameIsUnknown) {
    EXPECT_NE(rejected("hw:1,0").find("unknown input device"), std::string::npos);
    EXPECT_NE(rejected("alsa:hw:1,0").find("not in this build"), std::string::npos);
}
#endif

TEST(MakeAudioSource, ABadSpecIsRefusedBeforeAnythingIsBuilt) {
    StreamFormat format = STEREO_16;
    std::string error;
    EXPECT_EQ(make_audio_source("tone:x", format, error), nullptr);
    EXPECT_FALSE(error.empty());
}

TEST(MakeAudioSource, TheToneSourceKeepsThePreferredFormat) {
    StreamFormat format = STEREO_16;
    std::string error;
    const std::unique_ptr<AudioSource> source = make_audio_source("tone", format, error);
    ASSERT_NE(source, nullptr) << error;
    EXPECT_EQ(source->name(), "tone");
    EXPECT_EQ(format.sample_rate, 48000U);
    EXPECT_EQ(format.channels, 2);
    EXPECT_EQ(format.bit_depth, 16);
}

TEST(MakeAudioSource, TheToneSourceRefusesADepthTheRoleCannotSend) {
    StreamFormat format{48000, 2, 8};
    std::string error;
    EXPECT_EQ(make_audio_source("tone", format, error), nullptr);
    EXPECT_NE(error.find("8-bit"), std::string::npos) << error;
}

// The test tone

TEST(NullAudioSource, ReadsArePacedTimestampedAndWholeFrames) {
    NullAudioSource source(NullSourceSignal::Tone);
    const StreamFormat format{44100, 2, 24};
    ASSERT_TRUE(source.open(format));

    std::vector<uint8_t> buffer(882 * 6);
    int64_t first_time = 0;
    int64_t second_time = 0;
    const auto opened = std::chrono::steady_clock::now();
    const int first = source.read(buffer.data(), buffer.size(), 100, first_time);
    const bool audible =
        std::any_of(buffer.begin(), buffer.end(), [](uint8_t b) { return b != 0; });
    const int second = source.read(buffer.data(), buffer.size(), 100, second_time);

    // Two 20 ms reads cannot finish before 40 ms of audio exists.
    EXPECT_GE(std::chrono::steady_clock::now() - opened, std::chrono::milliseconds(39));
    EXPECT_EQ(first, 882 * 6);
    EXPECT_EQ(second, 882 * 6);
    EXPECT_TRUE(audible);
    EXPECT_GT(first_time, 0);
    // Stamped by frame count, so consecutive reads are exactly one read apart.
    EXPECT_EQ(second_time - first_time, 20000);
}

TEST(NullAudioSource, ATimeoutShorterThanTheReadReturnsOnlyWhatIsDue) {
    NullAudioSource source(NullSourceSignal::Silence);
    ASSERT_TRUE(source.open(STEREO_16));

    std::vector<uint8_t> buffer(48000 * 4);
    int64_t time = 0;
    const int bytes = source.read(buffer.data(), buffer.size(), 20, time);

    EXPECT_LT(bytes, static_cast<int>(buffer.size()));
    EXPECT_EQ(bytes % 4, 0);
}

TEST(NullAudioSource, AClosedSourceReadsAsLost) {
    NullAudioSource source(NullSourceSignal::Tone);
    std::vector<uint8_t> buffer(64);
    int64_t time = 0;
    EXPECT_LT(source.read(buffer.data(), buffer.size(), 1, time), 0);
}

// SourceCapture

TEST(SourceCapture, NothingIsCapturedUntilTheStreamStarts) {
    NullAudioSource source(NullSourceSignal::Tone);
    RecordingWriter recorder;
    SourceCapture capture(source, STEREO_16, recorder.writer());

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    EXPECT_FALSE(capture.streaming());
    EXPECT_EQ(recorder.writes(), 0U);
}

TEST(SourceCapture, StartCapturesAndStopJoinsTheThread) {
    NullAudioSource source(NullSourceSignal::Tone);
    RecordingWriter recorder;
    SourceCapture capture(source, STEREO_16, recorder.writer());

    capture.on_streaming_started();
    EXPECT_TRUE(capture.streaming());
    ASSERT_TRUE(recorder.wait_for(3));

    capture.on_streaming_stopped();
    EXPECT_FALSE(capture.streaming());
    // Joined, so nothing can arrive after the stop returned.
    const size_t at_stop = recorder.writes();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(recorder.writes(), at_stop);
}

TEST(SourceCapture, EveryWriteIsWholeFramesFromOneThread) {
    NullAudioSource source(NullSourceSignal::Tone);
    RecordingWriter recorder;
    // 6 bytes a frame, and 882 frames a read: no power of two to hide a split frame.
    const StreamFormat format{44100, 2, 24};
    SourceCapture capture(source, format, recorder.writer());

    capture.on_streaming_started();
    ASSERT_TRUE(recorder.wait_for(5));
    capture.on_streaming_stopped();

    for (size_t i = 0; i < recorder.lengths_.size(); ++i) {
        EXPECT_GT(recorder.lengths_[i], 0U);
        EXPECT_EQ(recorder.lengths_[i] % 6, 0U) << "write " << i;
        EXPECT_GT(recorder.times_[i], 0) << "write " << i;
        EXPECT_EQ(recorder.threads_[i], recorder.threads_[0]);
        EXPECT_NE(recorder.threads_[i], std::this_thread::get_id());
    }
}

TEST(SourceCapture, AStreamCanBeStartedAgainAfterAStop) {
    NullAudioSource source(NullSourceSignal::Silence);
    RecordingWriter recorder;
    SourceCapture capture(source, STEREO_16, recorder.writer());

    capture.on_streaming_started();
    ASSERT_TRUE(recorder.wait_for(1));
    capture.on_streaming_stopped();
    const size_t first_run = recorder.writes();

    capture.on_streaming_started();
    ASSERT_TRUE(recorder.wait_for(first_run + 1));
    capture.on_streaming_stopped();
}

TEST(SourceCapture, RepeatedCallbacksAreHarmless) {
    NullAudioSource source(NullSourceSignal::Silence);
    RecordingWriter recorder;
    SourceCapture capture(source, STEREO_16, recorder.writer());

    capture.on_streaming_stopped();
    capture.on_streaming_started();
    capture.on_streaming_started();
    EXPECT_TRUE(capture.streaming());
    capture.on_streaming_stopped();
    capture.on_streaming_stopped();
    EXPECT_FALSE(capture.streaming());
}

TEST(SourceCapture, DestructionJoinsARunningThread) {
    NullAudioSource source(NullSourceSignal::Tone);
    RecordingWriter recorder;
    {
        SourceCapture capture(source, STEREO_16, recorder.writer());
        capture.on_streaming_started();
        ASSERT_TRUE(recorder.wait_for(1));
    }
    const size_t at_destruction = recorder.writes();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(recorder.writes(), at_destruction);
}

TEST(SourceCapture, AnUnclockedDeviceIsHeldNearRealTime) {
    EagerSource source;
    RecordingWriter recorder;
    SourceCapture capture(source, STEREO_16, recorder.writer());

    capture.on_streaming_started();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto before = std::chrono::steady_clock::now();
    capture.on_streaming_stopped();

    // One window's allowance plus the read that crossed it, not 200 ms of free-running reads.
    size_t frames = 0;
    for (const size_t length : recorder.lengths_) {
        frames += length / 4;
    }
    EXPECT_LE(frames, 48000U * 3U / 2U + 960U);
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::milliseconds(500));
}

TEST(SourceCapture, AStopIsNotHeldUpByADeviceThatWillNotOpen) {
    DeadSource source;
    RecordingWriter recorder;
    SourceCapture capture(source, STEREO_16, recorder.writer());

    capture.on_streaming_started();
    while (source.opens.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto before = std::chrono::steady_clock::now();
    capture.on_streaming_stopped();
    const auto took = std::chrono::steady_clock::now() - before;

    // The retry delay is seconds long; a stop must cut it short.
    EXPECT_LT(took, std::chrono::milliseconds(500));
    EXPECT_EQ(recorder.writes(), 0U);
}

}  // namespace
}  // namespace sendspin_cli
