// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "dynamic_lease_policy.h"

namespace dynamic_lease_policy
{
namespace
{
struct Transaction {
    HANDLE engine;
    const Api &api;
    bool active = true;
    ~Transaction()
    {
        if (active) {
            api.abort(engine);
        }
    }
};

bool keysValid(const Objects &objects)
{
    constexpr GUID empty{};
    const std::array<GUID, 4> keys{objects.provider, objects.sublayer, objects.filters[0], objects.filters[1]};
    for (size_t index = 0; index != keys.size(); ++index) {
        if (IsEqualGUID(keys[index], empty)) {
            return false;
        }
        for (size_t previous = 0; previous != index; ++previous) {
            if (IsEqualGUID(keys[index], keys[previous])) {
                return false;
            }
        }
    }
    return true;
}

bool addFilter(HANDLE engine, const Objects &objects, const Scope &scope, size_t index, const Api &api)
{
    auto remote = FWP_V4_ADDR_AND_MASK{ntohl(scope.remote.S_un.S_addr), 0xffffffff};
    auto source = FWP_V4_ADDR_AND_MASK{ntohl(scope.source.S_un.S_addr), 0xffffffff};
    auto user = FWP_BYTE_BLOB{GetSecurityDescriptorLength(scope.user), static_cast<UINT8 *>(scope.user)};
    auto luid = scope.interfaceLuid;
    std::array<FWPM_FILTER_CONDITION0, 7> conditions{};
    conditions[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
    conditions[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
    conditions[0].conditionValue.byteBlob = const_cast<FWP_BYTE_BLOB *>(scope.app);
    conditions[1].fieldKey = FWPM_CONDITION_ALE_USER_ID;
    conditions[1].conditionValue.type = FWP_SECURITY_DESCRIPTOR_TYPE;
    conditions[1].conditionValue.sd = &user;
    conditions[2].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
    conditions[2].conditionValue.type = FWP_V4_ADDR_MASK;
    conditions[2].conditionValue.v4AddrMask = &remote;
    conditions[3].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
    conditions[3].conditionValue.type = FWP_UINT16;
    conditions[3].conditionValue.uint16 = 443;
    conditions[4].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
    conditions[4].conditionValue.type = FWP_UINT8;
    conditions[4].conditionValue.uint8 = IPPROTO_TCP;
    conditions[5].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
    conditions[5].conditionValue.type = FWP_V4_ADDR_MASK;
    conditions[5].conditionValue.v4AddrMask = &source;
    conditions[6].fieldKey = FWPM_CONDITION_IP_LOCAL_INTERFACE;
    conditions[6].conditionValue.type = FWP_UINT64;
    conditions[6].conditionValue.uint64 = &luid;
    for (auto &condition : conditions) {
        condition.matchType = FWP_MATCH_EQUAL;
    }
    auto weight = UINT64{0};
    FWPM_FILTER0 filter{};
    filter.filterKey = objects.filters[index];
    filter.providerKey = const_cast<GUID *>(&objects.provider);
    filter.subLayerKey = objects.sublayer;
    filter.layerKey = index == 0 ? FWPM_LAYER_ALE_AUTH_CONNECT_V4 : FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4;
    filter.displayData.name = const_cast<wchar_t *>(L"privateCloud authenticated fixed-server lease");
    filter.action.type = FWP_ACTION_PERMIT;
    filter.flags = FWPM_FILTER_FLAG_CLEAR_ACTION_RIGHT;
    filter.weight.type = FWP_UINT64;
    filter.weight.uint64 = &weight;
    filter.numFilterConditions = static_cast<UINT32>(conditions.size());
    filter.filterCondition = conditions.data();
    return api.addFilter(engine, &filter, nullptr, nullptr) == ERROR_SUCCESS;
}
}

bool createKeys(Objects &objects, const Api &api)
{
    objects = {};
    return api.uuid(&objects.provider) == RPC_S_OK && api.uuid(&objects.sublayer) == RPC_S_OK && api.uuid(&objects.filters[0]) == RPC_S_OK
        && api.uuid(&objects.filters[1]) == RPC_S_OK && keysValid(objects);
}

bool install(HANDLE engine, const Objects &objects, const Scope &scope, const std::function<bool()> &canceled, const Api &api)
{
    if (!engine || !keysValid(objects) || !scope.app || !scope.app->size || !scope.app->data || !scope.user || !IsValidSecurityDescriptor(scope.user)
        || !scope.remote.S_un.S_addr || !scope.source.S_un.S_addr || !scope.interfaceLuid || !canceled || canceled() || api.begin(engine, 0)) {
        return false;
    }
    Transaction transaction{engine, api};
    FWPM_PROVIDER0 provider{};
    provider.providerKey = objects.provider;
    provider.displayData.name = const_cast<wchar_t *>(L"privateCloud dynamic fixed-server lease");
    FWPM_SUBLAYER0 sublayer{};
    sublayer.subLayerKey = objects.sublayer;
    sublayer.providerKey = const_cast<GUID *>(&objects.provider);
    sublayer.displayData.name = const_cast<wchar_t *>(L"privateCloud dynamic direct connection");
    sublayer.weight = privateCloudPermitWeight;
    if (canceled() || api.addProvider(engine, &provider, nullptr) || canceled() || api.addSublayer(engine, &sublayer, nullptr) || canceled()) {
        return false;
    }
    FWPM_SUBLAYER0 *readback = nullptr;
    const auto read = api.getSublayer(engine, &objects.sublayer, &readback);
    const auto weightMatches = !read && readback && privateCloudPermitWeightMatches(readback->weight) && readback->providerKey
        && IsEqualGUID(*readback->providerKey, objects.provider) && IsEqualGUID(readback->subLayerKey, objects.sublayer) && readback->flags == 0;
    if (readback) {
        api.freeMemory(reinterpret_cast<void **>(&readback));
    }
    if (!weightMatches || canceled()) {
        return false;
    }
    for (size_t index = 0; index != objects.filters.size(); ++index) {
        if (canceled() || !addFilter(engine, objects, scope, index, api)) {
            return false;
        }
    }
    if (canceled() || api.commit(engine)) {
        return false;
    }
    transaction.active = false;
    return !canceled();
}
}
