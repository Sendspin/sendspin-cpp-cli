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

/// How -o portaudio:<device> picks a device by name; built only with the PortAudio backend.

#ifdef SENDSPIN_CLI_HAVE_PORTAUDIO

#include "portaudio_device.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace sendspin_cli {
namespace {

/// One output device listed under three host APIs, as Windows lists every one.
const std::vector<PaDeviceEntry> WINDOWS{
    {4, "Speakers (7- Schiit Modi+)", "mme"},
    {5, "Odyssey G95NC (NVIDIA High Defi", "mme"},
    {12, "Speakers (7- Schiit Modi+)", "directsound"},
    {13, "Odyssey G95NC (NVIDIA High Definition Audio)", "directsound"},
    {19, "Speakers (7- Schiit Modi+)", "wasapi"},
    {16, "Odyssey G95NC (NVIDIA High Definition Audio)", "wasapi"},
};

/// Matches `device`, requiring success, and returns the index it picked.
PaDeviceIndex matched(const std::string& device, const std::vector<PaDeviceEntry>& entries) {
    PaDeviceIndex out = paNoDevice;
    std::string error;
    EXPECT_TRUE(match_pa_device_name(device, entries, PaDirection::Output, out, error))
        << device << ": " << error;
    return out;
}

/// Matches `device`, requiring failure, and returns the reason given.
std::string refused(const std::string& device, const std::vector<PaDeviceEntry>& entries) {
    PaDeviceIndex out = paNoDevice;
    std::string error;
    EXPECT_FALSE(match_pa_device_name(device, entries, PaDirection::Output, out, error))
        << "matched '" << device << "' to " << out;
    EXPECT_FALSE(error.empty()) << "no reason given for '" << device << "'";
    return error;
}

TEST(PaHostApiPrefix, NamesTheHostApisASpecCanSay) {
    EXPECT_STREQ(pa_host_api_prefix(paMME), "mme");
    EXPECT_STREQ(pa_host_api_prefix(paDirectSound), "directsound");
    EXPECT_STREQ(pa_host_api_prefix(paWASAPI), "wasapi");
    EXPECT_STREQ(pa_host_api_prefix(paWDMKS), "wdmks");
    EXPECT_STREQ(pa_host_api_prefix(paALSA), "alsa");
    EXPECT_STREQ(pa_host_api_prefix(paJACK), "jack");
    EXPECT_EQ(pa_host_api_prefix(paInDevelopment), nullptr);
}

TEST(MatchPaDeviceName, AUniqueNameResolvesWithoutAPrefix) {
    const std::vector<PaDeviceEntry> entries{{0, "Built-in Output", "coreaudio"},
                                             {1, "USB DAC", "coreaudio"}};
    EXPECT_EQ(matched("USB DAC", entries), 1);
    EXPECT_EQ(matched("usb dac", entries), 1);
    // MME's truncated copy is the only device with that exact name.
    EXPECT_EQ(matched("Odyssey G95NC (NVIDIA High Defi", WINDOWS), 5);
}

TEST(MatchPaDeviceName, ANameUnderSeveralHostApisIsRefusedWithThePrefixedForm) {
    const std::string error = refused("Speakers (7- Schiit Modi+)", WINDOWS);
    EXPECT_NE(error.find("3 output devices share that name (4 mme, 12 directsound, 19 wasapi)"),
              std::string::npos)
        << error;
    EXPECT_NE(error.find("-o \"portaudio:mme:Speakers (7- Schiit Modi+)\""), std::string::npos)
        << error;
}

TEST(MatchPaDeviceName, AHostApiPrefixPicksThatOne) {
    EXPECT_EQ(matched("mme:Speakers (7- Schiit Modi+)", WINDOWS), 4);
    EXPECT_EQ(matched("directsound:Speakers (7- Schiit Modi+)", WINDOWS), 12);
    EXPECT_EQ(matched("wasapi:Speakers (7- Schiit Modi+)", WINDOWS), 19);
    EXPECT_EQ(matched("DirectSound:odyssey g95nc (nvidia high definition audio)", WINDOWS), 13);
}

TEST(MatchPaDeviceName, AWholeNameWinsOverReadingItAsAPrefix) {
    const std::vector<PaDeviceEntry> entries{{0, "wasapi:Den", "mme"}, {1, "Den", "wasapi"}};
    EXPECT_EQ(matched("wasapi:Den", entries), 0);
}

TEST(MatchPaDeviceName, ANameWithAColonResolvesBareOrPrefixed) {
    const std::vector<PaDeviceEntry> entries{{0, "HDA Intel PCH: ALC892 Analog (hw:0,0)", "alsa"},
                                             {1, "pulse", "alsa"}};
    EXPECT_EQ(matched("HDA Intel PCH: ALC892 Analog (hw:0,0)", entries), 0);
    EXPECT_EQ(matched("alsa:HDA Intel PCH: ALC892 Analog (hw:0,0)", entries), 0);
}

TEST(MatchPaDeviceName, AnUnknownOrAbsentPrefixIsJustANameThatMatchesNothing) {
    EXPECT_NE(refused("kitchen:Speakers (7- Schiit Modi+)", WINDOWS)
                  .find("no output device by that name"),
              std::string::npos);
    EXPECT_NE(
        refused("asio:Speakers (7- Schiit Modi+)", WINDOWS).find("no output device by that name"),
        std::string::npos);
}

TEST(MatchPaDeviceName, TwinsUnderOneHostApiAreNamedByIndex) {
    const std::vector<PaDeviceEntry> entries{{7, "USB Audio", "wasapi"},
                                             {9, "USB Audio", "wasapi"}};
    for (const char* device : {"USB Audio", "wasapi:USB Audio"}) {
        const std::string error = refused(device, entries);
        EXPECT_NE(error.find("(7 wasapi, 9 wasapi)"), std::string::npos) << error;
        EXPECT_NE(error.find("by index instead"), std::string::npos) << error;
        EXPECT_EQ(error.find("in front"), std::string::npos) << error;
    }
}

TEST(MatchPaDeviceName, AnUnnameableHostApiFallsBackToTheIndex) {
    const std::vector<PaDeviceEntry> entries{{2, "Line Out", nullptr}, {3, "Line Out", "mme"}};
    const std::string error = refused("Line Out", entries);
    EXPECT_NE(error.find("(2, 3 mme)"), std::string::npos) << error;
    EXPECT_NE(error.find("by index instead"), std::string::npos) << error;
}

}  // namespace
}  // namespace sendspin_cli

#endif  // SENDSPIN_CLI_HAVE_PORTAUDIO
