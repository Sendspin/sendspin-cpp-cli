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

/// The socket calls the control channel makes, over POSIX sockets or Winsock's AF_UNIX.

#pragma once

#ifdef _WIN32
// winsock2.h must precede afunix.h.
// clang-format off
#include <winsock2.h>
#include <afunix.h>
// clang-format on
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <cstddef>
#include <cstring>
#include <string>

namespace sendspin_cli {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t INVALID_SOCKET_HANDLE = -1;
#endif

/// Starts Winsock once per process. Reference-counted, so it coexists with the library's own.
inline bool socket_startup() {
#ifdef _WIN32
    static const bool STARTED = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return STARTED;
#else
    return true;
#endif
}

inline void close_socket(socket_t socket) {
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

/// @return false with socket_error() set when the mode could not be changed.
inline bool set_nonblocking(socket_t socket) {
#ifdef _WIN32
    u_long enabled = 1;
    return ::ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

/// Bytes read, 0 at end of stream, negative on error.
inline long receive_bytes(socket_t socket, char* buffer, size_t length) {
#ifdef _WIN32
    return ::recv(socket, buffer, static_cast<int>(length), 0);
#else
    return static_cast<long>(::read(socket, buffer, length));
#endif
}

/// Bytes written, negative on error.
inline long send_bytes(socket_t socket, const char* data, size_t length) {
#ifdef _WIN32
    return ::send(socket, data, static_cast<int>(length), 0);
#else
    return static_cast<long>(::write(socket, data, length));
#endif
}

/// The last socket call's error code.
inline int socket_error() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline bool socket_interrupted(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

inline bool socket_would_block(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

inline std::string socket_error_text(int error) {
#ifdef _WIN32
    char text[256] = {};
    const DWORD length =
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       static_cast<DWORD>(error), 0, text, sizeof(text), nullptr);
    std::string message(text, length);
    // FormatMessage ends its text with ".\r\n".
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message.empty() ? "error " + std::to_string(error) : message;
#else
    return std::strerror(error);
#endif
}

/// Why connect() failed, as the subcommand explains it.
enum class ConnectFailure {
    Missing,  ///< no socket file at the path
    Refused,  ///< a file, but nothing accepting
    Denied,   ///< not this user's to connect to
    Other,
};

inline ConnectFailure classify_connect_error(int error) {
#ifdef _WIN32
    // Windows reports a missing AF_UNIX socket file as WSAECONNREFUSED too; see control_client.
    switch (error) {
        case WSAECONNREFUSED:
            return ConnectFailure::Refused;
        case WSAEACCES:
            return ConnectFailure::Denied;
        default:
            return ConnectFailure::Other;
    }
#else
    switch (error) {
        case ENOENT:
            return ConnectFailure::Missing;
        case ECONNREFUSED:
            return ConnectFailure::Refused;
        case EACCES:
        case EPERM:
            return ConnectFailure::Denied;
        default:
            return ConnectFailure::Other;
    }
#endif
}

}  // namespace sendspin_cli
