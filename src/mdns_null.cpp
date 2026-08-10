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

/// @file mdns_null.cpp
/// @brief MdnsService where `dns_sd.h` was not found at configure time
///
/// Built instead of mdns_dnssd.cpp, not alongside it. The daemon still starts and still
/// plays: it just has to be told where its server is, which is what mdns_available()
/// reporting false lets main.cpp and the parser say plainly rather than failing obscurely
/// once nothing is ever discovered.

#include "mdns.h"

#include <memory>
#include <string>
#include <vector>

namespace sendspin_cli {

/// Nothing to hold: the header only declares this so both builds share one class shape.
struct MdnsService::Impl {};

MdnsService::MdnsService() : impl_(std::make_unique<Impl>()) {}

MdnsService::~MdnsService() = default;

bool MdnsService::advertise(const std::string& /*instance*/, uint16_t /*port*/,
                            const std::string& /*path*/, const std::string& /*friendly_name*/,
                            std::string& error) {
    error = "this build has no mDNS support";
    return false;
}

bool MdnsService::browse(std::string& error) {
    error = "this build has no mDNS support";
    return false;
}

void MdnsService::poll(int64_t /*now_ms*/) {}

std::vector<DiscoveredServer> MdnsService::servers() const {
    return {};
}

void MdnsService::stop() {}

bool mdns_available() {
    return false;
}

std::string mdns_backend_name() {
    return "none";
}

}  // namespace sendspin_cli
