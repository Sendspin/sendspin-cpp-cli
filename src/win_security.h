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

/// Windows stand-ins for POSIX ownership and modes: per-user folders and owner-only ACLs.

#pragma once

#include <string>

namespace sendspin_cli {

/// This user's LocalAppData folder with '/' separators, or empty when it cannot be found.
std::string local_app_data_dir();

/// This user's roaming AppData folder, the counterpart of ~/.config; as local_app_data_dir().
std::string roaming_app_data_dir();

/// True if `path` is a directory owned by this user that no one but SYSTEM and Administrators
/// can also write to, or create files in that others could write.
bool is_private_directory(const std::string& path, std::string& reason);

/// Opens `path` read-write as a CRT descriptor, creating it if missing, like POSIX open().
/// @param exclusive Fail if it exists, like O_EXCL.
/// @param owner_only A file this creates grants only this user access, like mode 0600.
/// @return -1 with `error` set on failure.
int open_file(const std::string& path, bool exclusive, bool owner_only, std::string& error);

/// The text for GetLastError()'s current value.
std::string last_error_text();

}  // namespace sendspin_cli
