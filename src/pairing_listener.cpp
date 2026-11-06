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

#include "pairing_listener.h"

#include "log.h"

namespace sendspin_cli {

using sendspin::LogLevel;

static constexpr const char* LOG_TAG = LOG_TAG_CLI;

namespace {

/// Why a pairing attempt ended, for the log.
const char* pair_abort_reason(sendspin::SendspinPairAbortReason reason) {
    switch (reason) {
        case sendspin::SendspinPairAbortReason::ATTEMPT_TIMEOUT:
            return "the server did not finish in time";
        case sendspin::SendspinPairAbortReason::CONCURRENT_ATTEMPT:
            return "another pairing attempt was already in progress";
        case sendspin::SendspinPairAbortReason::METHOD_NOT_SUPPORTED:
            return "the server chose a pairing method this player does not offer";
        case sendspin::SendspinPairAbortReason::PAIRING_CODE_MISMATCH:
            return "the pairing code did not match";
        case sendspin::SendspinPairAbortReason::USER_CANCELLED:
            return "it was cancelled";
        case sendspin::SendspinPairAbortReason::UNKNOWN:
            break;
    }
    return "the exchange broke down";
}

}  // namespace

void PairingListener::tick(bool connected, bool paired) {
    if (this->connected_ && !connected) {
        // An attempt on a dropped connection never reports its end. Forgotten here rather than
        // when the next session comes up: that session's first server/activate can start pairing,
        // and loop() delivers the start before is_connected() first reads true.
        this->server_id_.clear();
    }
    if (connected && (!this->connected_ || paired != this->paired_)) {
        log_line(LogLevel::INFO, LOG_TAG, "Connected server is %s",
                 paired ? "paired" : "not paired");
    }
    this->connected_ = connected;
    this->paired_ = paired;
}

void PairingListener::on_pairing_started(const std::string& server_id) {
    this->server_id_ = server_id;
    log_line(LogLevel::INFO, LOG_TAG, "Pairing started with server %s", server_id.c_str());
}

void PairingListener::on_pairing_succeeded(const std::string& server_id) {
    this->server_id_.clear();
    log_line(LogLevel::INFO, LOG_TAG, "Paired with server %s", server_id.c_str());
}

void PairingListener::on_pairing_failed(const std::string& server_id,
                                        sendspin::SendspinPairAbortReason reason) {
    this->server_id_.clear();
    log_line(LogLevel::WARN, LOG_TAG, "Pairing with server %s failed: %s", server_id.c_str(),
             pair_abort_reason(reason));
}

void PairingListener::on_display_pairing_code(const std::string& code,
                                              sendspin::SendspinPairingCodeFormat /*format*/) {
    this->code_ = code;
    log_line(LogLevel::INFO, LOG_TAG, "*** Pairing code: %s *** -- type it into the server to pair",
             code.c_str());
}

void PairingListener::on_clear_pairing_code() {
    // Also how an attempt the server abandoned ends: neither outcome callback fires for it.
    this->server_id_.clear();
    this->code_.clear();
    log_line(LogLevel::INFO, LOG_TAG, "Pairing code withdrawn");
}

void PairingListener::on_open_pairing_window() {
    this->window_open_ = true;
    log_line(LogLevel::INFO, LOG_TAG,
             "A server is waiting to pair -- run 'sendspin-cli pair confirm' to let it, or "
             "'sendspin-cli pair cancel' to refuse");
}

void PairingListener::on_close_pairing_window() {
    // Fires when the attempt concludes, like on_clear_pairing_code().
    this->server_id_.clear();
    this->window_open_ = false;
    log_line(LogLevel::INFO, LOG_TAG, "No longer waiting for 'pair confirm'");
}

}  // namespace sendspin_cli
