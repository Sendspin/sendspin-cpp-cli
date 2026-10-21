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

/// Who may write a directory, and whether a file is private: modes on POSIX, ACLs on Windows.

#pragma once

#include <string>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on

#include <vector>
#else
#include <sys/stat.h>
#endif

namespace sendspin_cli {

#ifdef _WIN32
/// This user's SID as S-1-..., or empty.
inline std::string current_user_sid() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    std::string text;
    char* sid = nullptr;
    if (size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0 &&
        ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid) != 0) {
        text = sid;
        LocalFree(sid);
    }
    CloseHandle(token);
    return text;
}

/// Replaces `path`'s DACL with the one `sddl` describes.
inline bool set_dacl(const std::string& path, const std::string& sddl) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1,
                                                             &descriptor, nullptr) == 0) {
        return false;
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    const bool ok =
        GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) != 0 &&
        SetNamedSecurityInfoA(const_cast<char*>(path.c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    LocalFree(descriptor);
    return ok;
}
#endif

/// Lets this user create files in `path`, or stops them: the permissions case.
inline bool set_writable(const std::string& path, bool writable) {
#ifdef _WIN32
    const std::string sid = current_user_sid();
    // Denying add-file, add-subdirectory and delete-child is what 0500 does to a directory.
    return !sid.empty() &&
           set_dacl(path, writable ? "D:P(A;OICI;FA;;;" + sid + ")"
                                   : "D:P(D;;0x46;;;" + sid + ")(A;OICI;FA;;;" + sid + ")");
#else
    return ::chmod(path.c_str(), writable ? 0700 : 0500) == 0;
#endif
}

#ifdef _WIN32
/// True if `path`'s ACL grants this user, and nobody else, any access: its 0600.
inline bool owner_only(const std::string& path, std::string& detail) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoA(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
        detail = "cannot read the ACL";
        return false;
    }
    char* text = nullptr;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorA(
            descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr) != 0) {
        detail = text;
        LocalFree(text);
    }
    LocalFree(descriptor);

    // Protected, and one allow ACE for this user, spelled the way Windows prints it back: a
    // well-known account comes back as its alias (LA for the built-in Administrator).
    std::string expected = "D:P(A;;FA;;;" + current_user_sid() + ")";
    PSECURITY_DESCRIPTOR built = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(expected.c_str(), SDDL_REVISION_1,
                                                             &built, nullptr) != 0) {
        char* canonical = nullptr;
        if (ConvertSecurityDescriptorToStringSecurityDescriptorA(
                built, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &canonical, nullptr) != 0) {
            expected = canonical;
            LocalFree(canonical);
        }
        LocalFree(built);
    }
    return detail == expected;
}
#endif

}  // namespace sendspin_cli
