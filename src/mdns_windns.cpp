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

/// MdnsService over Windows' own DNS-SD API (windns.h, Windows 10 1809+).
/// Its callbacks run on thread-pool threads, so they only queue results for poll() to apply.

#include "mdns.h"

#include "log.h"
#include "outbound.h"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
// clang-format on

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

/// Advertising lines; discovery lines log under LOG_TAG_DISCOVERY.
static constexpr const char* LOG_TAG = LOG_TAG_MDNS;

namespace {

/// Wait after a candidate's first address, as mdns_dnssd.cpp does.
constexpr int64_t ADDRESS_SETTLE_MS = 250;

/// How long stop() waits for the deregistration to send its goodbye.
constexpr DWORD DEREGISTER_WAIT_MS = 500;

constexpr const char* MDNS_DOMAIN_SUFFIX = ".local";

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

std::string narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.pop_back();
    return out;
}

std::string describe_status(DWORD status) {
    char text[256] = {};
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, status, 0, text, sizeof(text), nullptr);
    std::string message(text, length);
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message.empty() ? "DNS status " + std::to_string(status) : message;
}

/// `full` without its `.<service>.local` suffix, or empty when it is not one of `service`'s.
std::string instance_label(const std::string& full, const std::string& service) {
    const std::string suffix = "." + service + MDNS_DOMAIN_SUFFIX;
    if (full.size() <= suffix.size() ||
        full.compare(full.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return {};
    }
    return full.substr(0, full.size() - suffix.size());
}

// The queue callbacks post to. Process-lifetime, so a callback that fires after its
// MdnsService is gone, or after a cancel, still has somewhere valid to write.

enum class EventKind { Registered, Deregistered, Browsed, Resolved };

struct Event {
    EventKind kind{EventKind::Browsed};
    uint64_t op{0};  ///< which operation it answers; stale ones are dropped
    DWORD status{0};
    std::string instance;                               ///< registered or resolved instance label
    std::vector<std::pair<std::string, bool>> browsed;  ///< full name, still present
    std::string host;
    uint16_t port{0};
    std::string path;
    std::string name;
    std::vector<std::string> addresses;
};

struct Mailbox {
    std::mutex mutex;
    std::deque<Event> events;
    HANDLE deregistered{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};

/// Never destroyed: callbacks may outlive every MdnsService.
Mailbox& mailbox() {
    static auto* const instance = new Mailbox();
    return *instance;
}

void post(Event event) {
    Mailbox& box = mailbox();
    if (event.kind == EventKind::Deregistered) {
        SetEvent(box.deregistered);
    }
    const std::lock_guard<std::mutex> lock(box.mutex);
    box.events.push_back(std::move(event));
}

uint64_t op_of(void* context) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(context));
}

void* context_of(uint64_t op) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(op));
}

/// The instance's TXT value for `key`, or empty.
std::string property(const DNS_SERVICE_INSTANCE& instance, const wchar_t* key) {
    for (DWORD i = 0; i < instance.dwPropertyCount; ++i) {
        if (instance.keys[i] != nullptr && _wcsicmp(instance.keys[i], key) == 0) {
            return narrow(instance.values[i]);
        }
    }
    return {};
}

VOID WINAPI register_complete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    Event event;
    event.kind = EventKind::Registered;
    event.op = op_of(context);
    event.status = status;
    if (instance != nullptr) {
        event.instance = narrow(instance->pszInstanceName);
        DnsServiceFreeInstance(instance);
    }
    post(std::move(event));
}

VOID WINAPI deregister_complete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    Event event;
    event.kind = EventKind::Deregistered;
    event.op = op_of(context);
    event.status = status;
    if (instance != nullptr) {
        DnsServiceFreeInstance(instance);
    }
    post(std::move(event));
}

VOID WINAPI browse_complete(DWORD status, PVOID context, PDNS_RECORD records) {
    Event event;
    event.kind = EventKind::Browsed;
    event.op = op_of(context);
    event.status = status;
    // The DNS-SD API is Unicode-only, whatever PDNS_RECORD names in this build.
    for (const auto* record = reinterpret_cast<const DNS_RECORDW*>(records); record != nullptr;
         record = record->pNext) {
        if (record->wType == DNS_TYPE_PTR && record->Data.PTR.pNameHost != nullptr) {
            // A zero TTL is the record's goodbye.
            event.browsed.emplace_back(narrow(record->Data.PTR.pNameHost), record->dwTtl != 0);
        }
    }
    if (records != nullptr) {
        DnsRecordListFree(records, DnsFreeRecordList);
    }
    post(std::move(event));
}

VOID WINAPI resolve_complete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    Event event;
    event.kind = EventKind::Resolved;
    event.op = op_of(context);
    event.status = status;
    if (instance != nullptr) {
        event.host = narrow(instance->pszHostName);
        event.port = instance->wPort;
        event.path = property(*instance, L"path");
        event.name = property(*instance, L"name");
        char text[INET6_ADDRSTRLEN] = {};
        if (instance->ip4Address != nullptr &&
            inet_ntop(AF_INET, instance->ip4Address, text, sizeof(text)) != nullptr) {
            event.addresses.emplace_back(text);
        }
        if (instance->ip6Address != nullptr &&
            inet_ntop(AF_INET6, instance->ip6Address, text, sizeof(text)) != nullptr) {
            event.addresses.emplace_back(text);
        }
        DnsServiceFreeInstance(instance);
    }
    post(std::move(event));
}

/// This machine's mDNS host name, `<dns hostname>.local`.
std::wstring local_host_name() {
    wchar_t name[256];
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNameDnsHostname, name, &size) == 0 || size == 0) {
        return L"localhost.local";
    }
    return std::wstring(name) + widen(MDNS_DOMAIN_SUFFIX);
}

}  // namespace

/// All windns state, kept out of the header for the null build.
struct MdnsService::Impl {
    /// A `_sendspin-server._tcp` instance being resolved, or already resolved.
    struct Candidate {
        std::string instance;
        std::string name;
        std::string path;
        uint16_t port{0};
        std::vector<std::string> addresses;

        uint64_t resolve_op{0};  ///< non-zero while its resolve is outstanding
        DNS_SERVICE_CANCEL resolve_cancel{};
        std::wstring resolve_name;  ///< kept alive for the request that points at it
        DNS_SERVICE_RESOLVE_REQUEST resolve_request{};

        int64_t first_address_ms{-1};
        uint64_t resolved_seq{0};  ///< 0 until ready; otherwise the order it resolved in
        bool announced{false};     ///< whether the "found" line has already been logged
    };

    // What was asked for, kept to retry.
    bool advertise_wanted{false};
    std::string advertise_instance;
    std::string advertise_path;
    std::string advertise_friendly_name;
    uint16_t advertise_port{0};
    bool browse_wanted{false};

    // Live windns state.
    uint64_t register_op{0};
    PDNS_SERVICE_INSTANCE register_instance{nullptr};
    DNS_SERVICE_REGISTER_REQUEST register_request{};
    uint64_t browse_op{0};
    DNS_SERVICE_CANCEL browse_cancel{};
    std::wstring browse_name;
    DNS_SERVICE_BROWSE_REQUEST browse_request{};
    std::map<std::string, std::unique_ptr<Candidate>> candidates;

    uint64_t next_op{1};
    uint64_t next_seq{1};

    // Recovery.
    int64_t restart_at_ms{-1};
    uint32_t restart_attempt{0};

    ~Impl() {
        this->teardown();
    }

    bool start_register(std::string& error) {
        const std::string instance = truncate_utf8(this->advertise_instance, MDNS_MAX_LABEL_BYTES);
        const std::string path = truncate_utf8(this->advertise_path, MDNS_MAX_TXT_VALUE_BYTES);
        const std::string friendly =
            truncate_utf8(this->advertise_friendly_name, MDNS_MAX_TXT_VALUE_BYTES);
        if (instance.size() < this->advertise_instance.size()) {
            cli_log(LogLevel::WARN,
                    "instance name is longer than the %zu-byte DNS label limit -- "
                    "advertising as \"%s\"",
                    MDNS_MAX_LABEL_BYTES, instance.c_str());
        }

        const std::wstring service =
            widen(instance + "." + MDNS_CLIENT_SERVICE + MDNS_DOMAIN_SUFFIX);
        const std::wstring host = local_host_name();
        const std::wstring path_value = widen(path);
        const std::wstring name_value = widen(friendly);
        PCWSTR keys[] = {L"path", L"name"};
        PCWSTR values[] = {path_value.c_str(), name_value.c_str()};
        const DWORD properties = friendly.empty() ? 1 : 2;

        this->register_instance =
            DnsServiceConstructInstance(service.c_str(), host.c_str(), nullptr, nullptr,
                                        this->advertise_port, 0, 0, properties, keys, values);
        if (this->register_instance == nullptr) {
            error = "could not register " + std::string(MDNS_CLIENT_SERVICE) +
                    ": the service instance could not be built";
            return false;
        }

        const uint64_t op = this->next_op++;
        this->register_request = {};
        this->register_request.Version = DNS_QUERY_REQUEST_VERSION1;
        this->register_request.InterfaceIndex = 0;
        this->register_request.pServiceInstance = this->register_instance;
        this->register_request.pRegisterCompletionCallback = register_complete;
        this->register_request.pQueryContext = context_of(op);
        const DWORD status = DnsServiceRegister(&this->register_request, nullptr);
        if (status != DNS_REQUEST_PENDING) {
            DnsServiceFreeInstance(this->register_instance);
            this->register_instance = nullptr;
            error = "could not register " + std::string(MDNS_CLIENT_SERVICE) + ": " +
                    describe_status(status);
            return false;
        }
        this->register_op = op;
        return true;
    }

    bool start_browse(std::string& error) {
        this->browse_name = widen(std::string(MDNS_SERVER_SERVICE) + MDNS_DOMAIN_SUFFIX);
        const uint64_t op = this->next_op++;
        // Kept in Impl, not on the stack: it must outlive the call.
        DNS_SERVICE_BROWSE_REQUEST& request = this->browse_request;
        request = {};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex = 0;
        request.QueryName = this->browse_name.c_str();
        request.pBrowseCallback = browse_complete;
        request.pQueryContext = context_of(op);
        this->browse_cancel = {};
        const DNS_STATUS status = DnsServiceBrowse(&request, &this->browse_cancel);
        if (status != DNS_REQUEST_PENDING) {
            error = "could not browse " + std::string(MDNS_SERVER_SERVICE) + ": " +
                    describe_status(status);
            return false;
        }
        this->browse_op = op;
        // A browse has no confirming callback, so reset the backoff here.
        this->restart_attempt = 0;
        return true;
    }

    void start_resolve(Candidate& candidate) {
        candidate.resolve_name =
            widen(candidate.instance + "." + MDNS_SERVER_SERVICE + MDNS_DOMAIN_SUFFIX);
        const uint64_t op = this->next_op++;
        DNS_SERVICE_RESOLVE_REQUEST& request = candidate.resolve_request;
        request = {};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex = 0;
        request.QueryName = candidate.resolve_name.data();
        request.pResolveCompletionCallback = resolve_complete;
        request.pQueryContext = context_of(op);
        candidate.resolve_cancel = {};
        const DNS_STATUS status = DnsServiceResolve(&request, &candidate.resolve_cancel);
        if (status != DNS_REQUEST_PENDING) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "could not resolve \"%s\" (%s)",
                     candidate.instance.c_str(), describe_status(status).c_str());
            return;
        }
        candidate.resolve_op = op;
    }

    void cancel_resolve(Candidate& candidate) {
        if (candidate.resolve_op != 0) {
            DnsServiceResolveCancel(&candidate.resolve_cancel);
            candidate.resolve_op = 0;
        }
    }

    void teardown() {
        for (auto& entry : this->candidates) {
            this->cancel_resolve(*entry.second);
        }
        this->candidates.clear();

        if (this->browse_op != 0) {
            DnsServiceBrowseCancel(&this->browse_cancel);
            this->browse_op = 0;
        }

        if (this->register_instance != nullptr) {
            // Deregistering sends the goodbye; give it a moment before the request is freed.
            const uint64_t op = this->next_op++;
            this->register_request.pRegisterCompletionCallback = deregister_complete;
            this->register_request.pQueryContext = context_of(op);
            ResetEvent(mailbox().deregistered);
            if (DnsServiceDeRegister(&this->register_request, nullptr) == DNS_REQUEST_PENDING) {
                WaitForSingleObject(mailbox().deregistered, DEREGISTER_WAIT_MS);
            }
            DnsServiceFreeInstance(this->register_instance);
            this->register_instance = nullptr;
        }
        this->register_op = 0;
    }

    void schedule_restart(int64_t now_ms) {
        this->restart_at_ms =
            now_ms + static_cast<int64_t>(next_retry_delay_ms(this->restart_attempt));
        if (next_retry_delay_ms(this->restart_attempt) < MAX_RETRY_DELAY_MS) {
            ++this->restart_attempt;
        }
    }

    /// Starts only what is wanted and not running.
    void restart() {
        std::string error;
        if (this->advertise_wanted && this->register_op == 0 && !this->start_register(error)) {
            cli_log(LogLevel::WARN, "%s", error.c_str());
        }
        if (this->browse_wanted && this->browse_op == 0 && !this->start_browse(error)) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "%s", error.c_str());
        }
    }

    void poll_once(int64_t now_ms) {
        // Anything wanted but not running is owed a retry.
        const bool missing = (this->advertise_wanted && this->register_op == 0) ||
                             (this->browse_wanted && this->browse_op == 0);
        if (missing && this->restart_at_ms < 0) {
            this->schedule_restart(now_ms);
        }
        if (this->restart_at_ms >= 0 && now_ms >= this->restart_at_ms) {
            this->restart_at_ms = -1;
            this->restart();
        }

        std::deque<Event> events;
        {
            Mailbox& box = mailbox();
            const std::lock_guard<std::mutex> lock(box.mutex);
            events.swap(box.events);
        }
        for (Event& event : events) {
            this->apply(event, now_ms);
        }
        this->settle_addresses(now_ms);
    }

    void apply(Event& event, int64_t now_ms) {
        switch (event.kind) {
            case EventKind::Registered:
                this->on_registered(event);
                break;
            case EventKind::Deregistered:
                break;
            case EventKind::Browsed:
                this->on_browsed(event);
                break;
            case EventKind::Resolved:
                this->on_resolved(event, now_ms);
                break;
        }
    }

    void on_registered(const Event& event) {
        if (event.op != this->register_op) {
            return;
        }
        if (event.status != ERROR_SUCCESS) {
            // Dropped, so poll() retries it on the backoff.
            cli_log(LogLevel::ERROR, "registration was refused (%s)",
                    describe_status(event.status).c_str());
            DnsServiceFreeInstance(this->register_instance);
            this->register_instance = nullptr;
            this->register_op = 0;
            return;
        }
        const std::string label = instance_label(event.instance, MDNS_CLIENT_SERVICE);
        cli_log(LogLevel::INFO, "advertising %s as \"%s\" on port %u (path %s)",
                MDNS_CLIENT_SERVICE, label.empty() ? event.instance.c_str() : label.c_str(),
                this->advertise_port, this->advertise_path.c_str());
        this->restart_attempt = 0;
    }

    void on_browsed(const Event& event) {
        if (event.op != this->browse_op) {
            return;
        }
        if (event.status != ERROR_SUCCESS) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "browse reported an error (%s)",
                     describe_status(event.status).c_str());
            return;
        }
        for (const auto& [full, present] : event.browsed) {
            const std::string instance = instance_label(full, MDNS_SERVER_SERVICE);
            if (instance.empty()) {
                continue;
            }
            log_line(LogLevel::DEBUG, LOG_TAG_DISCOVERY, "browse %s \"%s\"",
                     present ? "found" : "lost", instance.c_str());
            if (!present) {
                this->forget(instance);
                continue;
            }
            if (this->candidates.count(instance) != 0) {
                continue;
            }
            auto candidate = std::make_unique<Candidate>();
            candidate->instance = instance;
            this->start_resolve(*candidate);
            if (candidate->resolve_op != 0) {
                this->candidates.emplace(instance, std::move(candidate));
            }
        }
    }

    void on_resolved(const Event& event, int64_t now_ms) {
        Candidate* candidate = nullptr;
        for (auto& entry : this->candidates) {
            if (entry.second->resolve_op == event.op && event.op != 0) {
                candidate = entry.second.get();
                break;
            }
        }
        if (candidate == nullptr) {
            return;
        }
        candidate->resolve_op = 0;

        if (event.status != ERROR_SUCCESS) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "resolving \"%s\" failed (%s)",
                     candidate->instance.c_str(), describe_status(event.status).c_str());
            // Dropped, so a later browse result can add it again.
            this->candidates.erase(candidate->instance);
            return;
        }

        log_line(LogLevel::DEBUG, LOG_TAG_DISCOVERY, "resolved \"%s\" to %s:%u",
                 candidate->instance.c_str(), event.host.c_str(), event.port);
        candidate->port = event.port;
        candidate->path = event.path;
        candidate->name = event.name;
        candidate->addresses = event.addresses;
        if (candidate->addresses.empty()) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY,
                     "no address came back for \"%s\" (%s) -- dropping",
                     candidate->instance.c_str(), event.host.c_str());
            this->candidates.erase(candidate->instance);
            return;
        }
        candidate->first_address_ms = now_ms;
    }

    /// Marks candidates dialable once their addresses have had time to all arrive.
    void settle_addresses(int64_t now_ms) {
        for (auto& entry : this->candidates) {
            Candidate& candidate = *entry.second;
            if (candidate.resolved_seq != 0 || candidate.first_address_ms < 0 ||
                now_ms - candidate.first_address_ms < ADDRESS_SETTLE_MS) {
                continue;
            }
            std::string url;
            std::string error;
            DiscoveredServer server;
            this->fill(candidate, server);
            if (!discovered_server_url(server, url, error)) {
                if (!candidate.announced) {
                    candidate.announced = true;
                    log_line(LogLevel::DEBUG, LOG_TAG_DISCOVERY, "skipping server \"%s\" -- %s",
                             candidate.instance.c_str(), error.c_str());
                }
                continue;
            }
            candidate.resolved_seq = this->next_seq++;
            candidate.announced = true;
            log_line(LogLevel::INFO, LOG_TAG_DISCOVERY, "found server \"%s\" (name: %s) at %s",
                     candidate.instance.c_str(),
                     candidate.name.empty() ? "<unnamed>" : candidate.name.c_str(), url.c_str());
        }
    }

    void fill(const Candidate& candidate, DiscoveredServer& out) const {
        out.instance = candidate.instance;
        out.name = candidate.name;
        out.path = candidate.path;
        out.port = candidate.port;
        out.addresses = candidate.addresses;
    }

    void forget(const std::string& instance) {
        const auto found = this->candidates.find(instance);
        if (found == this->candidates.end()) {
            return;
        }
        if (found->second->resolved_seq != 0) {
            log_line(LogLevel::INFO, LOG_TAG_DISCOVERY, "server \"%s\" went away",
                     instance.c_str());
        }
        this->cancel_resolve(*found->second);
        this->candidates.erase(found);
    }
};

MdnsService::MdnsService() : impl_(std::make_unique<Impl>()) {}

MdnsService::~MdnsService() = default;

bool MdnsService::advertise(const std::string& instance, uint16_t port, const std::string& path,
                            const std::string& friendly_name, std::string& error) {
    this->impl_->advertise_wanted = true;
    this->impl_->advertise_instance = instance;
    this->impl_->advertise_port = port;
    this->impl_->advertise_path = path;
    this->impl_->advertise_friendly_name = friendly_name;
    return this->impl_->start_register(error);
}

bool MdnsService::browse(std::string& error) {
    this->impl_->browse_wanted = true;
    return this->impl_->start_browse(error);
}

void MdnsService::poll(int64_t now_ms) {
    this->impl_->poll_once(now_ms);
}

std::vector<DiscoveredServer> MdnsService::servers() const {
    std::vector<const Impl::Candidate*> ready;
    for (const auto& entry : this->impl_->candidates) {
        if (entry.second->resolved_seq != 0) {
            ready.push_back(entry.second.get());
        }
    }
    // The map is ordered by instance name, so the resolution order has to be restored here.
    std::sort(ready.begin(), ready.end(), [](const Impl::Candidate* a, const Impl::Candidate* b) {
        return a->resolved_seq < b->resolved_seq;
    });

    std::vector<DiscoveredServer> servers;
    servers.reserve(ready.size());
    for (const Impl::Candidate* candidate : ready) {
        DiscoveredServer server;
        this->impl_->fill(*candidate, server);
        std::string url;
        std::string error;
        if (!discovered_server_url(server, url, error)) {
            continue;
        }
        servers.push_back(std::move(server));
    }
    return servers;
}

void MdnsService::stop() {
    this->impl_->advertise_wanted = false;
    this->impl_->browse_wanted = false;
    this->impl_->restart_at_ms = -1;
    this->impl_->teardown();
}

bool mdns_available() {
    return true;
}

std::string mdns_backend_name() {
    return "windns (Windows DNS-SD)";
}

}  // namespace sendspin_cli
