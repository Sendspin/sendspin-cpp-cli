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

/// `sendspin-cli tui`: the terminal view over TuiModel, attached to the running player's socket.

#pragma once

#include <string>

namespace sendspin_cli {

/// Runs the terminal UI until `q` or Ctrl-C. It only talks to the control socket at `path`.
/// @param absent_reason Why `path` is empty, when it is.
/// @return The exit status: 0 on quit; 1, after saying so, when stdin or stdout is not a terminal.
int run_tui(const std::string& path, const std::string& absent_reason);

}  // namespace sendspin_cli
