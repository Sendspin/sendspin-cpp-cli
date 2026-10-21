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

/// The POSIX file calls the tests' fixtures make, on Windows too.

#pragma once

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>

/// Windows directories take their ACL from the parent, not a mode.
inline int mkdir(const char* path, unsigned /*mode*/) {
    return ::_mkdir(path);
}

#define STDERR_FILENO 2
#define F_OK 0

/// For fixtures that take a mode; file_access.h turns one into an ACL.
using mode_t = unsigned;
#else
#include <sys/stat.h>
#include <unistd.h>
// Only Windows translates line endings.
#define O_BINARY 0
#endif
