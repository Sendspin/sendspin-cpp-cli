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

/// The library's byte store (identity key, pairings, output delay), backed by the state store.

#pragma once

#include "state_store.h"

#include <sendspin/client.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sendspin_cli {

class CliPersistenceProvider final : public sendspin::SendspinPersistenceProvider {
public:
    /// `store` must outlive this provider, which must in turn outlive the client.
    explicit CliPersistenceProvider(StateStore& store) : store_(store) {}

    std::optional<size_t> read_blob(std::string_view key, std::span<uint8_t> out) override;
    bool write_blob(std::string_view key, std::span<const uint8_t> data) override;

private:
    StateStore& store_;
};

}  // namespace sendspin_cli
