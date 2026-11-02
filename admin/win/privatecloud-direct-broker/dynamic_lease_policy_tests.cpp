// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <initguid.h>
#include "dynamic_lease_policy.h"
#include <sddl.h>

#include <cstdio>

namespace
{
enum class Failure {
    None,
    Begin,
    Provider,
    Sublayer,
    Readback,
    Weight,
    FirstFilter,
    SecondFilter,
    Commit,
    CancelAfterProvider,
    CancelAfterCommit,
};
Failure failure;
bool canceled = false;
int checks = 0, failures = 0, begins = 0, aborts = 0, commits = 0, providers = 0, sublayers = 0, filters = 0;
ULONG nextGuid = 1;
FWPM_SUBLAYER0 readback{};
const dynamic_lease_policy::Objects *expectedObjects = nullptr;
const dynamic_lease_policy::Scope *expectedScope = nullptr;

void check(bool condition, const char *name)
{
    ++checks;
    failures += !condition;
    if (!condition) {
        std::printf("FAIL %s\n", name);
    }
}

RPC_STATUS RPC_ENTRY uuid(UUID *key)
{
    *key = {};
    key->Data1 = nextGuid++;
    return RPC_S_OK;
}
RPC_STATUS RPC_ENTRY uuidFailure(UUID *)
{
    return RPC_S_OUT_OF_MEMORY;
}
DWORD WINAPI begin(HANDLE, UINT32 flags)
{
    ++begins;
    check(flags == 0, "one writable transaction");
    return failure == Failure::Begin ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
DWORD WINAPI abort(HANDLE)
{
    ++aborts;
    return ERROR_SUCCESS;
}
DWORD WINAPI commit(HANDLE)
{
    ++commits;
    canceled |= failure == Failure::CancelAfterCommit;
    return failure == Failure::Commit ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
DWORD WINAPI provider(HANDLE, const FWPM_PROVIDER0 *value, PSECURITY_DESCRIPTOR security)
{
    ++providers;
    check(IsEqualGUID(value->providerKey, expectedObjects->provider) && value->flags == 0 && !value->serviceName && !value->providerData.size && !security,
          "fresh nonpersistent provider belongs to dynamic session");
    canceled |= failure == Failure::CancelAfterProvider;
    return failure == Failure::Provider ? FWP_E_ALREADY_EXISTS : ERROR_SUCCESS;
}
DWORD WINAPI sublayer(HANDLE, const FWPM_SUBLAYER0 *value, PSECURITY_DESCRIPTOR security)
{
    ++sublayers;
    check(providers == 1 && IsEqualGUID(value->subLayerKey, expectedObjects->sublayer) && value->providerKey
              && IsEqualGUID(*value->providerKey, expectedObjects->provider) && value->flags == 0 && value->weight == privateCloudPermitWeight && !security,
          "own sublayer links only own provider at approved weight");
    readback = *value;
    readback.weight = failure == Failure::Weight ? privateCloudNextPriorityClass : privateCloudPermitWeight + 1;
    return failure == Failure::Sublayer ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
DWORD WINAPI getSublayer(HANDLE, const GUID *key, FWPM_SUBLAYER0 **value)
{
    check(IsEqualGUID(*key, expectedObjects->sublayer), "readback uses own sublayer only");
    *value = failure == Failure::Readback ? nullptr : &readback;
    return failure == Failure::Readback ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
void WINAPI freeMemory(void **value)
{
    check(*value == &readback, "readback allocation released");
    *value = nullptr;
}
DWORD WINAPI filter(HANDLE, const FWPM_FILTER0 *value, PSECURITY_DESCRIPTOR security, UINT64 *id)
{
    const auto index = filters++;
    check(index < 2 && providers == 1 && sublayers == 1 && value->providerKey && IsEqualGUID(*value->providerKey, expectedObjects->provider)
              && IsEqualGUID(value->subLayerKey, expectedObjects->sublayer) && IsEqualGUID(value->filterKey, expectedObjects->filters[index]),
          "filter links only objects from same lease");
    check(IsEqualGUID(value->layerKey, index == 0 ? FWPM_LAYER_ALE_AUTH_CONNECT_V4 : FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4)
              && value->flags == FWPM_FILTER_FLAG_CLEAR_ACTION_RIGHT && value->action.type == FWP_ACTION_PERMIT && value->weight.type == FWP_UINT64
              && *value->weight.uint64 == 0 && !security && !id,
          "hard permit uses only the two expected IPv4 ALE layers");
    check(value->numFilterConditions == 7, "exactly seven scope conditions");
    if (value->numFilterConditions != 7) {
        return ERROR_INVALID_DATA;
    }
    const auto *conditions = value->filterCondition;
    for (auto item = 0; item != 7; ++item) {
        check(conditions[item].matchType == FWP_MATCH_EQUAL, "every scope condition is equality");
    }
    check(IsEqualGUID(conditions[0].fieldKey, FWPM_CONDITION_ALE_APP_ID) && conditions[0].conditionValue.type == FWP_BYTE_BLOB_TYPE
              && conditions[0].conditionValue.byteBlob == expectedScope->app,
          "exact protected executable app ID");
    check(IsEqualGUID(conditions[1].fieldKey, FWPM_CONDITION_ALE_USER_ID) && conditions[1].conditionValue.type == FWP_SECURITY_DESCRIPTOR_TYPE
              && conditions[1].conditionValue.sd->data == expectedScope->user
              && conditions[1].conditionValue.sd->size == GetSecurityDescriptorLength(expectedScope->user),
          "exact authenticated user descriptor");
    check(IsEqualGUID(conditions[2].fieldKey, FWPM_CONDITION_IP_REMOTE_ADDRESS) && conditions[2].conditionValue.type == FWP_V4_ADDR_MASK
              && conditions[2].conditionValue.v4AddrMask->addr == ntohl(expectedScope->remote.S_un.S_addr)
              && conditions[2].conditionValue.v4AddrMask->mask == 0xffffffff,
          "exact remote IPv4");
    check(IsEqualGUID(conditions[3].fieldKey, FWPM_CONDITION_IP_REMOTE_PORT) && conditions[3].conditionValue.type == FWP_UINT16
              && conditions[3].conditionValue.uint16 == 443,
          "fixed HTTPS port");
    check(IsEqualGUID(conditions[4].fieldKey, FWPM_CONDITION_IP_PROTOCOL) && conditions[4].conditionValue.type == FWP_UINT8
              && conditions[4].conditionValue.uint8 == IPPROTO_TCP,
          "TCP only");
    check(IsEqualGUID(conditions[5].fieldKey, FWPM_CONDITION_IP_LOCAL_ADDRESS) && conditions[5].conditionValue.type == FWP_V4_ADDR_MASK
              && conditions[5].conditionValue.v4AddrMask->addr == ntohl(expectedScope->source.S_un.S_addr)
              && conditions[5].conditionValue.v4AddrMask->mask == 0xffffffff,
          "exact source IPv4");
    check(IsEqualGUID(conditions[6].fieldKey, FWPM_CONDITION_IP_LOCAL_INTERFACE) && conditions[6].conditionValue.type == FWP_UINT64
              && *conditions[6].conditionValue.uint64 == expectedScope->interfaceLuid,
          "exact physical interface LUID");
    return (failure == Failure::FirstFilter && index == 0) || (failure == Failure::SecondFilter && index == 1) ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}

void reset(Failure value)
{
    failure = value;
    canceled = false;
    begins = aborts = commits = providers = sublayers = filters = 0;
}
}

int main()
{
    const dynamic_lease_policy::Api api{&uuid, &begin, &abort, &commit, &provider, &sublayer, &getSublayer, &filter, &freeMemory};
    dynamic_lease_policy::Objects objects, second;
    check(dynamic_lease_policy::createKeys(objects, api) && dynamic_lease_policy::createKeys(second, api) && !IsEqualGUID(objects.provider, second.provider)
              && !IsEqualGUID(objects.sublayer, second.sublayer),
          "each lease receives fresh object identities");
    auto failingApi = api;
    failingApi.uuid = &uuidFailure;
    check(!dynamic_lease_policy::createKeys(second, failingApi), "UUID failure refuses lease identity creation");
    BYTE appBytes[] = {1, 2, 3};
    FWP_BYTE_BLOB app{sizeof(appBytes), appBytes};
    PSECURITY_DESCRIPTOR user = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;CC;;;SY)", SDDL_REVISION_1, &user, nullptr)) {
        return 1;
    }
    dynamic_lease_policy::Scope scope{&app, user, {}, {}, 42};
    scope.remote.S_un.S_addr = htonl(0x01020304);
    scope.source.S_un.S_addr = htonl(0xc0a80102);
    expectedObjects = &objects;
    expectedScope = &scope;
    const auto engine = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1));
    for (const auto scenario : {Failure::None,
                                Failure::Begin,
                                Failure::Provider,
                                Failure::Sublayer,
                                Failure::Readback,
                                Failure::Weight,
                                Failure::FirstFilter,
                                Failure::SecondFilter,
                                Failure::Commit,
                                Failure::CancelAfterProvider,
                                Failure::CancelAfterCommit}) {
        reset(scenario);
        const auto result = dynamic_lease_policy::install(
            engine,
            objects,
            scope,
            [] {
                return canceled;
            },
            api);
        check(result == (scenario == Failure::None), "only completely successful uncanceled install succeeds");
        const auto committed = scenario == Failure::None || scenario == Failure::CancelAfterCommit;
        check(aborts == ((scenario == Failure::Begin || committed) ? 0 : 1), "every open failed transaction aborts exactly once");
        if (committed) {
            check(commits == 1 && filters == 2, "both permits commit together; postcommit cancellation returns failure for owner session cleanup");
        }
    }
    reset(Failure::None);
    try {
        dynamic_lease_policy::install(
            engine,
            objects,
            scope,
            [] {
                if (providers) {
                    throw 1;
                }
                return false;
            },
            api);
        check(false, "injected exception must propagate after rollback");
    } catch (int) {
        check(aborts == 1 && commits == 0, "exception after provider creation aborts the owning transaction");
    }
    reset(Failure::None);
    canceled = true;
    check(!dynamic_lease_policy::install(
              engine,
              objects,
              scope,
              [] {
                  return canceled;
              },
              api)
              && begins == 0,
          "revoked lease never begins a transaction");
    canceled = false;
    scope.app = nullptr;
    check(!dynamic_lease_policy::install(
              engine,
              objects,
              scope,
              [] {
                  return false;
              },
              api)
              && begins == 0,
          "missing executable scope cannot broaden permit");
    scope.app = &app;
    scope.interfaceLuid = 0;
    check(!dynamic_lease_policy::install(
              engine,
              objects,
              scope,
              [] {
                  return false;
              },
              api)
              && begins == 0,
          "missing interface cannot broaden permit");
    scope.interfaceLuid = 42;
    objects.sublayer = objects.provider;
    check(!dynamic_lease_policy::install(
              engine,
              objects,
              scope,
              [] {
                  return false;
              },
              api)
              && begins == 0,
          "invalid duplicate object identities rejected");
    LocalFree(user);
    std::printf("Dynamic lease policy: %d checks, %d failures; injected WFP only.\n", checks, failures);
    return failures ? 1 : 0;
}
