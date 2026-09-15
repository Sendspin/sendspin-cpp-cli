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

/// resolve_device_spec(): how -o reads its argument, without opening a device.

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

// Rule 1: the reserved device-less names

TEST(ResolveDeviceSpec, ReservedNamesResolveToTheDeviceLessSinks) {
    EXPECT_EQ(resolved("null").backend, SinkBackend::Null);
    EXPECT_EQ(resolved("stdout").backend, SinkBackend::Stdout);
    EXPECT_EQ(resolved("-").backend, SinkBackend::Stdout);

    EXPECT_TRUE(resolved("null").device.empty());
    EXPECT_TRUE(resolved("stdout").device.empty());
    EXPECT_TRUE(resolved("-").device.empty());
}

TEST(ResolveDeviceSpec, ReservedNamesWinOverTheAlsaPcmOfTheSameName) {
    // ALSA has a "null" PCM too; -o null must mean the discard sink on every build.
    EXPECT_EQ(resolved("null").backend, SinkBackend::Null);
}

TEST(ResolveDeviceSpec, EmptyIsRejected) {
    EXPECT_NE(rejected("").find("empty"), std::string::npos);
}

// Rule 2: <backend>:<device>

TEST(ResolveDeviceSpec, DeviceLessBackendsRefuseADevice) {
    const std::string null_error = rejected("null:something");
    EXPECT_NE(null_error.find("null"), std::string::npos);
    EXPECT_NE(null_error.find("takes no device"), std::string::npos);

    EXPECT_NE(rejected("stdout:something").find("takes no device"), std::string::npos);
}

TEST(ResolveDeviceSpec, BackendListMatchesTheBuild) {
    EXPECT_NE(audio_backend_list().find("null"), std::string::npos);
    EXPECT_NE(audio_backend_list().find("stdout"), std::string::npos);
#ifdef SENDSPIN_CLI_HAVE_ALSA
    EXPECT_NE(audio_backend_list().find("alsa"), std::string::npos);
#else
    EXPECT_EQ(audio_backend_list().find("alsa"), std::string::npos);
#endif
#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
    EXPECT_NE(audio_backend_list().find("portaudio"), std::string::npos);
#else
    EXPECT_EQ(audio_backend_list().find("portaudio"), std::string::npos);
#endif
#ifdef SENDSPIN_CLI_HAVE_PULSE
    EXPECT_NE(audio_backend_list().find("pulse"), std::string::npos);
#else
    EXPECT_EQ(audio_backend_list().find("pulse"), std::string::npos);
#endif
#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE
    EXPECT_NE(audio_backend_list().find("pipewire"), std::string::npos);
#else
    EXPECT_EQ(audio_backend_list().find("pipewire"), std::string::npos);
#endif
}

#ifdef SENDSPIN_CLI_HAVE_ALSA

TEST(ResolveDeviceSpec, AlsaPrefixSplitsOnTheFirstColonOnly) {
    // Only the first colon separates backend from device.
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
    EXPECT_NE(rejected("alsa").find("no device"), std::string::npos);
}

// Rule 3: a bare ALSA PCM name

TEST(ResolveDeviceSpec, BarePcmNamesStillMeanAlsa) {
    // Not `pulse` or `pipewire`: with native backends those are rule-1 names.
    for (const char* pcm : {"default", "hw:2,0", "plughw:2,0", "hdmi:CARD=NVidia,DEV=0",
                            "surround51:CARD=PCH"}) {
        const DeviceSpec spec = resolved(pcm);
        EXPECT_EQ(spec.backend, SinkBackend::Alsa) << pcm;
        EXPECT_EQ(spec.device, pcm) << pcm;
    }
}

TEST(ResolveDeviceSpec, TheAlsaPrefixIsTheWayBackToAShadowedPluginPcm) {
    // The documented escape hatch to the ALSA plugin PCMs, on every build.
    EXPECT_EQ(resolved("alsa:pulse").backend, SinkBackend::Alsa);
    EXPECT_EQ(resolved("alsa:pulse").device, "pulse");
    EXPECT_EQ(resolved("alsa:pipewire").backend, SinkBackend::Alsa);
    EXPECT_EQ(resolved("alsa:pipewire").device, "pipewire");
}

TEST(ResolveDeviceSpec, ShadowedPcmNamesAreOnlyTheBackendNames) {
    // Derived from the backend table; an ordinary PCM is never filtered.
    for (const char* pcm : {"default", "hw:2,0", "plughw:2,0", "surround51:CARD=PCH"}) {
        EXPECT_TRUE(alsa_pcm_is_reachable(pcm)) << pcm;
    }
    EXPECT_FALSE(alsa_pcm_is_reachable("null"));
    EXPECT_FALSE(alsa_pcm_is_reachable("alsa"));

#ifdef SENDSPIN_CLI_HAVE_PULSE
    EXPECT_FALSE(alsa_pcm_is_reachable("pulse"));
#else
    EXPECT_TRUE(alsa_pcm_is_reachable("pulse"));
#endif
#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE
    EXPECT_FALSE(alsa_pcm_is_reachable("pipewire"));
#else
    EXPECT_TRUE(alsa_pcm_is_reachable("pipewire"));
#endif
}

#else  // no ALSA backend in this build

TEST(ResolveDeviceSpec, AlsaPrefixSaysItIsNotInThisBuild) {
    // A build-configuration problem, distinct from an unknown backend.
    const std::string error = rejected("alsa:default");
    EXPECT_NE(error.find("ALSA backend"), std::string::npos);
    EXPECT_NE(error.find("not in this build"), std::string::npos);
    EXPECT_NE(error.find("SENDSPIN_CLI_WITH_ALSA"), std::string::npos);
    EXPECT_NE(error.find(audio_backend_list()), std::string::npos);
}

TEST(ResolveDeviceSpec, BarePcmNamesHaveNowhereToGo) {
    const std::string error = rejected("hw:2,0");
    EXPECT_NE(error.find("unknown output device"), std::string::npos);
    EXPECT_NE(error.find(audio_backend_list()), std::string::npos);
#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO
    // Names the prefix, since a bare device name is the likely mistake.
    EXPECT_NE(error.find("-o portaudio:hw:2,0"), std::string::npos);
#endif
}

#endif  // SENDSPIN_CLI_HAVE_ALSA

// The PortAudio prefix, whose device is optional

#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO

TEST(ResolveDeviceSpec, BarePortaudioMeansThisHostsDefaultOutput) {
    // An empty device follows the host's default output.
    const DeviceSpec spec = resolved("portaudio");
    EXPECT_EQ(spec.backend, SinkBackend::PortAudio);
    EXPECT_TRUE(spec.device.empty());
}

TEST(ResolveDeviceSpec, PortaudioTakesAnIndexOrAName) {
    EXPECT_EQ(resolved("portaudio:2").backend, SinkBackend::PortAudio);
    EXPECT_EQ(resolved("portaudio:2").device, "2");

    EXPECT_EQ(resolved("portaudio:Built-in Output").device, "Built-in Output");
    EXPECT_EQ(resolved("portaudio:MacBook Pro Speakers").device, "MacBook Pro Speakers");
    EXPECT_EQ(resolved("portaudio:hw:1,0").device, "hw:1,0");
}

TEST(ResolveDeviceSpec, PortaudioPrefixWithNothingAfterTheColonIsRejected) {
    // An empty device after the colon is a truncated command line.
    const std::string error = rejected("portaudio:");
    EXPECT_NE(error.find("no device"), std::string::npos);
    EXPECT_NE(error.find("-o portaudio on its own"), std::string::npos)
        << "the message should point at the form that does mean the default";
}

#else  // no PortAudio backend in this build

TEST(ResolveDeviceSpec, PortaudioSaysItIsNotInThisBuild) {
    // Reserved, so it is not handed to ALSA as a PCM name.
    for (const char* spec : {"portaudio", "portaudio:2", "portaudio:Built-in Output"}) {
        const std::string error = rejected(spec);
        EXPECT_NE(error.find("PortAudio backend"), std::string::npos) << spec;
        EXPECT_NE(error.find("not in this build"), std::string::npos) << spec;
        EXPECT_NE(error.find("SENDSPIN_CLI_WITH_PORTAUDIO"), std::string::npos) << spec;
        EXPECT_NE(error.find(audio_backend_list()), std::string::npos)
            << spec << ": the error should name the backends this build has";
    }
}

#endif  // SENDSPIN_CLI_HAVE_PORTAUDIO

// The sound-server prefixes, which shadow same-named ALSA PCMs where built

#ifdef SENDSPIN_CLI_HAVE_PULSE

TEST(ResolveDeviceSpec, BarePulseMeansTheServersOwnDefaultSink) {
    const DeviceSpec spec = resolved("pulse");
    EXPECT_EQ(spec.backend, SinkBackend::Pulse);
    EXPECT_TRUE(spec.device.empty())
        << "an empty device is how the sink is told to follow the server's default";
}

TEST(ResolveDeviceSpec, PulseTakesASinkName) {
    EXPECT_EQ(resolved("pulse:alsa_output.pci-0000_00_1f.3.analog-stereo").backend,
              SinkBackend::Pulse);
    EXPECT_EQ(resolved("pulse:alsa_output.pci-0000_00_1f.3.analog-stereo").device,
              "alsa_output.pci-0000_00_1f.3.analog-stereo");

    EXPECT_EQ(resolved("pulse:tunnel:hifi").device, "tunnel:hifi");
}

TEST(ResolveDeviceSpec, PulsePrefixWithNothingAfterTheColonIsRejected) {
    // An empty device after the colon is a truncated command line.
    const std::string error = rejected("pulse:");
    EXPECT_NE(error.find("no device"), std::string::npos);
    EXPECT_NE(error.find("-o pulse on its own"), std::string::npos)
        << "the message should point at the form that does mean the default";
}

#else  // no PulseAudio backend in this build

TEST(ResolveDeviceSpec, PulseSaysItIsNotInThisBuildAndNamesTheAlsaRouteToo) {
    // With ALSA built, a bare `pulse` is still an ALSA PCM (see below).
    const std::string error = rejected("pulse:my-sink");
    EXPECT_NE(error.find("PulseAudio backend"), std::string::npos);
    EXPECT_NE(error.find("not in this build"), std::string::npos);
    EXPECT_NE(error.find("SENDSPIN_CLI_WITH_PULSE"), std::string::npos);
    EXPECT_NE(error.find(audio_backend_list()), std::string::npos)
        << "the error should name the backends this build has";
#ifdef SENDSPIN_CLI_HAVE_ALSA
    // Points at the ALSA plugin route that already works.
    EXPECT_NE(error.find("-o alsa:pulse"), std::string::npos);
#endif
}

TEST(ResolveDeviceSpec, BarePulseIsStillTheAlsaPluginPcm) {
#ifdef SENDSPIN_CLI_HAVE_ALSA
    // Without the native backend, a bare name keeps its ALSA meaning.
    EXPECT_EQ(resolved("pulse").backend, SinkBackend::Alsa);
    EXPECT_EQ(resolved("pulse").device, "pulse");
#else
    // No ALSA either, so the reserved entry names the flag.
    EXPECT_NE(rejected("pulse").find("PulseAudio backend"), std::string::npos);
#endif
}

#endif  // SENDSPIN_CLI_HAVE_PULSE

#ifdef SENDSPIN_CLI_HAVE_PIPEWIRE

TEST(ResolveDeviceSpec, BarePipewireMeansTheGraphsOwnRouting) {
    const DeviceSpec spec = resolved("pipewire");
    EXPECT_EQ(spec.backend, SinkBackend::PipeWire);
    EXPECT_TRUE(spec.device.empty())
        << "an empty device is how the sink is told to let the graph route it";
}

TEST(ResolveDeviceSpec, PipewireTakesANodeName) {
    EXPECT_EQ(resolved("pipewire:alsa_output.usb-Topping_D10s").backend, SinkBackend::PipeWire);
    EXPECT_EQ(resolved("pipewire:alsa_output.usb-Topping_D10s").device,
              "alsa_output.usb-Topping_D10s");

    EXPECT_EQ(resolved("pipewire:bluez_output:44:5C").device, "bluez_output:44:5C");
}

TEST(ResolveDeviceSpec, PipewirePrefixWithNothingAfterTheColonIsRejected) {
    const std::string error = rejected("pipewire:");
    EXPECT_NE(error.find("no device"), std::string::npos);
    EXPECT_NE(error.find("-o pipewire on its own"), std::string::npos)
        << "the message should point at the form that does mean the default";
}

#else  // no PipeWire backend in this build

TEST(ResolveDeviceSpec, PipewireSaysItIsNotInThisBuildAndNamesTheAlsaRouteToo) {
    // With ALSA built, a bare `pipewire` is still an ALSA PCM (see below).
    const std::string error = rejected("pipewire:my-node");
    EXPECT_NE(error.find("PipeWire backend"), std::string::npos);
    EXPECT_NE(error.find("not in this build"), std::string::npos);
    EXPECT_NE(error.find("SENDSPIN_CLI_WITH_PIPEWIRE"), std::string::npos);
    EXPECT_NE(error.find(audio_backend_list()), std::string::npos)
        << "the error should name the backends this build has";
#ifdef SENDSPIN_CLI_HAVE_ALSA
    EXPECT_NE(error.find("-o alsa:pipewire"), std::string::npos);
#endif
}

TEST(ResolveDeviceSpec, BarePipewireIsStillTheAlsaPluginPcm) {
#ifdef SENDSPIN_CLI_HAVE_ALSA
    // Without the native backend, a bare name keeps its ALSA meaning.
    EXPECT_EQ(resolved("pipewire").backend, SinkBackend::Alsa);
    EXPECT_EQ(resolved("pipewire").device, "pipewire");
#else
    // No ALSA either, so the reserved entry names the flag.
    EXPECT_NE(rejected("pipewire").find("PipeWire backend"), std::string::npos);
#endif
}

#endif  // SENDSPIN_CLI_HAVE_PIPEWIRE

}  // namespace
}  // namespace sendspin_cli
