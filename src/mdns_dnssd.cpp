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

/// @file mdns_dnssd.cpp
/// @brief MdnsService over `dns_sd.h` -- Bonjour on macOS, libavahi-compat-libdnssd on Linux
///
/// Everything here runs on the main loop thread, driven from poll(). That is a requirement
/// rather than a simplification: a browse result is what triggers
/// SendspinClient::connect_to(), which is documented as main-loop-only.
///
/// Address lookup goes through DNSServiceQueryRecord() rather than the more obvious
/// DNSServiceGetAddrInfo(): the latter is a Bonjour extension that
/// libavahi-compat-libdnssd does not implement at all, so it would take the Linux build
/// with it. QueryRecord() is in the compat layer, and being asynchronous it keeps the whole
/// resolve chain on the one thread -- where the POSIX getaddrinfo() upstream's
/// `examples/tui_client` falls back to needs a thread per lookup.
///
/// The Linux half of that is read from avahi's own `compat.c` and `unsupported.c`, not run:
/// this has only been exercised against Bonjour. See docs/ROADMAP.md item 12.

#include "mdns.h"

#include "log.h"
#include "outbound.h"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <netinet/in.h>
#include <poll.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sendspin_cli {

using sendspin::LogLevel;

/// The advertising half of this file. The browse, resolve and address-query callbacks
/// speak for discovery instead, and say so by calling log_line() with that tag.
static constexpr const char* LOG_TAG = LOG_TAG_MDNS;

namespace {

/// How long after its first address a candidate waits before it is offered for dialling.
///
/// The A and AAAA queries are issued together and a responder usually answers both in one
/// packet, but nothing guarantees they land in the same poll() tick. Without this window a
/// dual-stack server whose AAAA arrived first would be dialled over IPv6, against the
/// stated preference for IPv4 -- and the reference server binds V4Only, so that is a route
/// to a listener that may not be there.
constexpr int64_t ADDRESS_SETTLE_MS = 250;

/// A TXT value, or empty when the key is absent.
std::string txt_value(uint16_t txt_len, const unsigned char* txt_record, const char* key) {
    uint8_t length = 0;
    const void* value = TXTRecordGetValuePtr(txt_len, txt_record, key, &length);
    if (value == nullptr) {
        return {};
    }
    return std::string(static_cast<const char*>(value), length);
}

/// An A or AAAA record's rdata as a printable literal, or empty when it is neither.
std::string address_literal(uint16_t rrtype, uint16_t rdlen, const void* rdata) {
    char text[INET6_ADDRSTRLEN] = {};
    if (rrtype == kDNSServiceType_A && rdlen == sizeof(in_addr)) {
        if (inet_ntop(AF_INET, rdata, text, sizeof(text)) != nullptr) {
            return text;
        }
    } else if (rrtype == kDNSServiceType_AAAA && rdlen == sizeof(in6_addr)) {
        if (inet_ntop(AF_INET6, rdata, text, sizeof(text)) != nullptr) {
            return text;
        }
    }
    return {};
}

}  // namespace

/// The whole of the dns_sd state, kept out of the header so nothing else has to see
/// `dns_sd.h` -- which the null build does not have at all.
struct MdnsService::Impl {
    /// A `_sendspin-server._tcp` instance being resolved, or already resolved.
    struct Candidate {
        std::string instance;
        std::string name;
        std::string path;
        std::string host;  ///< SRV target, the input to the address queries
        uint16_t port{0};
        uint32_t interface_index{0};  ///< where it resolved, and so where addresses are asked
        std::set<uint32_t> interfaces;  ///< every interface the browse still reports it on
        std::vector<std::string> addresses;

        DNSServiceRef resolve_ref{nullptr};
        DNSServiceRef v4_ref{nullptr};
        DNSServiceRef v6_ref{nullptr};

        int64_t first_address_ms{-1};
        uint64_t resolved_seq{0};  ///< 0 until ready; otherwise the order it resolved in
        bool announced{false};     ///< whether the "found" line has already been logged
        bool unusable{false};      ///< dropped for what it is, not for having gone away
    };

    /// What a callback needs to find its way back. Owned by the candidate, so it outlives
    /// every ref that points at it and dies with them.
    struct CandidateContext {
        Impl* impl{nullptr};
        std::string instance;
    };

    // -- what was asked for, kept so a daemon restart can be recovered from --------------
    bool advertise_wanted{false};
    std::string advertise_instance;
    std::string advertise_path;
    std::string advertise_friendly_name;
    uint16_t advertise_port{0};
    bool browse_wanted{false};

    // -- live dns_sd state ---------------------------------------------------------------
    DNSServiceRef register_ref{nullptr};
    DNSServiceRef browse_ref{nullptr};
    std::map<std::string, std::unique_ptr<Candidate>> candidates;
    std::map<std::string, std::unique_ptr<CandidateContext>> contexts;

    /// Refs finished with during a callback. Deallocating a ref from inside its own
    /// DNSServiceProcessResult() is asking the daemon to free what it is still using, so
    /// they are retired here and released once processing has returned.
    std::vector<DNSServiceRef> retired;

    /// Candidates to erase once the callbacks have returned -- either because the browse
    /// says they have gone, or because they turned out to be undialable. Deferred for the
    /// same reason `retired` is: erasing a candidate frees the refs a callback is inside.
    std::vector<std::string> doomed;

    uint64_t next_seq{1};

    // -- recovery ------------------------------------------------------------------------
    int64_t restart_at_ms{-1};
    uint32_t restart_attempt{0};

    /// The tick poll_once() is in, so a callback can stamp when an address arrived.
    int64_t now_ms{0};

    ~Impl() {
        this->teardown();
    }

    // ====================================================================================
    // Lifecycle
    // ====================================================================================

    bool start_register(std::string& error) {
        // Byte limits, not character limits: an instance label is one DNS label (63 bytes)
        // and a TXT value's length is carried in a single octet. Truncating is what keeps
        // dns_sd's own uint8_t length argument from silently wrapping on a long -n.
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

        TXTRecordRef txt;
        TXTRecordCreate(&txt, 0, nullptr);
        // `path` is REQUIRED by the spec; `name` is optional, so an empty one is left out
        // rather than advertised as an empty string.
        TXTRecordSetValue(&txt, "path", static_cast<uint8_t>(path.size()), path.c_str());
        if (!friendly.empty()) {
            TXTRecordSetValue(&txt, "name", static_cast<uint8_t>(friendly.size()),
                              friendly.c_str());
        }

        // Flags 0 so the daemon renames rather than failing on a collision; the register
        // callback is what reports the name we actually got.
        const DNSServiceErrorType err = DNSServiceRegister(
            &this->register_ref, 0, kDNSServiceInterfaceIndexAny, instance.c_str(),
            MDNS_CLIENT_SERVICE, nullptr, nullptr, htons(this->advertise_port),
            TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), Impl::register_callback, this);

        TXTRecordDeallocate(&txt);

        if (err != kDNSServiceErr_NoError) {
            this->register_ref = nullptr;
            error = "could not register " + std::string(MDNS_CLIENT_SERVICE) + ": " +
                    Impl::describe_error(err);
            return false;
        }
        return true;
    }

    bool start_browse(std::string& error) {
        const DNSServiceErrorType err =
            DNSServiceBrowse(&this->browse_ref, 0, kDNSServiceInterfaceIndexAny,
                             MDNS_SERVER_SERVICE, nullptr, Impl::browse_callback, this);
        if (err != kDNSServiceErr_NoError) {
            this->browse_ref = nullptr;
            error = "could not browse " + std::string(MDNS_SERVER_SERVICE) + ": " +
                    Impl::describe_error(err);
            return false;
        }
        // A browse has no callback-confirmed success the way a registration does, so the
        // backoff is cleared here instead.
        this->restart_attempt = 0;
        return true;
    }

    void teardown() {
        // Retired refs go first: they still carry a pointer to a CandidateContext, so
        // clearing the contexts before releasing them would break the ownership rule the
        // context is documented to keep.
        this->drain_retired();

        for (auto& entry : this->candidates) {
            this->release_candidate_refs(*entry.second);
        }
        this->candidates.clear();
        this->contexts.clear();
        this->doomed.clear();

        Impl::release(this->browse_ref);
        Impl::release(this->register_ref);
    }

    /// Tears everything down and schedules a fresh registration.
    ///
    /// The case this exists for is `avahi-daemon` being restarted: every ref then reports
    /// kDNSServiceErr_ServiceNotRunning forever, and a player that ignored it would go
    /// permanently undiscoverable while logging nothing at all.
    ///
    /// Everything goes, not just the ref that reported it, and that is deliberate:
    /// DNSServiceProcessResult() fails for connection-level reasons -- the daemon is gone --
    /// rather than per-operation ones, which arrive through each callback's own errorCode.
    /// So one ref reporting it means every ref is already dead.
    void fail(DNSServiceErrorType err, const char* what, int64_t now_ms) {
        cli_log(LogLevel::ERROR, "%s failed (%s) -- restarting mDNS in %u ms", what,
                Impl::describe_error(err).c_str(), next_retry_delay_ms(this->restart_attempt));
        this->teardown();
        this->schedule_restart(now_ms);
    }

    void schedule_restart(int64_t now_ms) {
        this->restart_at_ms =
            now_ms + static_cast<int64_t>(next_retry_delay_ms(this->restart_attempt));
        if (next_retry_delay_ms(this->restart_attempt) < MAX_RETRY_DELAY_MS) {
            ++this->restart_attempt;
        }
    }

    /// Starts whatever is wanted and not already running.
    ///
    /// Only what is missing: a registration and a browse can fail independently -- one
    /// starting while the other does not is the ordinary case at boot -- and re-running a
    /// live one would overwrite its `DNSServiceRef` without deallocating it, leaking the
    /// ref and leaving two advertisements of the same instance.
    void restart() {
        std::string error;
        if (this->advertise_wanted && this->register_ref == nullptr &&
            !this->start_register(error)) {
            cli_log(LogLevel::WARN, "%s", error.c_str());
        }
        if (this->browse_wanted && this->browse_ref == nullptr && !this->start_browse(error)) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "%s", error.c_str());
        }
    }

    // ====================================================================================
    // The poll pump
    // ====================================================================================

    void poll_once(int64_t now_ms) {
        this->now_ms = now_ms;

        // Anything asked for but not running is owed a retry, whichever way it stopped --
        // a daemon that was not up when we started looks the same here as one that was
        // restarted under us, and both should be recovered from rather than given up on.
        const bool missing = (this->advertise_wanted && this->register_ref == nullptr) ||
                             (this->browse_wanted && this->browse_ref == nullptr);
        if (missing && this->restart_at_ms < 0) {
            this->schedule_restart(now_ms);
        }

        if (this->restart_at_ms >= 0 && now_ms >= this->restart_at_ms) {
            this->restart_at_ms = -1;
            this->restart();
        }

        std::vector<DNSServiceRef> refs = this->live_refs();
        if (!refs.empty()) {
            std::vector<pollfd> fds;
            fds.reserve(refs.size());
            for (DNSServiceRef ref : refs) {
                fds.push_back(pollfd{DNSServiceRefSockFD(ref), POLLIN, 0});
            }

            // Zero timeout: this shares the main loop with client.loop(), so it may never
            // wait. DNSServiceProcessResult() blocks until a result arrives, which is why
            // it is called only for a descriptor poll() has already reported readable.
            if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), 0) > 0) {
                for (size_t i = 0; i < fds.size(); ++i) {
                    if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                        continue;
                    }
                    // A callback may have retired or torn down refs later in this snapshot.
                    if (!this->is_live(refs[i])) {
                        continue;
                    }
                    const DNSServiceErrorType err = DNSServiceProcessResult(refs[i]);
                    if (err != kDNSServiceErr_NoError) {
                        this->fail(err, "processing an mDNS result", now_ms);
                        break;
                    }
                }
            }
        }

        this->drain_retired();
        this->purge_doomed();
        this->settle_addresses(now_ms);
    }

    std::vector<DNSServiceRef> live_refs() const {
        std::vector<DNSServiceRef> refs;
        if (this->register_ref != nullptr) {
            refs.push_back(this->register_ref);
        }
        if (this->browse_ref != nullptr) {
            refs.push_back(this->browse_ref);
        }
        for (const auto& entry : this->candidates) {
            for (DNSServiceRef ref :
                 {entry.second->resolve_ref, entry.second->v4_ref, entry.second->v6_ref}) {
                if (ref != nullptr) {
                    refs.push_back(ref);
                }
            }
        }
        return refs;
    }

    /// Whether `ref` is still one of ours, checked between callbacks: an earlier one in the
    /// same tick may have retired it, or torn everything down.
    bool is_live(DNSServiceRef ref) const {
        if (ref == this->register_ref || ref == this->browse_ref) {
            return ref != nullptr;
        }
        for (const auto& entry : this->candidates) {
            const Candidate& candidate = *entry.second;
            if (ref == candidate.resolve_ref || ref == candidate.v4_ref ||
                ref == candidate.v6_ref) {
                return true;
            }
        }
        return false;
    }

    void drain_retired() {
        for (DNSServiceRef ref : this->retired) {
            DNSServiceRefDeallocate(ref);
        }
        this->retired.clear();
    }

    void purge_doomed() {
        for (const std::string& instance : this->doomed) {
            // A Remove that emptied the interface set can be followed, in the same tick, by
            // an Add on another one -- a flap rather than a departure. Such a candidate is
            // only really gone if nothing has put an interface back. One marked `unusable`
            // goes regardless: it is being dropped for what it is, not for where it is.
            const Candidate* candidate = this->find(instance);
            if (candidate != nullptr && !candidate->unusable && !candidate->interfaces.empty()) {
                continue;
            }
            this->forget(instance);
        }
        this->doomed.clear();
    }

    /// Marks candidates dialable once their addresses have had time to all arrive.
    void settle_addresses(int64_t now_ms) {
        for (auto& entry : this->candidates) {
            Candidate& candidate = *entry.second;
            if (candidate.resolved_seq != 0 || candidate.first_address_ms < 0) {
                continue;
            }
            if (now_ms - candidate.first_address_ms < ADDRESS_SETTLE_MS) {
                continue;
            }

            std::string url;
            std::string error;
            DiscoveredServer server;
            this->fill(candidate, server);
            if (!discovered_server_url(server, url, error)) {
                // Logged once per candidate, at debug: a server misconfigured this way is
                // otherwise completely invisible, but it is also not this player's problem.
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

    // ====================================================================================
    // Candidate bookkeeping
    // ====================================================================================

    void release_candidate_refs(Candidate& candidate) {
        Impl::release(candidate.resolve_ref);
        Impl::release(candidate.v4_ref);
        Impl::release(candidate.v6_ref);
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
        this->release_candidate_refs(*found->second);
        this->candidates.erase(found);
        this->contexts.erase(instance);
    }

    /// The context a candidate's callbacks are given, created on first use.
    CandidateContext* context_for(const std::string& instance) {
        auto& slot = this->contexts[instance];
        if (!slot) {
            slot = std::make_unique<CandidateContext>();
            slot->impl = this;
            slot->instance = instance;
        }
        return slot.get();
    }

    Candidate* find(const std::string& instance) {
        const auto found = this->candidates.find(instance);
        return found == this->candidates.end() ? nullptr : found->second.get();
    }

    void add_address(Candidate& candidate, const std::string& address, bool add, int64_t now_ms) {
        const auto found =
            std::find(candidate.addresses.begin(), candidate.addresses.end(), address);
        if (!add) {
            if (found != candidate.addresses.end()) {
                candidate.addresses.erase(found);
            }
            return;
        }
        if (found != candidate.addresses.end()) {
            return;
        }
        candidate.addresses.push_back(address);
        if (candidate.first_address_ms < 0) {
            candidate.first_address_ms = now_ms;
        }
    }

    // ====================================================================================
    // dns_sd callbacks -- all of them run inside poll_once(), on the main loop thread
    // ====================================================================================

    static void DNSSD_API register_callback(DNSServiceRef /*ref*/, DNSServiceFlags /*flags*/,
                                            DNSServiceErrorType err, const char* name,
                                            const char* regtype, const char* /*domain*/,
                                            void* context) {
        auto* impl = static_cast<Impl*>(context);
        if (err != kDNSServiceErr_NoError) {
            cli_log(LogLevel::ERROR, "registration was refused (%s)",
                    Impl::describe_error(err).c_str());
            return;
        }
        // Reported rather than assumed: with flags 0 the daemon renames on a collision, so
        // the name that went out may not be the name that was asked for. The asked-for name
        // is deliberately left as it was, so a re-registration after a daemon restart does
        // not compound the rename into "name (2) (3)".
        cli_log(LogLevel::INFO, "advertising %s as \"%s\" on port %u (path %s)", regtype, name,
                impl->advertise_port, impl->advertise_path.c_str());
        impl->restart_attempt = 0;
    }

    static void DNSSD_API browse_callback(DNSServiceRef /*ref*/, DNSServiceFlags flags,
                                          uint32_t interface_index, DNSServiceErrorType err,
                                          const char* service_name, const char* regtype,
                                          const char* domain, void* context) {
        auto* impl = static_cast<Impl*>(context);
        if (err != kDNSServiceErr_NoError) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "browse reported an error (%s)",
                     Impl::describe_error(err).c_str());
            return;
        }

        const std::string instance = service_name;
        log_line(LogLevel::DEBUG, LOG_TAG_DISCOVERY, "browse %s \"%s\" on interface %u",
                 (flags & kDNSServiceFlagsAdd) != 0 ? "found" : "lost", instance.c_str(),
                 interface_index);
        Candidate* existing = impl->find(instance);

        // Adds and removes are per interface, and one instance is normally reported on
        // several. So the candidate is only dropped once the *last* interface carrying it
        // has gone -- dropping it on the first Remove would lose a server that is still
        // there, and the browse would not re-Add it, since it never went away as far as the
        // daemon is concerned.
        if ((flags & kDNSServiceFlagsAdd) == 0) {
            if (existing != nullptr) {
                existing->interfaces.erase(interface_index);
                if (existing->interfaces.empty()) {
                    impl->doomed.push_back(instance);
                }
            }
            return;
        }

        if (existing != nullptr) {
            existing->interfaces.insert(interface_index);
            return;
        }

        auto candidate = std::make_unique<Candidate>();
        candidate->instance = instance;
        candidate->interface_index = interface_index;
        candidate->interfaces.insert(interface_index);

        // Resolved across every interface rather than on the one this reply arrived on, and
        // the regtype and domain are passed through from the reply rather than rebuilt.
        //
        // The interface matters: a host with more than one -- a VPN or a second physical
        // link alongside the LAN -- gets a browse reply per interface, and the instance is
        // only actually resolvable on the one it really lives on. Pinning the resolve to
        // whichever reply happened to arrive first is a coin toss that fails silently,
        // because a resolve for a service that is not on that interface simply never calls
        // back. kDNSServiceInterfaceIndexAny lets the daemon answer from wherever it can:
        // whichever interface replies first wins -- resolve_callback takes one reply and
        // retires the ref -- and that reply carries the interface the address queries use.
        const DNSServiceErrorType resolve_err = DNSServiceResolve(
            &candidate->resolve_ref, 0, kDNSServiceInterfaceIndexAny, service_name, regtype, domain,
            Impl::resolve_callback, impl->context_for(instance));
        if (resolve_err != kDNSServiceErr_NoError) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "could not resolve \"%s\" (%s)",
                     instance.c_str(), Impl::describe_error(resolve_err).c_str());
            impl->contexts.erase(instance);
            return;
        }

        impl->candidates.emplace(instance, std::move(candidate));
    }

    static void DNSSD_API resolve_callback(DNSServiceRef ref, DNSServiceFlags /*flags*/,
                                           uint32_t interface_index, DNSServiceErrorType err,
                                           const char* /*fullname*/, const char* hosttarget,
                                           uint16_t port, uint16_t txt_len,
                                           const unsigned char* txt_record, void* context) {
        auto* ctx = static_cast<CandidateContext*>(context);
        Candidate* candidate = ctx->impl->find(ctx->instance);
        if (candidate == nullptr) {
            return;
        }

        if (err != kDNSServiceErr_NoError) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY, "resolving \"%s\" failed (%s)",
                     ctx->instance.c_str(), Impl::describe_error(err).c_str());
            candidate->unusable = true;
            ctx->impl->doomed.push_back(ctx->instance);
            return;
        }

        // One resolve is enough: the SRV target and TXT are the same on every interface the
        // instance answers on.
        //
        // The guard is not belt-and-braces. A resolve delivers one reply per interface, and
        // a single DNSServiceProcessResult() call hands over all of the ones already
        // waiting -- so this fires several times in a row, before poll_once() gets to look
        // at `resolve_ref` again. Without the check the same ref would be retired once per
        // reply and then deallocated that many times, and the address queries below would
        // be reissued over the top of their own live refs.
        if (candidate->resolve_ref == nullptr) {
            return;
        }
        candidate->resolve_ref = nullptr;
        ctx->impl->retired.push_back(ref);

        log_line(LogLevel::DEBUG, LOG_TAG_DISCOVERY, "resolved \"%s\" to %s:%u on interface %u",
                 ctx->instance.c_str(), hosttarget, ntohs(port), interface_index);

        candidate->host = hosttarget;
        candidate->port = ntohs(port);
        candidate->interface_index = interface_index;
        candidate->path = txt_value(txt_len, txt_record, "path");
        candidate->name = txt_value(txt_len, txt_record, "name");

        // Both families are asked at once, and the settle window is what stops whichever
        // answers first from deciding the preference. The queries stay open, so an address
        // change reaches the candidate set instead of stranding it on a stale one.
        ctx->impl->start_address_query(*candidate, kDNSServiceType_A, candidate->v4_ref, ctx);
        ctx->impl->start_address_query(*candidate, kDNSServiceType_AAAA, candidate->v6_ref, ctx);

        // With neither query running no address can ever arrive, so the candidate would sit
        // in the map forever: never dialable, never announced, and never re-created, since
        // the browse has no reason to report it again. Dropping it puts it back in reach of
        // the next browse announcement, and one WARN is what makes the difference between
        // "nothing was found" and "something was found and then quietly lost".
        if (candidate->v4_ref == nullptr && candidate->v6_ref == nullptr) {
            log_line(LogLevel::WARN, LOG_TAG_DISCOVERY,
                     "cannot look up any address for \"%s\" (%s) -- dropping",
                     ctx->instance.c_str(), candidate->host.c_str());
            candidate->unusable = true;
            ctx->impl->doomed.push_back(ctx->instance);
        }
    }

    static void DNSSD_API query_callback(DNSServiceRef /*ref*/, DNSServiceFlags flags,
                                         uint32_t /*interface_index*/, DNSServiceErrorType err,
                                         const char* /*fullname*/, uint16_t rrtype,
                                         uint16_t /*rrclass*/, uint16_t rdlen, const void* rdata,
                                         uint32_t /*ttl*/, void* context) {
        auto* ctx = static_cast<CandidateContext*>(context);
        Candidate* candidate = ctx->impl->find(ctx->instance);
        if (candidate == nullptr) {
            return;
        }
        if (err != kDNSServiceErr_NoError) {
            // Not fatal to the candidate: one family failing still leaves the other, and
            // kDNSServiceErr_NoSuchRecord is the ordinary answer for a v4-only host.
            log_line(LogLevel::DEBUG, LOG_TAG_DISCOVERY, "address query for \"%s\" returned %s",
                     ctx->instance.c_str(), Impl::describe_error(err).c_str());
            return;
        }

        const std::string address = address_literal(rrtype, rdlen, rdata);
        if (address.empty()) {
            return;
        }
        log_line(LogLevel::DEBUG, LOG_TAG_DISCOVERY, "\"%s\" %s address %s", ctx->instance.c_str(),
                 (flags & kDNSServiceFlagsAdd) != 0 ? "gained" : "lost", address.c_str());
        ctx->impl->add_address(*candidate, address, (flags & kDNSServiceFlagsAdd) != 0,
                               ctx->impl->now_ms);
    }

    void start_address_query(Candidate& candidate, uint16_t rrtype, DNSServiceRef& ref,
                             CandidateContext* ctx) {
        const DNSServiceErrorType err =
            DNSServiceQueryRecord(&ref, 0, candidate.interface_index, candidate.host.c_str(),
                                  rrtype, kDNSServiceClass_IN, Impl::query_callback, ctx);
        if (err != kDNSServiceErr_NoError) {
            ref = nullptr;
            log_line(LogLevel::DEBUG, LOG_TAG_DISCOVERY, "could not query %s for \"%s\" (%s)",
                     rrtype == kDNSServiceType_A ? "A" : "AAAA", candidate.instance.c_str(),
                     Impl::describe_error(err).c_str());
        }
    }

    // ====================================================================================
    // Helpers
    // ====================================================================================

    static void release(DNSServiceRef& ref) {
        if (ref != nullptr) {
            DNSServiceRefDeallocate(ref);
            ref = nullptr;
        }
    }

    /// dns_sd has no strerror of its own, so the few errors worth telling apart are named
    /// and the rest carry their number.
    ///
    /// Two of them are Bonjour's alone: libavahi-compat-libdnssd declares neither
    /// kDNSServiceErr_ServiceNotRunning nor kDNSServiceErr_Timeout, and both are enumerators
    /// rather than macros, so CMake compiles a use of each and defines the matching
    /// SENDSPIN_CLI_HAVE_ERR_* only where the header really has it. Where a header does not
    /// declare one, that code is not part of the error set that implementation documents, and
    /// anything it does return falls to the default and carries its number.
    static std::string describe_error(DNSServiceErrorType err) {
        switch (err) {
            case kDNSServiceErr_NoError:
                return "no error";
            case kDNSServiceErr_NameConflict:
                return "name conflict";
            case kDNSServiceErr_NoSuchRecord:
                return "no such record";
            case kDNSServiceErr_NoSuchName:
                return "no such name";
            case kDNSServiceErr_Unsupported:
                return "unsupported by this mDNS implementation";
#ifdef SENDSPIN_CLI_HAVE_ERR_SERVICE_NOT_RUNNING
            case kDNSServiceErr_ServiceNotRunning:
                return "the mDNS daemon is not running";
#endif
#ifdef SENDSPIN_CLI_HAVE_ERR_TIMEOUT
            case kDNSServiceErr_Timeout:
                return "timed out";
#endif
            default:
                return "dns_sd error " + std::to_string(err);
        }
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
        // Re-checked rather than trusted from settle time: an address can be *withdrawn*
        // after a candidate was announced, which would leave it offered here but no longer
        // dialable. Since selection takes the first match, one such candidate at the front
        // of the list would otherwise block every good server behind it, silently.
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
#ifdef __APPLE__
    return "dns_sd (Bonjour)";
#else
    return "dns_sd (avahi-compat)";
#endif
}

}  // namespace sendspin_cli
