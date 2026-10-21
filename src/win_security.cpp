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

#include "win_security.h"

// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>
// clang-format on

#include <memory>
#include <vector>

namespace sendspin_cli {

namespace {

/// Rights that let a holder change what is in a directory, or a file created in it.
constexpr ACCESS_MASK WRITE_RIGHTS = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                                     FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE |
                                     WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

struct LocalFreeDeleter {
    void operator()(void* memory) const {
        LocalFree(memory);
    }
};

/// The process user's SID, or empty when the token cannot be read.
std::vector<BYTE> current_user_sid() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    std::vector<BYTE> sid;
    if (size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0) {
        const PSID user = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
        const DWORD length = GetLengthSid(user);
        sid.resize(length);
        CopySid(length, sid.data(), user);
    }
    CloseHandle(token);
    return sid;
}

bool is_well_known(PSID sid, WELL_KNOWN_SID_TYPE type) {
    return IsWellKnownSid(sid, type) != 0;
}

/// `DOMAIN\name` for a SID, or its S-1-... form when it has no name.
std::string account_name(PSID sid) {
    char name[256];
    char domain[256];
    DWORD name_size = sizeof(name);
    DWORD domain_size = sizeof(domain);
    SID_NAME_USE use;
    if (LookupAccountSidA(nullptr, sid, name, &name_size, domain, &domain_size, &use) != 0) {
        return domain_size > 0 ? std::string(domain) + "\\" + name : std::string(name);
    }
    char* text = nullptr;
    if (ConvertSidToStringSidA(sid, &text) == 0) {
        return "an unknown account";
    }
    std::string result(text);
    LocalFree(text);
    return result;
}

}  // namespace

std::string last_error_text() {
    const DWORD code = GetLastError();
    char text[256] = {};
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, text, sizeof(text), nullptr);
    std::string message(text, length);
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message.empty() ? "error " + std::to_string(code) : message;
}

namespace {

/// A known folder with '/' separators, or empty when it cannot be found.
std::string known_folder(REFKNOWNFOLDERID id) {
    // The known folder, not its environment variable, which could redirect it.
    PWSTR wide = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &wide))) {
        return {};
    }
    // The ANSI code page, which every narrow Win32 call here reads paths in.
    const int size = WideCharToMultiByte(CP_ACP, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string path;
    if (size > 1) {
        path.resize(static_cast<size_t>(size));
        WideCharToMultiByte(CP_ACP, 0, wide, -1, path.data(), size, nullptr, nullptr);
        path.pop_back();
    }
    CoTaskMemFree(wide);
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    return path;
}

}  // namespace

std::string local_app_data_dir() {
    return known_folder(FOLDERID_LocalAppData);
}

std::string roaming_app_data_dir() {
    return known_folder(FOLDERID_RoamingAppData);
}

bool is_private_directory(const std::string& path, std::string& reason) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        reason = "cannot read " + path + ": " + last_error_text();
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        reason = path + " is not a directory";
        return false;
    }

    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetNamedSecurityInfoA(
        path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &descriptor);
    if (status != ERROR_SUCCESS) {
        SetLastError(status);
        reason = "cannot read the ACL of " + path + ": " + last_error_text();
        return false;
    }
    const std::unique_ptr<void, LocalFreeDeleter> held(descriptor);

    std::vector<BYTE> user = current_user_sid();
    if (user.empty()) {
        reason = "cannot tell which user this is: " + last_error_text();
        return false;
    }
    if (EqualSid(owner, user.data()) == 0) {
        reason = path + " is owned by " + account_name(owner) + ", not by this user";
        return false;
    }
    if (dacl == nullptr) {
        reason = path + " has no ACL, which grants everyone full access";
        return false;
    }

    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* entry = nullptr;
        if (GetAce(dacl, index, &entry) == 0) {
            reason = "cannot read the ACL of " + path + ": " + last_error_text();
            return false;
        }
        const auto* header = static_cast<ACE_HEADER*>(entry);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
        }
        const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if ((allowed->Mask & WRITE_RIGHTS) == 0) {
            continue;
        }
        PSID trustee = const_cast<DWORD*>(&allowed->SidStart);
        // CREATOR OWNER stands for whoever creates the file, which is this user.
        if (EqualSid(trustee, user.data()) != 0 || is_well_known(trustee, WinLocalSystemSid) ||
            is_well_known(trustee, WinBuiltinAdministratorsSid) ||
            is_well_known(trustee, WinCreatorOwnerSid)) {
            continue;
        }
        reason = path + " grants " + account_name(trustee) + " write access";
        return false;
    }
    return true;
}

int open_file(const std::string& path, bool exclusive, bool owner_only, std::string& error) {
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    std::unique_ptr<void, LocalFreeDeleter> descriptor;
    if (owner_only) {
        std::vector<BYTE> user = current_user_sid();
        char* sid = nullptr;
        if (user.empty() || ConvertSidToStringSidA(user.data(), &sid) == 0) {
            error = "cannot tell which user this is: " + last_error_text();
            return -1;
        }
        // Protected, so nothing inherited from the directory widens it.
        const std::string sddl = std::string("D:P(A;;FA;;;") + sid + ")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR built = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(), SDDL_REVISION_1,
                                                                 &built, nullptr) == 0) {
            error = "cannot build an owner-only ACL: " + last_error_text();
            return -1;
        }
        descriptor.reset(built);
        attributes.lpSecurityDescriptor = built;
    }

    const HANDLE handle =
        CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &attributes,
                    exclusive ? CREATE_NEW : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = last_error_text();
        return -1;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
    if (fd < 0) {
        error = "cannot wrap the file handle";
        CloseHandle(handle);
        return -1;
    }
    return fd;
}

}  // namespace sendspin_cli
