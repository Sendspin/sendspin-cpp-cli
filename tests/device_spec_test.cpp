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
/// no sound card, and each backend's expectations are compiled per build.

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
    // `pulse` and `pipewire` are deliberately not in this list: on a build with the native
    // backends they are rule-1 names now, which is what ShadowedPcmNamesAreOnlyTheBackendNames
    // below covers on both kinds of build.
    for (const char* pcm : {"default", "hw:2,0", "plughw:2,0", "hdmi:CARD=NVidia,DEV=0",
                            "surround51:CARD=PCH"}) {
        const DeviceSpec spec = resolved(pcm);
        EXPECT_EQ(spec.backend, SinkBackend::Alsa) << pcm;
        EXPECT_EQ(spec.device, pcm) << pcm;
    }
}

TEST(ResolveDeviceSpec, TheAlsaPrefixIsTheWayBackToAShadowedPluginPcm) {
    // The escape hatch the pulse/pipewire prefixes owe: rule 2 was already unambiguous, and it
    // has to stay that way whether or not this build has the native backends, because it is what
    // every message about the shadowing points at.
    EXPECT_EQ(resolved("alsa:pulse").backend, SinkBackend::Alsa);
    EXPECT_EQ(resolved("alsa:pulse").device, "pulse");
    EXPECT_EQ(resolved("alsa:pipewire").backend, SinkBackend::Alsa);
    EXPECT_EQ(resolved("alsa:pipewire").device, "pipewire");
}

TEST(ResolveDeviceSpec, ShadowedPcmNamesAreOnlyTheBackendNames) {
    // What -l filters out of the ALSA PCM list, derived from the backend table rather than from a
    // second list of names. An ordinary PCM must never be filtered, whatever the build has.
    for (const char* pcm : {"default", "hw:2,0", "plughw:2,0", "surround51:CARD=PCH"}) {
        EXPECT_TRUE(alsa_pcm_is_reachable(pcm)) << pcm;
    }
    // ALSA ships a `null` PCM on every host, and -o null has always meant the discard sink.
    EXPECT_FALSE(alsa_pcm_is_reachable("null"));
    // A bare `alsa` is refused rather than resolved, so no PCM of that name is reachable either.
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
    // Distinct from the message a backend this project has never built gets: this one is
    // a build-configuration problem with a build-configuration fix.
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
    // The likeliest reason to land here on a PortAudio-only build is a device name typed
    // without its prefix, so the message has to name the prefix, not just the backend list.
    EXPECT_NE(error.find("-o portaudio:hw:2,0"), std::string::npos);
#endif
}

#endif  // SENDSPIN_CLI_HAVE_ALSA

// ---------------------------------------------------------------------------
// The PortAudio prefix, whose device is optional
// ---------------------------------------------------------------------------

#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO

TEST(ResolveDeviceSpec, BarePortaudioMeansThisHostsDefaultOutput) {
    // The one backend that resolves with no device: an empty DeviceSpec::device is how the
    // sink is told to follow whatever the host's default output currently is.
    const DeviceSpec spec = resolved("portaudio");
    EXPECT_EQ(spec.backend, SinkBackend::PortAudio);
    EXPECT_TRUE(spec.device.empty());
}

TEST(ResolveDeviceSpec, PortaudioTakesAnIndexOrAName) {
    EXPECT_EQ(resolved("portaudio:2").backend, SinkBackend::PortAudio);
    EXPECT_EQ(resolved("portaudio:2").device, "2");

    // Device names carry spaces, and everything after the first colon is the device -- so a
    // name with a colon of its own survives too.
    EXPECT_EQ(resolved("portaudio:Built-in Output").device, "Built-in Output");
    EXPECT_EQ(resolved("portaudio:MacBook Pro Speakers").device, "MacBook Pro Speakers");
    EXPECT_EQ(resolved("portaudio:hw:1,0").device, "hw:1,0");
}

TEST(ResolveDeviceSpec, PortaudioPrefixWithNothingAfterTheColonIsRejected) {
    // A written-but-empty device is a truncated command line, not a request for the default:
    // -o portaudio already says that, and saying it twice two ways would hide a typo.
    const std::string error = rejected("portaudio:");
    EXPECT_NE(error.find("no device"), std::string::npos);
    EXPECT_NE(error.find("-o portaudio on its own"), std::string::npos)
        << "the message should point at the form that does mean the default";
}

#else  // no PortAudio backend in this build

TEST(ResolveDeviceSpec, PortaudioSaysItIsNotInThisBuild) {
    // Reserved on purpose: without the entry this would fall through to rule 3 and be handed
    // to ALSA as a PCM name, failing with "Unknown PCM portaudio:2" rather than saying which
    // backends exist and which flag turns this one on.
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

// ---------------------------------------------------------------------------
// The sound-server prefixes, whose devices are optional -- and which shadow an
// ALSA plugin PCM of the same name wherever they are built
// ---------------------------------------------------------------------------

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

    // Everything after the FIRST colon is the sink, so a name carrying its own colon survives.
    EXPECT_EQ(resolved("pulse:tunnel:hifi").device, "tunnel:hifi");
}

TEST(ResolveDeviceSpec, PulsePrefixWithNothingAfterTheColonIsRejected) {
    // A written-but-empty device is a truncated command line, not a request for the default:
    // -o pulse already says that, and saying it twice two ways would hide a typo.
    const std::string error = rejected("pulse:");
    EXPECT_NE(error.find("no device"), std::string::npos);
    EXPECT_NE(error.find("-o pulse on its own"), std::string::npos)
        << "the message should point at the form that does mean the default";
}

#else  // no PulseAudio backend in this build

TEST(ResolveDeviceSpec, PulseSaysItIsNotInThisBuildAndNamesTheAlsaRouteToo) {
    // Only the prefixed form lands here on a build that has ALSA: a bare `pulse` is still an ALSA
    // PCM name there, which is what BarePulseIsStillTheAlsaPluginPcm below asserts.
    const std::string error = rejected("pulse:my-sink");
    EXPECT_NE(error.find("PulseAudio backend"), std::string::npos);
    EXPECT_NE(error.find("not in this build"), std::string::npos);
    EXPECT_NE(error.find("SENDSPIN_CLI_WITH_PULSE"), std::string::npos);
    EXPECT_NE(error.find(audio_backend_list()), std::string::npos)
        << "the error should name the backends this build has";
#ifdef SENDSPIN_CLI_HAVE_ALSA
    // The flag alone would send someone off to rebuild for a path that already works on their
    // host: ALSA's plugin PCM reaches the same server, and this build can play through it.
    EXPECT_NE(error.find("-o alsa:pulse"), std::string::npos);
#endif
}

TEST(ResolveDeviceSpec, BarePulseIsStillTheAlsaPluginPcm) {
#ifdef SENDSPIN_CLI_HAVE_ALSA
    // Nothing shadows it on this build, and the reserved entry deliberately steps aside for a
    // *bare* name ALSA can still serve -- so -o pulse means exactly what it always meant, and a
    // build without the native backend takes nothing away from a working command line.
    EXPECT_EQ(resolved("pulse").backend, SinkBackend::Alsa);
    EXPECT_EQ(resolved("pulse").device, "pulse");
#else
    // No ALSA either, so there is no plugin PCM to step aside for. The reserved entry answers, and
    // names the flag that would turn the native backend on.
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

    // Everything after the FIRST colon is the node, so a name carrying its own colon survives.
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
    // Only the prefixed form lands here on a build that has ALSA: a bare `pipewire` is still an
    // ALSA PCM name there, which is what BarePipewireIsStillTheAlsaPluginPcm below asserts.
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
    // Nothing shadows it on this build, and the reserved entry deliberately steps aside for a
    // *bare* name ALSA can still serve -- so -o pipewire means exactly what it always meant, and a
    // build without the native backend takes nothing away from a working command line.
    EXPECT_EQ(resolved("pipewire").backend, SinkBackend::Alsa);
    EXPECT_EQ(resolved("pipewire").device, "pipewire");
#else
    // No ALSA either, so there is no plugin PCM to step aside for. The reserved entry answers, and
    // names the flag that would turn the native backend on.
    EXPECT_NE(rejected("pipewire").find("PipeWire backend"), std::string::npos);
#endif
}

#endif  // SENDSPIN_CLI_HAVE_PIPEWIRE

}  // namespace
}  // namespace sendspin_cli
