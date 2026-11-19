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

#include "persistence_provider.h"

#include <algorithm>
#include <string>
#include <vector>

namespace sendspin_cli {

std::optional<size_t> CliPersistenceProvider::read_blob(std::string_view key,
                                                        std::span<uint8_t> out) {
    const std::optional<std::vector<uint8_t>> stored = this->store_.blob(std::string(key));
    if (!stored.has_value() || stored->size() > out.size()) {
        return std::nullopt;
    }
    std::copy(stored->begin(), stored->end(), out.begin());
    return stored->size();
}

bool CliPersistenceProvider::write_blob(std::string_view key, std::span<const uint8_t> data) {
    return this->store_.set_blob(std::string(key), data.data(), data.size());
}

}  // namespace sendspin_cli
