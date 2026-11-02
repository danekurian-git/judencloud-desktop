// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Exercise the actual native notification callbacks without starting a service.
#define wmain privateCloudBrokerEntryForTests
#include "broker.cpp"
#undef wmain

int pureTests()
{
    int failures = 0;
    auto check = [&](bool okay, const char *name) {
        std::printf("%s %s\n", okay ? "PASS" : "FAIL", name);
        failures += !okay;
    };
    auto scope = [](Lease &lease) {
        InetPtonA(AF_INET, "203.0.113.77", &lease.endpoint.remote);
        lease.endpoint.luid.Value = 100;
        lease.endpoint.index = 19;
        lease.proof.tunnel.Value = 200;
        lease.metadataReady.store(true);
    };
    {
        Lease lease;
        scope(lease);
        MIB_IPFORWARD_ROW2 row{};
        row.InterfaceLuid.Value = 300;
        row.InterfaceIndex = 47;
        row.DestinationPrefix.Prefix.si_family = AF_INET;
        InetPtonA(AF_INET, "192.168.50.0", &row.DestinationPrefix.Prefix.Ipv4.sin_addr);
        row.DestinationPrefix.PrefixLength = 24;
        routeChanged(&lease, &row, MibAddInstance);
        check(WaitForSingleObject(lease.changed.value, 0) == WAIT_TIMEOUT, "unrelated subnet route keeps the connection");
    }
    {
        Lease lease;
        scope(lease);
        MIB_IPINTERFACE_ROW row{};
        row.Family = AF_INET;
        row.InterfaceLuid.Value = 300;
        row.InterfaceIndex = 47;
        interfaceChanged(&lease, &row, MibParameterNotification);
        check(WaitForSingleObject(lease.changed.value, 0) == WAIT_TIMEOUT, "unrelated interface keeps the connection");
    }
    {
        Lease lease;
        scope(lease);
        FWPM_PROVIDER_CHANGE0 change{};
        change.providerKey.Data1 = 123;
        providerChanged(&lease, &change);
        check(WaitForSingleObject(lease.changed.value, 0) == WAIT_TIMEOUT, "unrelated provider keeps the connection");
    }
    {
        Lease lease;
        scope(lease);
        FWPM_SUBLAYER_CHANGE0 change{};
        change.subLayerKey.Data1 = 123;
        sublayerChanged(&lease, &change);
        check(WaitForSingleObject(lease.changed.value, 0) == WAIT_TIMEOUT, "unrelated sublayer keeps the connection");
    }

    auto routeCase = [&](const char *prefix, unsigned bits, bool expected, const char *name) {
        Lease lease;
        scope(lease);
        MIB_IPFORWARD_ROW2 row{};
        row.InterfaceLuid.Value = 300;
        row.InterfaceIndex = 47;
        row.DestinationPrefix.Prefix.si_family = AF_INET;
        InetPtonA(AF_INET, prefix, &row.DestinationPrefix.Prefix.Ipv4.sin_addr);
        row.DestinationPrefix.PrefixLength = static_cast<UINT8>(bits);
        routeChanged(&lease, &row, MibDeleteInstance);
        check((WaitForSingleObject(lease.changed.value, 0) == WAIT_OBJECT_0) == expected, name);
    };
    routeCase("203.0.113.77", 32, true, "server host-route deletion revokes");
    routeCase("203.0.113.78", 32, false, "neighbor host-route deletion is ignored");
    routeCase("203.0.113.0", 24, true, "server subnet change revokes on another adapter too");
    routeCase("0.0.0.0", 0, true, "default-route change revokes");
    routeCase("192.168.0.0", 33, true, "invalid route prefix fails closed");
    auto interfaceCase = [&](UINT64 luid, NET_IFINDEX index, bool expected, const char *name) {
        Lease lease;
        scope(lease);
        MIB_IPINTERFACE_ROW row{};
        row.Family = AF_INET;
        row.InterfaceLuid.Value = luid;
        row.InterfaceIndex = index;
        interfaceChanged(&lease, &row, MibParameterNotification);
        check((WaitForSingleObject(lease.changed.value, 0) == WAIT_OBJECT_0) == expected, name);
    };
    interfaceCase(100, 19, true, "selected physical interface revokes");
    interfaceCase(200, 30, true, "recognized Tailscale tunnel revokes");
    interfaceCase(300, 19, true, "reused selected interface index revokes");
    interfaceCase(0, 0, true, "missing interface identity fails closed");
    interfaceCase(300, 47, false, "other adapter remains ignored");
    for (const auto kind : {0, 1, 2, 3, 4}) {
        Lease lease;
        scope(lease);
        switch (kind) {
        case 0:
            routeChanged(&lease, nullptr, MibParameterNotification);
            break;
        case 1:
            interfaceChanged(&lease, nullptr, MibParameterNotification);
            break;
        case 2:
            filterChanged(&lease, nullptr);
            break;
        case 3:
            sublayerChanged(&lease, nullptr);
            break;
        case 4:
            providerChanged(&lease, nullptr);
            break;
        }
        check(WaitForSingleObject(lease.changed.value, 0) == WAIT_OBJECT_0, "missing notification data fails closed");
    }
    {
        Lease lease;
        scope(lease);
        GUID key{};
        key.Data1 = 123;
        lease.watchedProviders.push_back(key);
        FWPM_PROVIDER_CHANGE0 change{};
        change.providerKey = key;
        providerChanged(&lease, &change);
        check(WaitForSingleObject(lease.changed.value, 0) == WAIT_OBJECT_0, "referenced provider revokes");
    }
    {
        Lease lease;
        scope(lease);
        GUID key{};
        key.Data1 = 123;
        lease.watchedSublayers.push_back(key);
        FWPM_SUBLAYER_CHANGE0 change{};
        change.subLayerKey = key;
        sublayerChanged(&lease, &change);
        check(WaitForSingleObject(lease.changed.value, 0) == WAIT_OBJECT_0, "referenced sublayer revokes");
    }
    {
        Lease lease;
        FWPM_PROVIDER_CHANGE0 change{};
        change.providerKey.Data1 = 123;
        providerChanged(&lease, &change);
        check(WaitForSingleObject(lease.changed.value, 0) == WAIT_OBJECT_0, "metadata changes during incomplete snapshot fail closed");
    }
    for (int own = 0; own < 3; ++own) {
        Lease lease;
        scope(lease);
        lease.objects.filters[0].Data1 = 1;
        lease.objects.filters[1].Data1 = 2;
        FWPM_FILTER_CHANGE0 change{};
        change.filterKey.Data1 = own + 1;
        filterChanged(&lease, &change);
        check((WaitForSingleObject(lease.changed.value, 0) == WAIT_OBJECT_0) == (own == 2), "own filters ignored; matching foreign filter revokes");
    }
    return failures ? 1 : 0;
}

struct NotificationCapture {
    GUID expected{};
    Handle received{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic<unsigned> count{0};
};
void CALLBACK captureFilter(void *context, const FWPM_FILTER_CHANGE0 *change)
{
    auto *capture = static_cast<NotificationCapture *>(context);
    if (change && IsEqualGUID(change->filterKey, capture->expected)) {
        ++capture->count;
        SetEvent(capture->received.value);
    }
}

int nativeTests()
{
    int failures = 0;
    auto check = [&](bool okay, const char *name) {
        std::printf("%s %s\n", okay ? "PASS" : "FAIL", name);
        std::fflush(stdout);
        failures += !okay;
        return okay;
    };
    Engine observer, writer;
    FWPM_SESSION0 session{};
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    session.txnWaitTimeoutInMSec = 3000;
    if (!check(FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &observer.value) == ERROR_SUCCESS, "open observer")
        || !check(FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &writer.value) == ERROR_SUCCESS, "open isolated dynamic session")) {
        return 1;
    }
    FWPM_PROVIDER0 provider{};
    UuidCreate(&provider.providerKey);
    provider.displayData.name = const_cast<wchar_t *>(L"privateCloud notification regression test");
    FWPM_SUBLAYER0 layer{};
    UuidCreate(&layer.subLayerKey);
    layer.providerKey = &provider.providerKey;
    layer.displayData.name = provider.displayData.name;
    if (!check(FwpmProviderAdd0(writer.value, &provider, nullptr) == ERROR_SUCCESS, "add temporary test provider")
        || !check(FwpmSubLayerAdd0(writer.value, &layer, nullptr) == ERROR_SUCCESS, "add temporary test sublayer")) {
        return 1;
    }
    Endpoint endpoint;
    InetPtonA(AF_INET, "203.0.113.77", &endpoint.remote);
    InetPtonA(AF_INET, "192.168.123.45", &endpoint.source);
    endpoint.luid.Value = 100;
    WfpMemory<FWP_BYTE_BLOB> app, otherApp;
    if (!check(FwpmGetAppIdFromFileName0(processPath(GetCurrentProcess()).c_str(), &app.value) == ERROR_SUCCESS, "resolve test application identity")
        || !check(FwpmGetAppIdFromFileName0(L"C:\\Windows\\System32\\cmd.exe", &otherApp.value) == ERROR_SUCCESS, "resolve different application identity")) {
        return 1;
    }
    const auto enumCase = [&](const GUID &layerKey, std::vector<FWPM_FILTER_CONDITION0> conditions, bool expected, const char *name) {
        if (!check(FwpmTransactionBegin0(writer.value, 0) == ERROR_SUCCESS, "begin isolated uncommitted test")) {
            return;
        }
        FWPM_FILTER0 filter{};
        UuidCreate(&filter.filterKey);
        filter.providerKey = &provider.providerKey;
        filter.subLayerKey = layer.subLayerKey;
        filter.layerKey = layerKey;
        filter.displayData.name = provider.displayData.name;
        // No permit or block: enumeration fixtures are never committed. Even
        // subscription fixtures below merely continue ordinary filtering.
        filter.action.type = FWP_ACTION_CONTINUE;
        filter.weight.type = FWP_EMPTY;
        filter.numFilterConditions = static_cast<UINT32>(conditions.size());
        filter.filterCondition = conditions.data();
        const auto added = FwpmFilterAdd0(writer.value, &filter, nullptr, nullptr);
        bool found = false;
        TrafficFilterQuery query(endpoint, app.value, FWPM_LAYER_ALE_AUTH_CONNECT_V4);
        HANDLE enumeration = nullptr;
        auto status = added ? added : FwpmFilterCreateEnumHandle0(writer.value, &query.value, &enumeration);
        if (!status) {
            for (;;) {
                WfpMemory<FWPM_FILTER0 *> entries;
                UINT32 count = 0;
                status = FwpmFilterEnum0(writer.value, enumeration, 256, &entries.value, &count);
                if (status || !count) {
                    break;
                }
                for (UINT32 index = 0; index < count; ++index) {
                    found |= IsEqualGUID(entries.value[index]->filterKey, filter.filterKey) != FALSE;
                }
            }
            status |= FwpmFilterDestroyEnumHandle0(writer.value, enumeration);
        }
        status |= FwpmTransactionAbort0(writer.value);
        if (status) {
            std::printf("native status %lu (add %lu)\n", status, added);
        }
        check(!status && found == expected, name);
    };
    TrafficFilterQuery fixture(endpoint, app.value, FWPM_LAYER_ALE_AUTH_CONNECT_V4);
    const auto exact = std::vector<FWPM_FILTER_CONDITION0>(fixture.conditions.begin(), fixture.conditions.end());
    enumCase(FWPM_LAYER_ALE_AUTH_CONNECT_V4, {}, true, "catch-all policy remains relevant");
    enumCase(FWPM_LAYER_ALE_AUTH_CONNECT_V4, exact, true, "exact connection policy remains relevant");
    for (unsigned field = 0; field != 6; ++field) {
        auto different = exact;
        UINT64 otherLuid = 999;
        switch (field) {
        case 0:
            different[0].conditionValue.uint32 ^= 1;
            break;
        case 1:
            different[1].conditionValue.uint16 = 8443;
            break;
        case 2:
            different[2].conditionValue.uint8 = IPPROTO_UDP;
            break;
        case 3:
            different[3].conditionValue.byteBlob = otherApp.value;
            break;
        case 4:
            different[4].conditionValue.uint32 ^= 1;
            break;
        case 5:
            different[5].conditionValue.uint64 = &otherLuid;
            break;
        }
        enumCase(FWPM_LAYER_ALE_AUTH_CONNECT_V4, different, false, "disjoint address/port/protocol/app/source/interface is ignored");
    }
    enumCase(FWPM_LAYER_ALE_AUTH_CONNECT_V6, {}, false, "IPv6-only change is irrelevant to IPv4 connection");
    FWP_V4_ADDR_AND_MASK subnet{0xcb007100, 0xffffff00};
    auto subnetConditions = exact;
    subnetConditions[0].conditionValue.type = FWP_V4_ADDR_MASK;
    subnetConditions[0].conditionValue.v4AddrMask = &subnet;
    enumCase(FWPM_LAYER_ALE_AUTH_CONNECT_V4, subnetConditions, true, "containing IPv4 subnet remains relevant");
    FWP_RANGE0 range{};
    range.valueLow.type = range.valueHigh.type = FWP_UINT16;
    range.valueLow.uint16 = 400;
    range.valueHigh.uint16 = 500;
    auto rangeConditions = exact;
    rangeConditions[1].matchType = FWP_MATCH_RANGE;
    rangeConditions[1].conditionValue.type = FWP_RANGE_TYPE;
    rangeConditions[1].conditionValue.rangeValue = &range;
    enumCase(FWPM_LAYER_ALE_AUTH_CONNECT_V4, rangeConditions, true, "port range overlapping 443 remains relevant");
    range.valueLow.uint16 = 800;
    range.valueHigh.uint16 = 900;
    enumCase(FWPM_LAYER_ALE_AUTH_CONNECT_V4, rangeConditions, false, "disjoint port range ignored");

    NotificationCapture capture;
    TrafficFilterQuery query(endpoint, app.value, FWPM_LAYER_ALE_AUTH_CONNECT_V4);
    FWPM_FILTER_SUBSCRIPTION0 subscription{};
    subscription.flags = FWPM_SUBSCRIPTION_FLAG_NOTIFY_ON_ADD | FWPM_SUBSCRIPTION_FLAG_NOTIFY_ON_DELETE;
    subscription.enumTemplate = &query.value;
    HANDLE subscribed = nullptr;
    const auto subscribedStatus = FwpmFilterSubscribeChanges0(observer.value, &subscription, captureFilter, &capture, &subscribed);
    if (!check(subscribedStatus == ERROR_SUCCESS, "subscribe with production traffic template")) {
        std::printf("subscription status %lu\n", subscribedStatus);
        return 1;
    }
    for (const bool matching : {false, true}) {
        auto conditions = exact;
        conditions[1].conditionValue.uint16 = matching ? 443 : 8443;
        FWPM_FILTER0 filter{};
        UuidCreate(&filter.filterKey);
        capture.expected = filter.filterKey;
        capture.count.store(0);
        ResetEvent(capture.received.value);
        filter.providerKey = &provider.providerKey;
        filter.subLayerKey = layer.subLayerKey;
        filter.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
        filter.displayData.name = provider.displayData.name;
        filter.action.type = FWP_ACTION_CONTINUE;
        filter.weight.type = FWP_EMPTY;
        filter.numFilterConditions = static_cast<UINT32>(conditions.size());
        filter.filterCondition = conditions.data();
        const auto added = FwpmFilterAdd0(writer.value, &filter, nullptr, nullptr);
        check(added == ERROR_SUCCESS, "add nonterminating TEST-NET-only subscription fixture");
        const auto notified = WaitForSingleObject(capture.received.value, matching ? 3000 : 500) == WAIT_OBJECT_0;
        check(notified == matching, "native add notification includes relevant and excludes unrelated filter");
        ResetEvent(capture.received.value);
        check(FwpmFilterDeleteByKey0(writer.value, &filter.filterKey) == ERROR_SUCCESS, "delete fixture");
        const auto deleted = WaitForSingleObject(capture.received.value, matching ? 3000 : 500) == WAIT_OBJECT_0;
        check(deleted == matching, "native deletion uses original filter scope");
    }
    check(FwpmFilterUnsubscribeChanges0(observer.value, subscribed) == ERROR_SUCCESS, "unsubscribe cleanly");
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--native") {
        FILE *report = nullptr;
        if (freopen_s(&report, argv[2], "wx", stdout)) {
            return 2;
        }
        return nativeTests();
    }
    return argc == 1 ? pureTests() : 2;
}
