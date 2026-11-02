// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Fixed-destination privateCloud control-plane service. No relay or TLS parser.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <initguid.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <rpc.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>
#include <vector>

#include "broker_pipe_lifecycle.h"
#include "broker_process_security.h"
#include "dynamic_lease_policy.h"
#include "fixed_server_resolver.h"
#include "lease_watchdog.h"
#include "policy_gate.h"

namespace
{
constexpr wchar_t serviceName[] = L"privateCloudDirectBroker";
constexpr wchar_t pipeName[] = L"\\\\.\\pipe\\privateCloudDirectBroker-v1";
constexpr char initialRequest[] = "PRIVATECLOUD-DIRECT/1\n";
constexpr char renewalRequest[] = "PRIVATECLOUD-RENEW/1\n";
constexpr DWORD leaseMs = 120000;
constexpr DWORD clientSetupDeadlineMs = 10000;
constexpr DWORD ioDeadlineMs = 5000;

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v)
        : value(v)
    {
    }
    ~Handle()
    {
        reset();
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void reset(HANDLE v = nullptr)
    {
        if (value && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
        value = v;
    }
    explicit operator bool() const
    {
        return value && value != INVALID_HANDLE_VALUE;
    }
};
struct LocalMemory {
    void *value = nullptr;
    ~LocalMemory()
    {
        if (value) {
            LocalFree(value);
        }
    }
};
template<class T>
struct WfpMemory {
    T *value = nullptr;
    ~WfpMemory()
    {
        if (value) {
            FwpmFreeMemory0(reinterpret_cast<void **>(&value));
        }
    }
};
struct Engine {
    HANDLE value = nullptr;
    ~Engine()
    {
        if (value) {
            FwpmEngineClose0(value);
        }
    }
};

Handle stopEvent;
SERVICE_STATUS_HANDLE statusHandle = nullptr;
SRWLOCK statusLock = SRWLOCK_INIT;

void reportStatus(DWORD state, DWORD error = NO_ERROR)
{
    AcquireSRWLockExclusive(&statusLock);
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = error;
    status.dwWaitHint = state == SERVICE_STOP_PENDING || state == SERVICE_START_PENDING ? 10000 : 0;
    SetServiceStatus(statusHandle, &status);
    ReleaseSRWLockExclusive(&statusLock);
}

DWORD WINAPI controlHandler(DWORD control, DWORD, void *, void *)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        reportStatus(SERVICE_STOP_PENDING);
        SetEvent(stopEvent.value);
        return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

bool stopped()
{
    return WaitForSingleObject(stopEvent.value, 0) == WAIT_OBJECT_0;
}

std::wstring processPath(HANDLE process)
{
    std::vector<wchar_t> path(32768);
    auto size = static_cast<DWORD>(path.size());
    return QueryFullProcessImageNameW(process, 0, path.data(), &size) ? std::wstring(path.data(), size) : L"";
}

std::wstring programFilesPath()
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &raw))) {
        return {};
    }
    const std::wstring path(raw);
    CoTaskMemFree(raw);
    return path;
}

std::wstring brokerSid()
{
    const auto account = std::wstring(L"NT SERVICE\\") + serviceName;
    DWORD sidSize = 0, domainSize = 0;
    SID_NAME_USE use{};
    LookupAccountNameW(nullptr, account.c_str(), nullptr, &sidSize, nullptr, &domainSize, &use);
    if (!sidSize || sidSize > SECURITY_MAX_SID_SIZE || domainSize > 32768) {
        return {};
    }
    std::vector<BYTE> sid(sidSize);
    std::vector<wchar_t> domain(domainSize);
    if (!LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sidSize, domain.data(), &domainSize, &use)) {
        return {};
    }
    LocalMemory text;
    if (!ConvertSidToStringSidW(sid.data(), reinterpret_cast<LPWSTR *>(&text.value))) {
        return {};
    }
    return static_cast<wchar_t *>(text.value);
}

bool trustedOwner(PSID sid)
{
    if (!sid || !IsValidSid(sid)) {
        return false;
    }
    for (const auto type : {WinLocalSystemSid, WinBuiltinAdministratorsSid}) {
        BYTE storage[SECURITY_MAX_SID_SIZE]{};
        DWORD size = sizeof(storage);
        if (CreateWellKnownSid(type, nullptr, storage, &size) && EqualSid(sid, storage)) {
            return true;
        }
    }
    // Program Files is normally owned by TrustedInstaller.
    LocalMemory trusted;
    return ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &trusted.value) && EqualSid(sid, trusted.value);
}

bool protectedPath(const std::wstring &path, bool directory)
{
    Handle file(CreateFileW(path.c_str(),
                            READ_CONTROL | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                            nullptr));
    if (!file) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &attributes, sizeof(attributes))
        || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || bool(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) {
        return false;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    LocalMemory security;
    if (GetSecurityInfo(file.value,
                        SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner,
                        nullptr,
                        &dacl,
                        nullptr,
                        reinterpret_cast<PSECURITY_DESCRIPTOR *>(&security.value))
            != ERROR_SUCCESS
        || !trustedOwner(owner) || !dacl) {
        return false;
    }
    constexpr ACCESS_MASK writeMask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC
        | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER *header = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<void **>(&header))) {
            return false;
        }
        if (header->AceFlags & INHERIT_ONLY_ACE) {
            continue;
        }
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            continue;
        }
        // Fail closed on conditional/object ACEs we do not interpret.
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            return false;
        }
        const auto *allow = reinterpret_cast<const ACCESS_ALLOWED_ACE *>(header);
        if ((allow->Mask & writeMask) && !trustedOwner(const_cast<DWORD *>(&allow->SidStart))) {
            return false;
        }
    }
    return true;
}

std::vector<BYTE> tokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS type)
{
    DWORD size = 0;
    GetTokenInformation(token, type, nullptr, 0, &size);
    if (!size || size > 65536) {
        return {};
    }
    std::vector<BYTE> buffer(size);
    return GetTokenInformation(token, type, buffer.data(), size, &size) ? buffer : std::vector<BYTE>{};
}

struct Client {
    Handle process;
    DWORD pid = 0;
    DWORD session = 0;
    std::vector<BYTE> sid;
    std::wstring image;
};

bool authenticate(HANDLE pipe, Client &client)
{
    ULONG pid = 0, pipeSession = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !GetNamedPipeClientSessionId(pipe, &pipeSession) || !pid || !pipeSession) {
        return false;
    }
    client.process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!client.process || WaitForSingleObject(client.process.value, 0) != WAIT_TIMEOUT) {
        return false;
    }
    const auto base = programFilesPath();
    const auto product = base + L"\\privateCloud";
    const auto expected = product + L"\\privateCloud.exe";
    client.image = processPath(client.process.value);
    if (base.empty() || _wcsicmp(client.image.c_str(), expected.c_str()) != 0 || !protectedPath(base, true) || !protectedPath(product, true)
        || !protectedPath(expected, false)) {
        return false;
    }
    Handle token;
    if (!OpenProcessToken(client.process.value, TOKEN_QUERY, &token.value)) {
        return false;
    }
    const auto user = tokenInformation(token.value, TokenUser);
    const auto session = tokenInformation(token.value, TokenSessionId);
    const auto groups = tokenInformation(token.value, TokenGroups);
    if (user.empty() || session.size() != sizeof(DWORD) || groups.empty()) {
        return false;
    }
    client.session = *reinterpret_cast<const DWORD *>(session.data());
    if (!client.session || client.session != pipeSession) {
        return false;
    }
    const auto *tokenGroups = reinterpret_cast<const TOKEN_GROUPS *>(groups.data());
    bool interactive = false;
    BYTE interactiveSid[SECURITY_MAX_SID_SIZE]{};
    DWORD interactiveSize = sizeof(interactiveSid);
    if (!CreateWellKnownSid(WinInteractiveSid, nullptr, interactiveSid, &interactiveSize)) {
        return false;
    }
    for (DWORD i = 0; i < tokenGroups->GroupCount; ++i) {
        if ((tokenGroups->Groups[i].Attributes & SE_GROUP_ENABLED) && EqualSid(tokenGroups->Groups[i].Sid, interactiveSid)) {
            interactive = true;
        }
    }
    if (!interactive) {
        return false;
    }
    const PSID sid = reinterpret_cast<const TOKEN_USER *>(user.data())->User.Sid;
    client.sid.resize(GetLengthSid(sid));
    if (!CopySid(static_cast<DWORD>(client.sid.size()), client.sid.data(), sid)) {
        return false;
    }
    ULONG currentPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &currentPid) || currentPid != pid) {
        return false;
    }
    client.pid = pid;
    return WaitForSingleObject(client.process.value, 0) == WAIT_TIMEOUT;
}

bool publicV4(IN_ADDR address)
{
    const auto ip = ntohl(address.S_un.S_addr);
    const auto a = ip >> 24, b = (ip >> 16) & 255, c = (ip >> 8) & 255;
    return a != 0 && a != 10 && a != 127 && a < 224 && !(a == 100 && b >= 64 && b <= 127) && !(a == 169 && b == 254) && !(a == 172 && b >= 16 && b <= 31)
        && !(a == 192 && b == 168) && !(a == 192 && b == 0 && (c == 0 || c == 2)) && !(a == 192 && b == 88 && c == 99) && !(a == 198 && (b == 18 || b == 19))
        && !(a == 198 && b == 51 && c == 100) && !(a == 203 && b == 0 && c == 113);
}

std::vector<IN_ADDR> resolveServer(int *winsockError = nullptr, HANDLE cancellation = nullptr)
{
    const auto answer = fixed_server_resolver::resolve(cancellation ? cancellation : stopEvent.value);
    std::vector<IN_ADDR> result;
    if (winsockError) {
        *winsockError = answer.error;
    }
    if (answer.error) {
        return result;
    }
    for (const auto address : answer.addresses) {
        if (publicV4(address)) {
            result.push_back(address);
        }
    }
    std::sort(result.begin(), result.end(), [](IN_ADDR a, IN_ADDR b) {
        return ntohl(a.S_un.S_addr) < ntohl(b.S_un.S_addr);
    });
    result.erase(std::unique(result.begin(),
                             result.end(),
                             [](IN_ADDR a, IN_ADDR b) {
                                 return a.S_un.S_addr == b.S_un.S_addr;
                             }),
                 result.end());
    return result;
}

struct Endpoint {
    IN_ADDR remote{}, source{};
    NET_LUID luid{};
    NET_IFINDEX index = 0;
    std::vector<ULONG> dnsAnswers;
    bool operator==(const Endpoint &other) const
    {
        return remote.S_un.S_addr == other.remote.S_un.S_addr && source.S_un.S_addr == other.source.S_un.S_addr && luid.Value == other.luid.Value
            && index == other.index && dnsAnswers == other.dnsAnswers;
    }
};

bool chooseEndpoint(Endpoint &endpoint, LeaseWatchdog &watchdog)
{
    if (watchdog.revoked()) {
        return false;
    }
    const auto addresses = resolveServer(nullptr, watchdog.revocationEvent());
    if (addresses.empty() || stopped() || watchdog.revoked()) {
        return false;
    }
    ULONG size = 16384, result = ERROR_BUFFER_OVERFLOW;
    std::vector<BYTE> storage;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        if (watchdog.revoked()) {
            return false;
        }
        storage.resize(size);
        result = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                      nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()),
                                      &size);
    }
    if (result != NO_ERROR) {
        return false;
    }
    unsigned long long metric = ~0ULL;
    bool found = false;
    for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(storage.data()); adapter; adapter = adapter->Next) {
        if (watchdog.revoked()) {
            return false;
        }
        if (adapter->OperStatus != IfOperStatusUp || !adapter->FirstGatewayAddress
            || (adapter->IfType != IF_TYPE_ETHERNET_CSMACD && adapter->IfType != IF_TYPE_IEEE80211)) {
            continue;
        }
        MIB_IF_ROW2 row{};
        row.InterfaceLuid = adapter->Luid;
        if (GetIfEntry2(&row) != NO_ERROR || !row.InterfaceAndOperStatusFlags.HardwareInterface) {
            continue;
        }
        SOCKADDR_INET destination{};
        destination.Ipv4.sin_family = AF_INET;
        destination.Ipv4.sin_addr = addresses.front();
        MIB_IPFORWARD_ROW2 route{};
        SOCKADDR_INET source{};
        if (GetBestRoute2(&adapter->Luid, 0, nullptr, &destination, 0, &route, &source) != NO_ERROR || route.InterfaceLuid.Value != adapter->Luid.Value
            || source.si_family != AF_INET || !source.Ipv4.sin_addr.S_un.S_addr || route.NextHop.si_family != AF_INET
            || !route.NextHop.Ipv4.sin_addr.S_un.S_addr) {
            continue;
        }
        const auto candidate = static_cast<unsigned long long>(route.Metric) + adapter->Ipv4Metric;
        if (candidate > metric || (candidate == metric && found && adapter->IfIndex >= endpoint.index)) {
            continue;
        }
        endpoint = {addresses.front(), source.Ipv4.sin_addr, adapter->Luid, adapter->IfIndex, {}};
        for (const auto &address : addresses) {
            endpoint.dnsAnswers.push_back(address.S_un.S_addr);
        }
        found = true;
        metric = candidate;
    }
    return found && !watchdog.revoked();
}

struct RevocationDiagnostic {
    std::atomic<const char *> reason{nullptr};
    DWORD clientPid = 0;
    NET_IFINDEX interfaceIndex = 0;
    void record(const char *value)
    {
        const char *empty = nullptr;
        reason.compare_exchange_strong(empty, value);
    }
};

// BFE applies the overlap test to both additions and deletions, including
// catch-all rules. The callback never has to query an already-deleted filter.
struct TrafficFilterQuery {
    std::array<FWPM_FILTER_CONDITION0, 6> conditions{};
    UINT64 luid;
    FWPM_FILTER_ENUM_TEMPLATE0 value{};
    TrafficFilterQuery(const Endpoint &endpoint, const FWP_BYTE_BLOB *app, const GUID &layer)
        : luid(endpoint.luid.Value)
    {
        conditions[0].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
        conditions[0].conditionValue.type = FWP_UINT32;
        conditions[0].conditionValue.uint32 = ntohl(endpoint.remote.S_un.S_addr);
        conditions[1].fieldKey = FWPM_CONDITION_IP_REMOTE_PORT;
        conditions[1].conditionValue.type = FWP_UINT16;
        conditions[1].conditionValue.uint16 = 443;
        conditions[2].fieldKey = FWPM_CONDITION_IP_PROTOCOL;
        conditions[2].conditionValue.type = FWP_UINT8;
        conditions[2].conditionValue.uint8 = IPPROTO_TCP;
        conditions[3].fieldKey = FWPM_CONDITION_ALE_APP_ID;
        conditions[3].conditionValue.type = FWP_BYTE_BLOB_TYPE;
        conditions[3].conditionValue.byteBlob = const_cast<FWP_BYTE_BLOB *>(app);
        conditions[4].fieldKey = FWPM_CONDITION_IP_LOCAL_ADDRESS;
        conditions[4].conditionValue.type = FWP_UINT32;
        conditions[4].conditionValue.uint32 = ntohl(endpoint.source.S_un.S_addr);
        conditions[5].fieldKey = FWPM_CONDITION_IP_LOCAL_INTERFACE;
        conditions[5].conditionValue.type = FWP_UINT64;
        conditions[5].conditionValue.uint64 = &luid;
        for (auto &condition : conditions) {
            condition.matchType = FWP_MATCH_EQUAL;
        }
        value.layerKey = layer;
        value.enumType = FWP_FILTER_ENUM_OVERLAPPING;
        value.actionMask = 0xffffffff;
        value.flags = FWP_FILTER_ENUM_FLAG_INCLUDE_DISABLED;
        value.numFilterConditions = static_cast<UINT32>(conditions.size());
        value.filterCondition = conditions.data();
    }
    TrafficFilterQuery(const TrafficFilterQuery &) = delete;
    TrafficFilterQuery &operator=(const TrafficFilterQuery &) = delete;
};

bool containsKey(const std::vector<GUID> &keys, const GUID &key)
{
    return std::any_of(keys.begin(), keys.end(), [&](const GUID &value) {
        return IsEqualGUID(value, key) != FALSE;
    });
}
void rememberKey(std::vector<GUID> &keys, const GUID &key)
{
    if (!IsEqualGUID(key, GUID{}) && !containsKey(keys, key)) {
        keys.push_back(key);
    }
}

struct Lease {
    Handle changed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Engine engine;
    HANDLE routeNotification = nullptr, interfaceNotification = nullptr;
    std::array<HANDLE, 4> filterSubscriptions{};
    HANDLE sublayerSubscription = nullptr, providerSubscription = nullptr;
    dynamic_lease_policy::Objects objects;
    bool permits = false;
    Endpoint endpoint;
    PolicyProof proof;
    // Written before publication; notification callbacks only read these sets.
    std::vector<GUID> watchedProviders, watchedSublayers;
    std::atomic<bool> metadataReady{false};
    RevocationDiagnostic *diagnostic = nullptr;
    void revoke(const char *reason)
    {
        if (diagnostic) {
            diagnostic->record(reason);
        }
        SetEvent(changed.value);
    }
    ~Lease()
    {
        if (routeNotification) {
            CancelMibChangeNotify2(routeNotification);
        }
        if (interfaceNotification) {
            CancelMibChangeNotify2(interfaceNotification);
        }
        for (const auto subscription : filterSubscriptions) {
            if (subscription) {
                FwpmFilterUnsubscribeChanges0(engine.value, subscription);
            }
        }
        if (sublayerSubscription) {
            FwpmSubLayerUnsubscribeChanges0(engine.value, sublayerSubscription);
        }
        if (providerSubscription) {
            FwpmProviderUnsubscribeChanges0(engine.value, providerSubscription);
        }
        // Engine closes before changed is destroyed. Dynamic filters disappear
        // even on process termination through BFE's dynamic-session rundown.
    }
    [[nodiscard]] bool ownFilter(const GUID &key) const
    {
        return IsEqualGUID(key, objects.filters[0]) || IsEqualGUID(key, objects.filters[1]);
    }
};

void CALLBACK routeChanged(void *context, MIB_IPFORWARD_ROW2 *row, MIB_NOTIFICATION_TYPE)
{
    auto *lease = static_cast<Lease *>(context);
    // Even another adapter's default/server route can change the routing policy
    // inspected by the gate. A disjoint subnet cannot affect this destination.
    if (!row || row->DestinationPrefix.Prefix.si_family != AF_INET || row->DestinationPrefix.PrefixLength > 32) {
        lease->revoke("unverifiable-route-notification");
        return;
    }
    const auto bits = row->DestinationPrefix.PrefixLength;
    const ULONG mask = bits ? 0xffffffffUL << (32 - bits) : 0;
    if ((ntohl(row->DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr) & mask) == (ntohl(lease->endpoint.remote.S_un.S_addr) & mask)) {
        lease->revoke("server-route-changed");
    }
}
void CALLBACK interfaceChanged(void *context, MIB_IPINTERFACE_ROW *row, MIB_NOTIFICATION_TYPE)
{
    auto *lease = static_cast<Lease *>(context);
    if (!row || row->Family != AF_INET || (!row->InterfaceLuid.Value && !row->InterfaceIndex)) {
        lease->revoke("unverifiable-interface-notification");
    } else if (row->InterfaceLuid.Value == lease->endpoint.luid.Value || row->InterfaceIndex == lease->endpoint.index
               || (lease->proof.tunnel.Value && row->InterfaceLuid.Value == lease->proof.tunnel.Value)) {
        lease->revoke("selected-interface-or-tailscale-tunnel-changed");
    }
}
void CALLBACK filterChanged(void *context, const FWPM_FILTER_CHANGE0 *change)
{
    auto *lease = static_cast<Lease *>(context);
    if (!change || !lease->ownFilter(change->filterKey)) {
        lease->revoke("matching-traffic-or-tailscale-filter-changed");
    }
}
void CALLBACK sublayerChanged(void *context, const FWPM_SUBLAYER_CHANGE0 *change)
{
    auto *lease = static_cast<Lease *>(context);
    if (!change || !lease->metadataReady.load() || containsKey(lease->watchedSublayers, change->subLayerKey)) {
        lease->revoke("referenced-sublayer-changed");
    }
}
void CALLBACK providerChanged(void *context, const FWPM_PROVIDER_CHANGE0 *change)
{
    auto *lease = static_cast<Lease *>(context);
    if (!change || !lease->metadataReady.load() || containsKey(lease->watchedProviders, change->providerKey)) {
        lease->revoke("referenced-provider-changed");
    }
}

bool captureWatchedMetadata(Lease &lease, const FWP_BYTE_BLOB *app, LeaseWatchdog &watchdog)
{
    rememberKey(lease.watchedProviders, lease.proof.provider);
    rememberKey(lease.watchedProviders, lease.objects.provider);
    rememberKey(lease.watchedSublayers, lease.proof.sublayer);
    rememberKey(lease.watchedSublayers, lease.objects.sublayer);
    if (FwpmTransactionBegin0(lease.engine.value, FWPM_TXN_READ_ONLY)) {
        return false;
    }
    bool valid = true;
    for (const auto &layer : {FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4}) {
        TrafficFilterQuery query(lease.endpoint, app, layer);
        HANDLE enumeration = nullptr;
        if (FwpmFilterCreateEnumHandle0(lease.engine.value, &query.value, &enumeration)) {
            valid = false;
            break;
        }
        bool complete = false;
        for (unsigned page = 0; page < 512 && !watchdog.revoked(); ++page) {
            WfpMemory<FWPM_FILTER0 *> entries;
            UINT32 count = 0;
            if (FwpmFilterEnum0(lease.engine.value, enumeration, 256, &entries.value, &count)) {
                break;
            }
            for (UINT32 index = 0; index < count; ++index) {
                const auto &filter = *entries.value[index];
                rememberKey(lease.watchedSublayers, filter.subLayerKey);
                if (filter.providerKey) {
                    rememberKey(lease.watchedProviders, *filter.providerKey);
                }
            }
            if (!count) {
                complete = true;
                break;
            }
        }
        if (FwpmFilterDestroyEnumHandle0(lease.engine.value, enumeration) || !complete) {
            valid = false;
            break;
        }
    }
    const auto closed = FwpmTransactionAbort0(lease.engine.value) == ERROR_SUCCESS;
    if (!valid || !closed || watchdog.revoked() || WaitForSingleObject(lease.changed.value, 0) != WAIT_TIMEOUT) {
        return false;
    }
    lease.metadataReady.store(true);
    return true;
}

bool sameProof(const PolicyProof &a, const PolicyProof &b)
{
    return a.state == b.state && IsEqualGUID(a.provider, b.provider) && IsEqualGUID(a.sublayer, b.sublayer) && a.tunnel.Value == b.tunnel.Value;
}

bool createLease(Lease &lease, const Client &client, const Endpoint &endpoint, PolicyProof &proof, LeaseWatchdog &watchdog)
{
    FWPM_SESSION0 session{};
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    session.txnWaitTimeoutInMSec = 3000;
    session.displayData.name = const_cast<wchar_t *>(L"privateCloud authenticated GUI lease");
    if (!lease.changed || watchdog.revoked() || !dynamic_lease_policy::createKeys(lease.objects)
        || FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &lease.engine.value)) {
        return false;
    }
    proof = inspectPolicy(lease.engine.value, endpoint.remote, lease.objects.sublayer);
    if (proof.state == PolicyState::Untrusted || watchdog.revoked()) {
        return false;
    }
    lease.endpoint = endpoint;
    lease.proof = proof;
    WfpMemory<FWP_BYTE_BLOB> app;
    if (FwpmGetAppIdFromFileName0(client.image.c_str(), &app.value)) {
        return false;
    }
    if (proof.state == PolicyState::RecognizedTailscale) {
        LocalMemory sidString, userSecurity;
        if (!ConvertSidToStringSidW(const_cast<BYTE *>(client.sid.data()), reinterpret_cast<LPWSTR *>(&sidString.value))) {
            return false;
        }
        const auto sddl = L"D:(A;;CC;;;" + std::wstring(static_cast<wchar_t *>(sidString.value)) + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
                                                                  SDDL_REVISION_1,
                                                                  reinterpret_cast<PSECURITY_DESCRIPTOR *>(&userSecurity.value),
                                                                  nullptr)) {
            return false;
        }
        const dynamic_lease_policy::Scope scope{app.value, userSecurity.value, endpoint.remote, endpoint.source, endpoint.luid.Value};
        if (!dynamic_lease_policy::install(lease.engine.value, lease.objects, scope, [&] {
                return watchdog.revoked();
            })) {
            return false;
        }
        lease.permits = true;
    }
    constexpr UINT32 flags = FWPM_SUBSCRIPTION_FLAG_NOTIFY_ON_ADD | FWPM_SUBSCRIPTION_FLAG_NOTIFY_ON_DELETE;
    size_t index = 0;
    for (const auto &layer : {FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4}) {
        TrafficFilterQuery query(endpoint, app.value, layer);
        FWPM_FILTER_SUBSCRIPTION0 filters{};
        filters.flags = flags;
        filters.enumTemplate = &query.value;
        if (watchdog.revoked() || FwpmFilterSubscribeChanges0(lease.engine.value, &filters, filterChanged, &lease, &lease.filterSubscriptions[index++])) {
            return false;
        }
        if (proof.state == PolicyState::RecognizedTailscale) {
            // Also retain DNS/service/tunnel rules used to establish the proof,
            // although some do not match this application's HTTPS traffic.
            FWPM_FILTER_ENUM_TEMPLATE0 tailscale{};
            tailscale.providerKey = &proof.provider;
            tailscale.layerKey = layer;
            tailscale.enumType = FWP_FILTER_ENUM_OVERLAPPING;
            tailscale.actionMask = 0xffffffff;
            tailscale.flags = FWP_FILTER_ENUM_FLAG_INCLUDE_DISABLED;
            filters.enumTemplate = &tailscale;
            if (FwpmFilterSubscribeChanges0(lease.engine.value, &filters, filterChanged, &lease, &lease.filterSubscriptions[index++])) {
                return false;
            }
        }
    }
    FWPM_SUBLAYER_SUBSCRIPTION0 layers{};
    layers.flags = flags;
    FWPM_PROVIDER_SUBSCRIPTION0 providers{};
    providers.flags = flags;
    if (watchdog.revoked() || FwpmSubLayerSubscribeChanges0(lease.engine.value, &layers, sublayerChanged, &lease, &lease.sublayerSubscription)
        || FwpmProviderSubscribeChanges0(lease.engine.value, &providers, providerChanged, &lease, &lease.providerSubscription)
        || NotifyRouteChange2(AF_INET, routeChanged, &lease, FALSE, &lease.routeNotification)
        || NotifyIpInterfaceChange(AF_INET, interfaceChanged, &lease, FALSE, &lease.interfaceNotification)
        || !captureWatchedMetadata(lease, app.value, watchdog)) {
        return false;
    }
    return !stopped() && !watchdog.revoked() && sameProof(proof, inspectPolicy(lease.engine.value, endpoint.remote, lease.objects.sublayer))
        && WaitForSingleObject(lease.changed.value, 0) == WAIT_TIMEOUT && !watchdog.revoked();
}

// One pending operation at a time. Cancellation is drained before the stack
// OVERLAPPED/event can be released, including service stop and process exit.
bool pipeIo(HANDLE pipe, HANDLE process, HANDLE changed, bool write, void *data, DWORD bytes, ULONGLONG deadline, LeaseWatchdog &watchdog)
{
    const auto allowed = [&] {
        return !watchdog.revoked() && !stopped() && GetTickCount64() < deadline && (!process || WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
            && (!changed || WaitForSingleObject(changed, 0) == WAIT_TIMEOUT);
    };
    if (!allowed()) {
        return false;
    }
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.value;
    DWORD transferred = 0;
    const BOOL completed = write ? WriteFile(pipe, data, bytes, &transferred, &overlapped) : ReadFile(pipe, data, bytes, &transferred, &overlapped);
    if (completed) {
        return transferred == bytes && allowed();
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    HANDLE handles[5] = {event.value, stopEvent.value, watchdog.revocationEvent()};
    auto count = DWORD{3};
    if (process) {
        handles[count++] = process;
    }
    if (changed) {
        handles[count++] = changed;
    }
    const auto now = GetTickCount64();
    const DWORD timeout = now >= deadline ? 0 : static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 120000));
    const DWORD waited = WaitForMultipleObjects(count, handles, FALSE, timeout);
    if (waited != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) && transferred == bytes && allowed();
}

bool readLine(HANDLE pipe, HANDLE process, HANDLE changed, std::string &line, ULONGLONG deadline, LeaseWatchdog &watchdog)
{
    line.clear();
    while (line.size() < 32 && !watchdog.revoked()) {
        char c = 0;
        if (!pipeIo(pipe, process, changed, false, &c, 1, deadline, watchdog)) {
            return false;
        }
        line += c;
        if (c == '\n') {
            return true;
        }
        if (c < 32 || c > 126) {
            return false;
        }
    }
    return false;
}

void replyError(HANDLE pipe, HANDLE process, const char *code, LeaseWatchdog &watchdog)
{
    auto reply = std::string("{\"version\":1,\"ok\":false,\"error\":\"") + code + "\"}\n";
    pipeIo(pipe, process, nullptr, true, reply.data(), static_cast<DWORD>(reply.size()), GetTickCount64() + ioDeadlineMs, watchdog);
}

std::string ipv4Text(IN_ADDR address)
{
    char text[INET_ADDRSTRLEN]{};
    return InetNtopA(AF_INET, &address, text, sizeof(text)) ? text : "";
}

std::string leaseReply(const Endpoint &endpoint, const std::string &id, bool permit, bool renewed)
{
    return "{\"version\":1,\"ok\":true,\"lease\":\"" + id + "\",\"family\":4,\"server\":\"" + ipv4Text(endpoint.remote)
        + "\",\"port\":443,\"interfaceIndex\":" + std::to_string(endpoint.index) + ",\"source\":\"" + ipv4Text(endpoint.source)
        + "\",\"expiresInSeconds\":120,\"permitInstalled\":" + (permit ? "true" : "false") + ",\"renewed\":" + (renewed ? "true" : "false") + "}\n";
}

void serveClientSession(HANDLE pipe, RevocationDiagnostic &diagnostic)
{
    // Reverse destruction order keeps the deadline active throughout native
    // subscription cancellation and dynamic-session closure, including errors.
    LeaseWatchdog watchdog(clientSetupDeadlineMs, stopEvent.value);
    if (!watchdog.ready() || watchdog.revoked()) {
        return;
    }
    Client client;
    if (!authenticate(pipe, client)) {
        replyError(pipe, nullptr, "client_not_authorized", watchdog);
        return;
    }
    if (!watchdog.observe(client.process.value)) {
        return;
    }
    std::string request;
    if (!readLine(pipe, client.process.value, nullptr, request, GetTickCount64() + ioDeadlineMs, watchdog) || request != initialRequest) {
        return;
    }
    Endpoint endpoint;
    if (!chooseEndpoint(endpoint, watchdog)) {
        replyError(pipe, client.process.value, "physical_ipv4_unavailable", watchdog);
        return;
    }
    GUID id{};
    RPC_CSTR text = nullptr;
    if (UuidCreate(&id) != RPC_S_OK || UuidToStringA(&id, &text) != RPC_S_OK) {
        return;
    }
    const std::string leaseId(reinterpret_cast<char *>(text));
    RpcStringFreeA(&text);
    diagnostic.clientPid = client.pid;
    diagnostic.interfaceIndex = endpoint.index;
    Lease lease;
    lease.diagnostic = &diagnostic;
    if (!lease.changed || !watchdog.observe(lease.changed.value)) {
        return;
    }
    PolicyProof proof{};
    if (!createLease(lease, client, endpoint, proof, watchdog)) {
        replyError(pipe, client.process.value, "policy_unavailable", watchdog);
        return;
    }
    auto renewed = GetTickCount64();
    if (!watchdog.renew(leaseMs)) {
        return;
    }
    auto reply = leaseReply(endpoint, leaseId, lease.permits, false);
    if (!pipeIo(pipe,
                client.process.value,
                lease.changed.value,
                true,
                reply.data(),
                static_cast<DWORD>(reply.size()),
                GetTickCount64() + ioDeadlineMs,
                watchdog)) {
        return;
    }
    while (!stopped() && !watchdog.revoked()) {
        if (!readLine(pipe, client.process.value, lease.changed.value, request, renewed + leaseMs, watchdog) || request != renewalRequest
            || GetTickCount64() - renewed < 5000) {
            return;
        }
        Endpoint current;
        if (!chooseEndpoint(current, watchdog) || !(current == endpoint)
            || !sameProof(proof, inspectPolicy(lease.engine.value, endpoint.remote, lease.objects.sublayer))
            || WaitForSingleObject(client.process.value, 0) != WAIT_TIMEOUT || WaitForSingleObject(lease.changed.value, 0) != WAIT_TIMEOUT
            || GetTickCount64() >= renewed + leaseMs || watchdog.revoked()) {
            diagnostic.record("endpoint-policy-or-lifetime-revalidation-failed");
            return;
        }
        renewed = GetTickCount64();
        if (!watchdog.renew(leaseMs)) {
            return;
        }
        reply = leaseReply(endpoint, leaseId, lease.permits, true);
        if (!pipeIo(pipe,
                    client.process.value,
                    lease.changed.value,
                    true,
                    reply.data(),
                    static_cast<DWORD>(reply.size()),
                    GetTickCount64() + ioDeadlineMs,
                    watchdog)) {
            return;
        }
    }
}

void serveClient(HANDLE pipe)
{
    RevocationDiagnostic diagnostic;
    serveClientSession(pipe, diagnostic);
    // The dynamic session and watchdog have already been drained. Event Log
    // delivery must never delay removal of an invalid firewall exception.
    if (const auto reason = diagnostic.reason.load()) {
        const auto source = RegisterEventSourceA(nullptr, "privateCloudDirectBroker");
        if (source) {
            char message[256]{};
            std::snprintf(message,
                          sizeof(message),
                          "privateCloud connection revoked: %s; clientPid=%lu; interfaceIndex=%lu",
                          reason,
                          diagnostic.clientPid,
                          diagnostic.interfaceIndex);
            const char *messages[] = {message};
            ReportEventA(source, EVENTLOG_WARNING_TYPE, 0, 1001, nullptr, 1, 0, messages, nullptr);
            DeregisterEventSource(source);
        }
    }
}

DWORD runService()
{
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) {
        return ERROR_SERVICE_SPECIFIC_ERROR;
    }
    LocalMemory security;
    // CreateFile also needs read-attributes admission even when its caller requests
    // only data/synchronization access. Pipe creation/control rights remain excluded.
    const auto sid = brokerSid();
    if (sid.empty()) {
        WSACleanup();
        return ERROR_NO_SUCH_USER;
    }
    const auto pipeSddl = broker_pipe_lifecycle::securityDescriptor(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(pipeSddl.c_str(),
                                                              SDDL_REVISION_1,
                                                              reinterpret_cast<PSECURITY_DESCRIPTOR *>(&security.value),
                                                              nullptr)) {
        WSACleanup();
        return GetLastError();
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.value, FALSE};
    DWORD result = NO_ERROR;
    broker_pipe_lifecycle::Server pipe(pipeName, &attributes);
    if (!pipe.valid()) {
        result = pipe.error();
        WSACleanup();
        return result;
    }
    while (!stopped()) {
        if (!pipe.accept(stopEvent.value, INFINITE)) {
            result = stopped() ? ERROR_SUCCESS : pipe.error();
            break;
        }
        if (!stopped()) {
            serveClient(pipe.handle());
        }
        CancelIoEx(pipe.handle(), nullptr);
        if (!pipe.disconnect()) {
            result = pipe.error();
            break;
        }
    }
    WSACleanup();
    return result;
}

void WINAPI serviceMain(DWORD, LPWSTR *)
{
    stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    statusHandle = RegisterServiceCtrlHandlerExW(serviceName, controlHandler, nullptr);
    if (!statusHandle) {
        return;
    }
    if (!stopEvent) {
        reportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }
    reportStatus(SERVICE_START_PENDING);
    DWORD result = ERROR_SERVICE_SPECIFIC_ERROR;
    try {
        const auto base = programFilesPath();
        const auto product = base + L"\\privateCloud";
        const auto expected = product + L"\\privateCloudDirectBroker.exe";
        if (base.empty() || _wcsicmp(processPath(GetCurrentProcess()).c_str(), expected.c_str()) != 0 || !protectedPath(base, true)
            || !protectedPath(product, true) || !protectedPath(expected, false)) {
            result = ERROR_ACCESS_DENIED;
        } else {
            result = broker_process_security::prepare();
            if (result == ERROR_SUCCESS) {
                reportStatus(SERVICE_RUNNING);
                result = runService();
            }
        }
    } catch (...) {
        result = ERROR_UNHANDLED_EXCEPTION;
    }
    reportStatus(SERVICE_STOPPED, result);
}

int selfTest()
{
    int failed = 0;
    const auto check = [&](bool okay, const char *name) {
        std::printf("%s %s\n", okay ? "PASS" : "FAIL", name);
        if (!okay) {
            ++failed;
        }
    };
    for (const char *value : {"0.1.2.3",
                              "10.2.3.4",
                              "100.64.0.1",
                              "100.127.255.254",
                              "127.0.0.1",
                              "169.254.2.3",
                              "172.16.0.1",
                              "172.31.255.1",
                              "192.168.1.1",
                              "192.0.2.1",
                              "198.18.0.1",
                              "198.51.100.1",
                              "203.0.113.1",
                              "224.0.0.1",
                              "255.255.255.255"}) {
        IN_ADDR address{};
        InetPtonA(AF_INET, value, &address);
        check(!publicV4(address), value);
    }
    for (const char *value : {"1.1.1.1", "8.8.8.8", "100.63.255.254", "100.128.0.1", "172.15.0.1", "172.32.0.1", "192.0.3.1", "198.51.99.1", "203.0.112.1"}) {
        IN_ADDR address{};
        InetPtonA(AF_INET, value, &address);
        check(publicV4(address), value);
    }
    check(std::string(initialRequest) != "PRIVATECLOUD-DIRECT/1\r\n", "no alternate protocol spelling");
    Endpoint endpoint{};
    InetPtonA(AF_INET, "1.1.1.1", &endpoint.remote);
    InetPtonA(AF_INET, "192.168.1.2", &endpoint.source);
    endpoint.index = 8;
    const auto reply = leaseReply(endpoint, "fixed-test-id", true, false);
    check(reply.size() < 4096 && reply.back() == '\n' && reply.find("\"expiresInSeconds\":120") != std::string::npos, "bounded reply and fixed lease");
    std::puts("Pure tests only: no WFP session, service install, DNS, socket connection, or network mutation.");
    return failed ? 1 : 0;
}

// Diagnostic only: DNS and a read-only WFP snapshot. This mode never creates
// providers, sublayers, filters, routes, sockets, registry entries or services.
int inspectCurrentPolicy(FILE *output)
{
    WSADATA data{};
    const auto startup = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup) {
        return startup;
    }
    int resolverError = 0;
    const auto addresses = resolveServer(&resolverError);
    if (addresses.empty()) {
        std::fprintf(output,
                     "{\"state\":\"Untrusted\",\"reason\":\"%s\",\"winsockError\":%d}\n",
                     resolverError ? "fixed-server-dns-unavailable" : "fixed-server-no-public-ipv4",
                     resolverError);
        WSACleanup();
        return 2;
    }
    int result = 0;
    {
        Engine engine;
        FWPM_SESSION0 session{};
        session.txnWaitTimeoutInMSec = 3000;
        session.displayData.name = const_cast<wchar_t *>(L"privateCloud read-only policy diagnostic");
        const auto status = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr, &session, &engine.value);
        if (status) {
            std::fprintf(output, "{\"state\":\"Untrusted\",\"reason\":\"wfp-read-access-unavailable\",\"win32Error\":%lu}\n", status);
            result = 2;
        } else {
            for (const auto address : addresses) {
                const auto proof = inspectPolicy(engine.value, address, GUID{});
                const auto *state = proof.state == PolicyState::NoBypassNeeded ? "NoBypassNeeded"
                    : proof.state == PolicyState::RecognizedTailscale          ? "RecognizedTailscale"
                                                                               : "Untrusted";
                const auto server = ipv4Text(address);
                std::fprintf(output,
                             "{\"server\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\",\"stage\":\"%s\",\"nativeError\":%lu,\"tunnelLuid\":%llu}\n",
                             server.c_str(),
                             state,
                             proof.reason,
                             proof.stage,
                             proof.nativeError,
                             proof.tunnel.Value);
                if (proof.state == PolicyState::Untrusted) {
                    result = 2;
                }
            }
        }
    }
    WSACleanup();
    return result;
}

int inspectPolicyToFile(const wchar_t *path)
{
    // CREATE_NEW prevents an elevated diagnostic from truncating an existing
    // file or following a pre-existing file symlink. The caller chooses a new
    // report path; no service command accepts a path from the pipe client.
    const auto file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return static_cast<int>(GetLastError());
    }
    const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_WRONLY | _O_BINARY);
    if (descriptor == -1) {
        CloseHandle(file);
        return ERROR_OPEN_FAILED;
    }
    auto *output = _fdopen(descriptor, "wb");
    if (!output) {
        _close(descriptor);
        return ERROR_OPEN_FAILED;
    }
    auto result = inspectCurrentPolicy(output);
    if (std::fclose(output) != 0) {
        result = ERROR_WRITE_FAULT;
    }
    return result;
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2 && wcscmp(argv[1], L"--self-test") == 0) {
        return selfTest();
    }
    if (argc == 2 && wcscmp(argv[1], L"--inspect-policy") == 0) {
        return inspectCurrentPolicy(stdout);
    }
    if (argc == 4 && wcscmp(argv[1], L"--inspect-policy") == 0 && wcscmp(argv[2], L"--output") == 0) {
        return inspectPolicyToFile(argv[3]);
    }
    if (argc != 1 && !(argc == 2 && wcscmp(argv[1], L"--service") == 0)) {
        return ERROR_INVALID_PARAMETER;
    }
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t *>(serviceName), serviceMain}, {nullptr, nullptr}};
    return StartServiceCtrlDispatcherW(table) ? 0 : static_cast<int>(GetLastError());
}
