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

/// HookRunner's Windows half: CreateProcessW() of cmd.exe /c, reaped by polling the handle.

#include "hooks.h"

#include "log.h"
#include "win_security.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_HOOK;

namespace {

/// Prefix of every hook variable; inherited ones are cleared first.
constexpr const wchar_t* ENV_PREFIX = L"SENDSPIN_";

/// `text`, which is UTF-8, as UTF-16.
std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

/// Appends `name=value` to `env`, or nothing when the value is empty.
void add_env(std::vector<std::wstring>& env, const wchar_t* name, const std::string& value) {
    if (value.empty()) {
        return;
    }
    env.push_back(std::wstring(name) + L"=" + widen(value));
}

/// True if `entry` names its variable `prefix...`, compared as Windows does: without case.
bool has_prefix(const std::wstring& entry, const wchar_t* prefix) {
    const size_t length = wcslen(prefix);
    return entry.size() >= length && _wcsnicmp(entry.c_str(), prefix, length) == 0;
}

/// Our environment minus inherited SENDSPIN_* variables, plus the event's own, as the
/// double-NUL-terminated block CreateProcessW() takes, sorted by name as it expects.
std::wstring hook_environment(const char* event, const HookContext& context) {
    std::vector<std::wstring> env;
    if (wchar_t* block = GetEnvironmentStringsW(); block != nullptr) {
        for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
            // "=C:=C:\..." entries carry per-drive working directories; keep them.
            if (!has_prefix(entry, ENV_PREFIX)) {
                env.emplace_back(entry);
            }
        }
        FreeEnvironmentStringsW(block);
    }
    add_env(env, L"SENDSPIN_EVENT", event);
    add_env(env, L"SENDSPIN_SERVER_ID", context.server_id);
    add_env(env, L"SENDSPIN_SERVER_NAME", context.server_name);
    add_env(env, L"SENDSPIN_SERVER_URL", context.server_url);
    add_env(env, L"SENDSPIN_CLIENT_ID", context.client_id);
    add_env(env, L"SENDSPIN_CLIENT_NAME", context.client_name);

    std::sort(env.begin(), env.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::wstring block;
    for (const std::wstring& entry : env) {
        block += entry;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

/// The shell to run hooks with: %ComSpec%, else cmd.exe from the system directory.
std::wstring command_interpreter() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"ComSpec", path, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return path;
    }
    const UINT system = GetSystemDirectoryW(path, MAX_PATH);
    return (system > 0 && system < MAX_PATH ? std::wstring(path)
                                            : std::wstring(L"C:\\Windows\\System32")) +
           L"\\cmd.exe";
}

/// An inheritable copy of `handle`, or of NUL when it is not usable; the caller closes it.
HANDLE inheritable(HANDLE handle, HANDLE null_device) {
    HANDLE source = (handle == nullptr || handle == INVALID_HANDLE_VALUE) ? null_device : handle;
    HANDLE copy = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, TRUE,
                        DUPLICATE_SAME_ACCESS) == 0) {
        return nullptr;
    }
    return copy;
}

}  // namespace

bool HookRunner::reap(RunningHook& hook) {
    if (WaitForSingleObject(hook.process, 0) == WAIT_TIMEOUT) {
        return false;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(hook.process, &code) == 0) {
        cli_log(LogLevel::DEBUG, "The %s hook [%lu] could not be waited on (%s)",
                hook.event.c_str(), hook.pid, last_error_text().c_str());
    } else if (code != 0) {
        cli_log(LogLevel::WARN, "The %s hook [%lu] exited %lu", hook.event.c_str(), hook.pid, code);
    } else {
        cli_log(LogLevel::DEBUG, "The %s hook [%lu] finished", hook.event.c_str(), hook.pid);
    }
    CloseHandle(hook.process);
    return true;
}

void HookRunner::spawn(const std::string& command, const char* event, const HookContext& context) {
    std::wstring environment = hook_environment(event, context);
    // /s strips just the outer quotes, so the command keeps any of its own.
    std::wstring command_line =
        L"\"" + command_interpreter() + L"\" /d /s /c \"" + widen(command) + L"\"";

    // stdout may carry PCM, so point it at stderr; stdin reads nothing.
    SECURITY_ATTRIBUTES null_attributes = {sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    const HANDLE null_device =
        CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &null_attributes, OPEN_EXISTING, 0, nullptr);
    HANDLE stdio[3] = {
        inheritable(null_device, null_device),
        inheritable(GetStdHandle(STD_ERROR_HANDLE), null_device),
        inheritable(GetStdHandle(STD_ERROR_HANDLE), null_device),
    };

    // Only these three are inherited, so a hook cannot hold the player's sockets across a restart.
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<BYTE> attribute_buffer(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    bool ready = stdio[0] != nullptr && stdio[1] != nullptr && stdio[2] != nullptr &&
                 InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) != 0;
    if (ready) {
        ready = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, stdio,
                                          sizeof(stdio), nullptr, nullptr) != 0;
    }

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdio[0];
    startup.StartupInfo.hStdOutput = stdio[1];
    startup.StartupInfo.hStdError = stdio[2];
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process = {};
    // No window: a hook is not a console the user can see.
    const bool started =
        ready &&
        CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                       environment.data(), nullptr, &startup.StartupInfo, &process) != 0;
    const std::string reason = started ? std::string() : last_error_text();

    if (ready) {
        DeleteProcThreadAttributeList(attributes);
    }
    for (HANDLE handle : stdio) {
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }
    if (null_device != INVALID_HANDLE_VALUE) {
        CloseHandle(null_device);
    }

    if (!started) {
        cli_log(LogLevel::WARN, "Could not run the %s hook: %s", event, reason.c_str());
        return;
    }
    CloseHandle(process.hThread);
    // Never log the command: it may carry credentials.
    cli_log(LogLevel::DEBUG, "Running %s hook [%lu]", event, process.dwProcessId);
    this->running_.push_back({process.dwProcessId, event, process.hProcess});
}

}  // namespace sendspin_cli
