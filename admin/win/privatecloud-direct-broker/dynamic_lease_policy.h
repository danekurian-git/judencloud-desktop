// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "policy_gate.h"
#include <fwpmu.h>
#include <rpc.h>

#include <array>
#include <functional>

namespace dynamic_lease_policy
{
struct Objects {
    GUID provider{};
    GUID sublayer{};
    std::array<GUID, 2> filters{};
};

struct Scope {
    const FWP_BYTE_BLOB *app = nullptr;
    PSECURITY_DESCRIPTOR user = nullptr;
    IN_ADDR remote{};
    IN_ADDR source{};
    UINT64 interfaceLuid = 0;
};

struct Api {
    decltype(&UuidCreate) uuid = &UuidCreate;
    decltype(&FwpmTransactionBegin0) begin = &FwpmTransactionBegin0;
    decltype(&FwpmTransactionAbort0) abort = &FwpmTransactionAbort0;
    decltype(&FwpmTransactionCommit0) commit = &FwpmTransactionCommit0;
    decltype(&FwpmProviderAdd0) addProvider = &FwpmProviderAdd0;
    decltype(&FwpmSubLayerAdd0) addSublayer = &FwpmSubLayerAdd0;
    decltype(&FwpmSubLayerGetByKey0) getSublayer = &FwpmSubLayerGetByKey0;
    decltype(&FwpmFilterAdd0) addFilter = &FwpmFilterAdd0;
    decltype(&FwpmFreeMemory0) freeMemory = &FwpmFreeMemory0;
};

// The supplied engine must be the owner's dynamic session. Every object is
// created there in one transaction; no existing object is adopted or modified.
bool createKeys(Objects &objects, const Api &api = {});
bool install(HANDLE engine, const Objects &objects, const Scope &scope, const std::function<bool()> &canceled, const Api &api = {});
}
