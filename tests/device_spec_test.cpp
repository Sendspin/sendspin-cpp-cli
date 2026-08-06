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

/// @file device_spec_test.cpp
/// @brief resolve_device_spec(): how -o reads its argument
///
/// Pure string work, so none of this opens a device -- these tests pass on a machine with
/// no sound card, and the ALSA-specific expectations are compiled per build.

#include "audio_sink.h"

#include <gtest/gtest.h>

#include <string>

namespace sendspin_cli {
namespace {

/// Resolves `spec`, requiring success, and returns what it resolved to.
DeviceSpec resolved(const std::string& spec) {
    DeviceSpec out;
    std::string error;
    EXPECT_TRUE(resolve_device_spec(spec, out, error)) << spec << ": " << error;
    return out;
}

/// Resolves `spec`, requiring failure, and returns the reason given.
std::string rejected(const std::string& spec) {
    DeviceSpec out;
    std::string error;
    EXPECT_FALSE(resolve_device_spec(spec, out, error)) << "accepted '" << spec << "'";
    EXPECT_FALSE(error.empty()) << "no reason given for '" << spec << "'";
    return error;
}

// ---------------------------------------------------------------------------
// Rule 1: the reserved device-less names
// ---------------------------------------------------------------------------

TEST(ResolveDeviceSpec, ReservedNamesResolveToTheDeviceLessSinks) {
    EXPECT_EQ(resolved("null").backend, SinkBackend::Null);
    EXPECT_EQ(resolved("stdout").backend, SinkBackend::Stdout);
    EXPECT_EQ(resolved("-").backend, SinkBackend::Stdout);

    // They take no device, so nothing should be carried through.
    EXPECT_TRUE(resolved("null").device.empty());
    EXPECT_TRUE(resolved("stdout").device.empty());
    EXPECT_TRUE(resolved("-").device.empty());
}

TEST(ResolveDeviceSpec, ReservedNamesWinOverTheAlsaPcmOfTheSameName) {
    // ALSA ships a PCM called "null" too. -o null has to keep meaning the discard sink on
    // every build, or the same command line would do different things per host.
    EXPECT_EQ(resolved("null").backend, SinkBackend::Null);
}

TEST(ResolveDeviceSpec, EmptyIsRejected) {
    EXPECT_NE(rejected("").find("empty"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Rule 2: <backend>:<device>
// ---------------------------------------------------------------------------

TEST(ResolveDeviceSpec, DeviceLessBackendsRefuseADevice) {
    const std::string null_error = rejected("null:something");
    EXPECT_NE(null_error.find("null"), std::string::npos);
    EXPECT_NE(null_error.find("takes no device"), std::string::npos);

    EXPECT_NE(rejected("stdout:something").find("takes no device"), std::string::npos);
}

TEST(ResolveDeviceSpec, AReservedBackendPrefixThisBuildLacksIsNamed) {
    // Reserved on purpose: without the entry this would fall through to rule 3 and be
    // handed to ALSA as a PCM name, failing with "Unknown PCM portaudio:2" rather than
    // saying which backends exist.
    const std::string error = rejected("portaudio:2");
    EXPECT_NE(error.find("PortAudio"), std::string::npos);
    EXPECT_NE(error.find(audio_backend_list()), std::string::npos)
        << "the error should name the backends this build has";
}

TEST(ResolveDeviceSpec, BackendListMatchesTheBuild) {
    EXPECT_NE(audio_backend_list().find("null"), std::string::npos);
    EXPECT_NE(audio_backend_list().find("stdout"), std::string::npos);
#ifdef SENDSPIN_CLI_HAVE_ALSA
    EXPECT_NE(audio_backend_list().find("alsa"), std::string::npos);
#else
    EXPECT_EQ(audio_backend_list().find("alsa"), std::string::npos);
#endif
}

#ifdef SENDSPIN_CLI_HAVE_ALSA

TEST(ResolveDeviceSpec, AlsaPrefixSplitsOnTheFirstColonOnly) {
    // ALSA device names carry their own colons, so only the first one separates the
    // backend from the device.
    const DeviceSpec spec = resolved("alsa:hw:2,0");
    EXPECT_EQ(spec.backend, SinkBackend::Alsa);
    EXPECT_EQ(spec.device, "hw:2,0");

    EXPECT_EQ(resolved("alsa:default").device, "default");
    EXPECT_EQ(resolved("alsa:hdmi:CARD=NVidia,DEV=0").device, "hdmi:CARD=NVidia,DEV=0");
}

TEST(ResolveDeviceSpec, AlsaPrefixWithNoDeviceIsRejected) {
    EXPECT_NE(rejected("alsa:").find("no device"), std::string::npos);
}

TEST(ResolveDeviceSpec, ABareBackendNameIsRejected) {
    // -o alsa names a backend but no device. Saying so beats handing "alsa" to ALSA as a
    // PCM name and reporting that no such PCM exists.
    EXPECT_NE(rejected("alsa").find("no device"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Rule 3: a bare ALSA PCM name -- what every existing invocation relies on
// ---------------------------------------------------------------------------

TEST(ResolveDeviceSpec, BarePcmNamesStillMeanAlsa) {
    for (const char* pcm : {"default", "hw:2,0", "plughw:2,0", "pipewire", "pulse",
                            "hdmi:CARD=NVidia,DEV=0", "surround51:CARD=PCH"}) {
        const DeviceSpec spec = resolved(pcm);
        EXPECT_EQ(spec.backend, SinkBackend::Alsa) << pcm;
        EXPECT_EQ(spec.device, pcm) << pcm;
    }
}

#else  // no ALSA backend in this build

TEST(ResolveDeviceSpec, AlsaPrefixSaysItIsNotInThisBuild) {
    // Distinct from the message a backend this project has never built gets: this one is
    // a build-configuration problem with a build-configuration fix.
    const std::string error = rejected("alsa:default");
    EXPECT_NE(error.find("not in this build"), std::string::npos);
    EXPECT_NE(error.find("SENDSPIN_CLI_WITH_ALSA"), std::string::npos);
    EXPECT_NE(error.find(audio_backend_list()), std::string::npos);
    EXPECT_EQ(error.find("PortAudio"), std::string::npos);
}

TEST(ResolveDeviceSpec, BarePcmNamesHaveNowhereToGo) {
    const std::string error = rejected("hw:2,0");
    EXPECT_NE(error.find("unknown output device"), std::string::npos);
    EXPECT_NE(error.find(audio_backend_list()), std::string::npos);
}

#endif  // SENDSPIN_CLI_HAVE_ALSA

}  // namespace
}  // namespace sendspin_cli
