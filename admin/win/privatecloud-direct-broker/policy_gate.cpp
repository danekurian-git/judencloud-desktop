// Read-only recognition of the specific Tailscale exit-node policy.
#include "policy_gate_detail.h"
#include <fwpmu.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <tlhelp32.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <string>
#include <memory>

namespace policy_detail
{
bool assessAncestry(const std::vector<ProcessIdentity> &chain, DWORD processPid, DWORD servicePid, InspectionFailure &failure)
{
    constexpr auto stage = "tailscale-firewall-ancestry";
    if (!processPid || !servicePid || chain.empty()) {
        return failure.record(stage, "process-ancestry-missing");
    }
    if (chain.size() > maximumAncestryProcesses || chain.front().pid != processPid || chain.back().pid != servicePid) {
        return failure.record(stage, "process-ancestry-outside-service-boundary");
    }
    for (size_t index = 0; index < chain.size(); ++index) {
        const auto &node = chain[index];
        if (!node.alive || !node.imageMatches || !node.systemUser) {
            return failure.record(stage, "process-ancestry-identity-untrusted");
        }
        if (!node.created || node.created != node.recheckedCreated || node.parent != node.recheckedParent) {
            return failure.record(stage, "process-ancestry-identity-changed");
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (chain[previous].pid == node.pid) {
                return failure.record(stage, "process-ancestry-cycle");
            }
        }
        if (index + 1 < chain.size()) {
            const auto &parent = chain[index + 1];
            if (node.parent != parent.pid || parent.created > node.created) {
                return failure.record(stage, "process-ancestry-parent-reused-or-mismatched");
            }
        }
    }
    return true;
}

bool sameAncestry(const std::vector<ProcessIdentity> &before, const std::vector<ProcessIdentity> &after)
{
    if (before.size() != after.size()) {
        return false;
    }
    for (size_t index = 0; index < before.size(); ++index) {
        if (before[index].pid != after[index].pid || before[index].parent != after[index].parent || before[index].created != after[index].created) {
            return false;
        }
    }
    return true;
}

bool collectSnapshot(bool &hasTailscale, const SnapshotOperations &operations)
{
    operations.captureIdentity();
    if (!operations.beginRead()) {
        return false;
    }
    bool captured = false;
    try {
        captured = operations.captureRules();
    } catch (...) {
        operations.endRead();
        throw;
    }
    const auto ended = operations.endRead();
    if (!captured || !ended) {
        return false;
    }
    return !hasTailscale || operations.identityUnchanged();
}

PolicyProof assess(const Facts &facts)
{
    PolicyProof result;
    auto fail = [&](const char *why) {
        result.reason = why;
        return result;
    };
    if (!facts.complete || !facts.routeUp || !facts.routeLuid) {
        return fail("policy-snapshot-incomplete");
    }
    if (!facts.hasTailscale) {
        if (!facts.routeHardware) {
            return fail("unrecognized-virtual-route");
        }
        result.state = PolicyState::NoBypassNeeded;
        result.reason = "physical-route-no-tailscale-policy";
        return result;
    }
    if (!facts.serviceTrusted || !facts.sessionTrusted || !facts.metadataTrusted) {
        return fail("tailscale-identity-unverified");
    }
    if (facts.routeHardware) {
        return fail("tailscale-policy-without-exit-route");
    }
    std::array<unsigned, 2> blocks{}, service{}, tunnel{}, loopback{}, dns{};
    for (const auto &rule : facts.rules) {
        // The administrator-approved exception may override other providers'
        // lower-priority policy for the exact privateCloud connection. WFP
        // retains arbitration; only Tailscale and our own lease are recognized.
        if (!rule.tailscale && !rule.own) {
            continue;
        }
        if (rule.layer > 1 || !rule.flagsSupported) {
            return fail("unsupported-filter-layout");
        }
        if (rule.own) {
            if (rule.action != Action::Permit) {
                return fail("unexpected-own-filter");
            }
            continue;
        }
        if (rule.sublayerWeight >= privateCloudPermitWeight) {
            return fail("tailscale-priority-unexpected");
        }
        if (rule.action == Action::Block) {
            if (rule.shape != Shape::CatchAll || rule.filterWeight != 0) {
                return fail("tailscale-block-shape-changed");
            }
            ++blocks[rule.layer];
        } else if (rule.action == Action::Permit) {
            if (rule.filterWeight == 12) {
                continue;
            }
            if (rule.filterWeight != 15) {
                return fail("tailscale-permit-weight-changed");
            }
            switch (rule.shape) {
            case Shape::Service:
                ++service[rule.layer];
                break;
            case Shape::Tunnel:
                if (rule.tunnel != facts.routeLuid) {
                    return fail("tailscale-route-luid-mismatch");
                }
                ++tunnel[rule.layer];
                break;
            case Shape::Loopback:
                ++loopback[rule.layer];
                break;
            case Shape::Dns:
                ++dns[rule.layer];
                break;
            default:
                return fail("tailscale-permit-shape-changed");
            }
        } else {
            return fail("tailscale-callout-unexpected");
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (blocks[i] != 1 || service[i] != 1 || tunnel[i] != 1 || loopback[i] != 1 || dns[i] != 1) {
            return fail("tailscale-required-rules-missing-or-duplicated");
        }
    }
    result.state = PolicyState::RecognizedTailscale;
    result.provider = facts.provider;
    result.sublayer = facts.sublayer;
    result.tunnel.Value = facts.routeLuid;
    result.reason = "recognized-tailscale-exit-policy";
    return result;
}
}

namespace
{
using policy_detail::InspectionFailure;
struct Handle {
    HANDLE value = nullptr;
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};
struct Service {
    SC_HANDLE value = nullptr;
    ~Service()
    {
        if (value) {
            CloseServiceHandle(value);
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
struct LocalMemory {
    void *value = nullptr;
    ~LocalMemory()
    {
        if (value) {
            LocalFree(value);
        }
    }
};
bool named(const wchar_t *value, const wchar_t *expected)
{
    return value && wcscmp(value, expected) == 0;
}
bool trustedOwner(PSID sid)
{
    if (!sid || !IsValidSid(sid)) {
        return false;
    }
    for (const auto type : {WinLocalSystemSid, WinBuiltinAdministratorsSid}) {
        BYTE bytes[SECURITY_MAX_SID_SIZE]{};
        DWORD size = sizeof(bytes);
        if (CreateWellKnownSid(type, nullptr, bytes, &size) && EqualSid(sid, bytes)) {
            return true;
        }
    }
    LocalMemory installer;
    return ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &installer.value) && EqualSid(sid, installer.value);
}
bool protectedPath(const std::wstring &path, bool directory, InspectionFailure &failure, const char *stage)
{
    Handle file{CreateFileW(path.c_str(),
                            READ_CONTROL | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr,
                            OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                            nullptr)};
    if (!file.value || file.value == INVALID_HANDLE_VALUE) {
        return failure.record(stage, "protected-path-open-failed", GetLastError());
    }
    FILE_ATTRIBUTE_TAG_INFO attrs{};
    if (!GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &attrs, sizeof(attrs))) {
        return failure.record(stage, "protected-path-attributes-unavailable", GetLastError());
    }
    if ((attrs.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || bool(attrs.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) {
        return failure.record(stage, "protected-path-type-or-reparse-unexpected");
    }
    LocalMemory security;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    const auto securityStatus = GetSecurityInfo(file.value,
                                                SE_FILE_OBJECT,
                                                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                                &owner,
                                                nullptr,
                                                &dacl,
                                                nullptr,
                                                reinterpret_cast<PSECURITY_DESCRIPTOR *>(&security.value));
    if (securityStatus) {
        return failure.record(stage, "protected-path-security-unavailable", securityStatus);
    }
    if (!trustedOwner(owner)) {
        return failure.record(stage, "protected-path-owner-untrusted");
    }
    if (!dacl) {
        return failure.record(stage, "protected-path-dacl-missing");
    }
    constexpr ACCESS_MASK writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC
        | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER *header = nullptr;
        if (!GetAce(dacl, i, reinterpret_cast<void **>(&header))) {
            return failure.record(stage, "protected-path-ace-unavailable", GetLastError());
        }
        if (header->AceFlags & INHERIT_ONLY_ACE || header->AceType == ACCESS_DENIED_ACE_TYPE) {
            continue;
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            return failure.record(stage, "protected-path-ace-type-unsupported");
        }
        const auto *allow = reinterpret_cast<const ACCESS_ALLOWED_ACE *>(header);
        if ((allow->Mask & writes) && !trustedOwner(const_cast<DWORD *>(&allow->SidStart))) {
            return failure.record(stage, "protected-path-write-ace-untrusted");
        }
    }
    return true;
}
bool signedByTailscale(const std::wstring &path, InspectionFailure &failure)
{
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &trust);
    bool valid = false;
    if (status == ERROR_SUCCESS) {
        auto *provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto *signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        auto *cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        wchar_t organization[128]{};
        if (cert && cert->pCert
            && CertGetNameStringW(cert->pCert,
                                  CERT_NAME_ATTR_TYPE,
                                  0,
                                  const_cast<char *>(szOID_ORGANIZATION_NAME),
                                  organization,
                                  static_cast<DWORD>(std::size(organization)))) {
            valid = wcscmp(organization, L"Tailscale Inc.") == 0 || wcscmp(organization, L"Tailscale Inc") == 0;
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &trust);
    if (status != ERROR_SUCCESS) {
        return failure.record("tailscale-signature", "signature-verification-failed", static_cast<DWORD>(status));
    }
    return valid || failure.record("tailscale-signature", "signature-organization-untrusted");
}
bool processHandleIsServiceImage(HANDLE process, const std::wstring &path, InspectionFailure &failure, const char *stage)
{
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
        return failure.record(stage, "process-not-alive");
    }
    std::array<wchar_t, 32768> image{};
    auto length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &length)) {
        return failure.record(stage, "process-image-query-failed", GetLastError());
    }
    if (_wcsicmp(image.data(), path.c_str())) {
        return failure.record(stage, "process-image-path-unexpected");
    }
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) {
        return failure.record(stage, "process-token-open-failed", GetLastError());
    }
    std::array<BYTE, 512> bytes{};
    DWORD needed = 0;
    if (!GetTokenInformation(token.value, TokenUser, bytes.data(), static_cast<DWORD>(bytes.size()), &needed)) {
        return failure.record(stage, "process-token-query-failed", GetLastError());
    }
    return IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, WinLocalSystemSid) != FALSE
        || failure.record(stage, "process-user-is-not-system");
}
bool processIsServiceImage(DWORD pid, const std::wstring &path, InspectionFailure &failure, const char *stage)
{
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
    if (!process.value) {
        return failure.record(stage, "process-open-failed", GetLastError());
    }
    return processHandleIsServiceImage(process.value, path, failure, stage);
}
bool trustedTailscaleService(std::wstring &path, DWORD &pid, InspectionFailure &failure)
{
    PWSTR base = nullptr;
    const auto knownFolderStatus = SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &base);
    if (FAILED(knownFolderStatus)) {
        return failure.record("tailscale-program-files", "program-files-path-unavailable", static_cast<DWORD>(knownFolderStatus));
    }
    const std::wstring directory = std::wstring(base) + L"\\Tailscale";
    const bool baseProtected = protectedPath(base, true, failure, "tailscale-program-files");
    CoTaskMemFree(base);
    path = directory + L"\\tailscaled.exe";
    if (!baseProtected || !protectedPath(directory, true, failure, "tailscale-install-directory")
        || !protectedPath(path, false, failure, "tailscale-service-image")) {
        return false;
    }
    Service scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.value) {
        return failure.record("tailscale-service-control", "service-manager-open-failed", GetLastError());
    }
    Service service{OpenServiceW(scm.value, L"Tailscale", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)};
    if (!service.value) {
        return failure.record("tailscale-service-control", "tailscale-service-open-failed", GetLastError());
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD size = 0;
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&status), sizeof(status), &size)) {
        return failure.record("tailscale-service-control", "tailscale-service-status-unavailable", GetLastError());
    }
    if (status.dwCurrentState != SERVICE_RUNNING || !status.dwProcessId) {
        return failure.record("tailscale-service-control", "tailscale-service-not-running");
    }
    pid = status.dwProcessId;
    return processIsServiceImage(pid, path, failure, "tailscale-service-process") && signedByTailscale(path, failure);
}
struct ParentLink {
    DWORD pid = 0;
    DWORD parent = 0;
};
constexpr size_t maximumProcessSnapshotEntries = 65536;
bool readParents(std::vector<ParentLink> &parents, InspectionFailure &failure)
{
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.value == INVALID_HANDLE_VALUE) {
        return failure.record("tailscale-firewall-ancestry", "process-snapshot-open-failed", GetLastError());
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry)) {
        return failure.record("tailscale-firewall-ancestry", "process-snapshot-read-failed", GetLastError());
    }
    do {
        if (parents.size() >= maximumProcessSnapshotEntries) {
            return failure.record("tailscale-firewall-ancestry", "process-snapshot-limit-exceeded");
        }
        parents.push_back({entry.th32ProcessID, entry.th32ParentProcessID});
    } while (Process32NextW(snapshot.value, &entry));
    const auto status = GetLastError();
    return status == ERROR_NO_MORE_FILES || failure.record("tailscale-firewall-ancestry", "process-snapshot-incomplete", status);
}

bool processCreation(HANDLE process, UINT64 &created, InspectionFailure &failure)
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return failure.record("tailscale-firewall-ancestry", "process-creation-time-unavailable", GetLastError());
    }
    created = (static_cast<UINT64>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    return true;
}

struct PinnedAncestry {
    std::vector<std::unique_ptr<Handle>> handles;
    std::vector<policy_detail::ProcessIdentity> identities;

    bool alive() const
    {
        return !handles.empty() && std::all_of(handles.begin(), handles.end(), [](const auto &handle) {
            return WaitForSingleObject(handle->value, 0) == WAIT_TIMEOUT;
        });
    }
};

bool authenticateAncestry(DWORD processPid, DWORD servicePid, const std::wstring &image, PinnedAncestry &ancestry, InspectionFailure &failure)
{
    std::vector<ParentLink> parents;
    if (!readParents(parents, failure)) {
        return false;
    }
    auto pid = processPid;
    for (size_t depth = 0; depth < policy_detail::maximumAncestryProcesses; ++depth) {
        const auto parent = std::find_if(parents.begin(), parents.end(), [pid](const auto &entry) {
            return entry.pid == pid;
        });
        if (!pid || parent == parents.end()) {
            return failure.record("tailscale-firewall-ancestry", "process-ancestry-node-missing");
        }
        if (std::any_of(ancestry.identities.begin(), ancestry.identities.end(), [pid](const auto &entry) {
                return entry.pid == pid;
            })) {
            return failure.record("tailscale-firewall-ancestry", "process-ancestry-cycle");
        }
        auto handle = std::make_unique<Handle>();
        handle->value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!handle->value) {
            return failure.record("tailscale-firewall-ancestry", "process-ancestry-open-failed", GetLastError());
        }
        if (!processHandleIsServiceImage(handle->value, image, failure, "tailscale-firewall-ancestry")) {
            return false;
        }
        policy_detail::ProcessIdentity identity;
        identity.pid = pid;
        identity.parent = parent->parent;
        identity.imageMatches = identity.systemUser = true;
        if (!processCreation(handle->value, identity.created, failure)) {
            return false;
        }
        ancestry.identities.push_back(identity);
        ancestry.handles.push_back(std::move(handle));
        if (pid == servicePid) {
            break;
        }
        pid = parent->parent;
    }
    // With all candidate process objects pinned, take a second parent snapshot.
    // Reused PIDs, changed parents and terminated objects must not bridge a gap.
    std::vector<ParentLink> currentParents;
    if (!readParents(currentParents, failure)) {
        return false;
    }
    for (size_t index = 0; index < ancestry.identities.size(); ++index) {
        auto &identity = ancestry.identities[index];
        const auto current = std::find_if(currentParents.begin(), currentParents.end(), [&](const auto &entry) {
            return entry.pid == identity.pid;
        });
        if (current == currentParents.end()) {
            return failure.record("tailscale-firewall-ancestry", "process-ancestry-node-disappeared");
        }
        identity.recheckedParent = current->parent;
        identity.alive = WaitForSingleObject(ancestry.handles[index]->value, 0) == WAIT_TIMEOUT;
        if (!processCreation(ancestry.handles[index]->value, identity.recheckedCreated, failure)) {
            return false;
        }
    }
    return policy_detail::assessAncestry(ancestry.identities, processPid, servicePid, failure);
}
bool trustedFirewallSession(HANDLE engine,
                            DWORD servicePid,
                            const std::wstring &image,
                            GUID &sessionKey,
                            DWORD &sessionPid,
                            PinnedAncestry &ancestry,
                            InspectionFailure &failure)
{
    HANDLE enumeration = nullptr;
    const auto createStatus = FwpmSessionCreateEnumHandle0(engine, nullptr, &enumeration);
    if (createStatus) {
        return failure.record("tailscale-session-enumeration", "session-enumerator-create-failed", createStatus);
    }
    bool complete = false, trusted = false, invalid = false;
    unsigned matches = 0;
    for (unsigned page = 0; page < 32; ++page) {
        WfpMemory<FWPM_SESSION0 *> entries;
        UINT32 count = 0;
        const auto enumStatus = FwpmSessionEnum0(engine, enumeration, 128, &entries.value, &count);
        if (enumStatus) {
            failure.record("tailscale-session-enumeration", "session-enumeration-failed", enumStatus);
            break;
        }
        for (UINT32 i = 0; i < count; ++i) {
            const auto &entry = *entries.value[i];
            if (!named(entry.displayData.name, L"Tailscale firewall")) {
                continue;
            }
            ++matches;
            bool valid = true;
            if (entry.flags != FWPM_SESSION_FLAG_DYNAMIC || entry.kernelMode) {
                valid = failure.record("tailscale-firewall-session", "session-flags-unexpected");
            } else if (!entry.sid || !IsWellKnownSid(entry.sid, WinLocalSystemSid)) {
                valid = failure.record("tailscale-firewall-session", "session-user-is-not-system");
            } else {
                PinnedAncestry candidate;
                valid = authenticateAncestry(entry.processId, servicePid, image, candidate, failure);
                if (valid) {
                    ancestry = std::move(candidate);
                }
            }
            trusted |= valid;
            invalid |= !valid;
            if (valid) {
                sessionKey = entry.sessionKey;
                sessionPid = entry.processId;
            }
        }
        if (count < 128) {
            complete = true;
            break;
        }
    }
    FwpmSessionDestroyEnumHandle0(engine, enumeration);
    if (!complete) {
        return failure.record("tailscale-session-enumeration", "session-enumeration-incomplete");
    }
    if (matches != 1) {
        return failure.record("tailscale-firewall-session", matches ? "session-name-duplicated" : "session-name-not-found");
    }
    return (!invalid && trusted) || failure.record("tailscale-firewall-session", "session-identity-unverified");
}
bool bytesEqual(const FWP_BYTE_BLOB *a, const FWP_BYTE_BLOB *b)
{
    return a && b && a->size == b->size && a->size && a->data && b->data && memcmp(a->data, b->data, a->size) == 0;
}
policy_detail::Shape classifyShape(const FWPM_FILTER0 &filter, const FWP_BYTE_BLOB *serviceApp, UINT64 &luid)
{
    using policy_detail::Shape;
    if (!filter.numFilterConditions) {
        return Shape::CatchAll;
    }
    if (!filter.filterCondition) {
        return Shape::Other;
    }
    if (filter.numFilterConditions == 1) {
        const auto &c = filter.filterCondition[0];
        if (IsEqualGUID(c.fieldKey, FWPM_CONDITION_ALE_APP_ID) && c.matchType == FWP_MATCH_EQUAL && c.conditionValue.type == FWP_BYTE_BLOB_TYPE
            && bytesEqual(c.conditionValue.byteBlob, serviceApp)) {
            return Shape::Service;
        }
        if (IsEqualGUID(c.fieldKey, FWPM_CONDITION_IP_LOCAL_INTERFACE) && c.matchType == FWP_MATCH_EQUAL && c.conditionValue.type == FWP_UINT64
            && c.conditionValue.uint64) {
            luid = *c.conditionValue.uint64;
            return Shape::Tunnel;
        }
        if (IsEqualGUID(c.fieldKey, FWPM_CONDITION_FLAGS) && c.matchType == FWP_MATCH_FLAGS_ALL_SET && c.conditionValue.type == FWP_UINT32
            && c.conditionValue.uint32 == FWP_CONDITION_FLAG_IS_LOOPBACK) {
            return Shape::Loopback;
        }
    }
    if (filter.numFilterConditions == 3) {
        unsigned port = 0, tcp = 0, udp = 0;
        for (UINT32 i = 0; i < 3; ++i) {
            const auto &c = filter.filterCondition[i];
            if (c.matchType != FWP_MATCH_EQUAL) {
                return Shape::Other;
            }
            if (IsEqualGUID(c.fieldKey, FWPM_CONDITION_IP_REMOTE_PORT) && c.conditionValue.type == FWP_UINT16 && c.conditionValue.uint16 == 53) {
                ++port;
            } else if (IsEqualGUID(c.fieldKey, FWPM_CONDITION_IP_PROTOCOL) && c.conditionValue.type == FWP_UINT8) {
                tcp += c.conditionValue.uint8 == IPPROTO_TCP;
                udp += c.conditionValue.uint8 == IPPROTO_UDP;
            } else {
                return Shape::Other;
            }
        }
        if (port == 1 && tcp == 1 && udp == 1) {
            return Shape::Dns;
        }
    }
    return Shape::Other;
}
bool definitelyDisjoint(const FWPM_FILTER0 &filter, IN_ADDR server)
{
    if (filter.numFilterConditions && !filter.filterCondition) {
        return false;
    }
    // Repeated fields are OR. Prove all alternatives of one field disjoint.
    for (const GUID *field : {&FWPM_CONDITION_IP_PROTOCOL, &FWPM_CONDITION_IP_REMOTE_PORT, &FWPM_CONDITION_IP_REMOTE_ADDRESS}) {
        bool found = false, allDisjoint = true;
        for (UINT32 i = 0; i < filter.numFilterConditions; ++i) {
            const auto &c = filter.filterCondition[i];
            if (!IsEqualGUID(c.fieldKey, *field)) {
                continue;
            }
            found = true;
            bool disjoint = false;
            if (c.matchType == FWP_MATCH_EQUAL) {
                if (IsEqualGUID(*field, FWPM_CONDITION_IP_PROTOCOL) && c.conditionValue.type == FWP_UINT8) {
                    disjoint = c.conditionValue.uint8 != IPPROTO_TCP;
                }
                if (IsEqualGUID(*field, FWPM_CONDITION_IP_REMOTE_PORT) && c.conditionValue.type == FWP_UINT16) {
                    disjoint = c.conditionValue.uint16 != 443;
                }
                if (IsEqualGUID(*field, FWPM_CONDITION_IP_REMOTE_ADDRESS)) {
                    if (c.conditionValue.type == FWP_UINT32) {
                        disjoint = c.conditionValue.uint32 != ntohl(server.S_un.S_addr);
                    }
                    if (c.conditionValue.type == FWP_V4_ADDR_MASK && c.conditionValue.v4AddrMask) {
                        const auto &v = *c.conditionValue.v4AddrMask;
                        disjoint = (ntohl(server.S_un.S_addr) & v.mask) != (v.addr & v.mask);
                    }
                }
            }
            allDisjoint &= disjoint;
        }
        if (found && allDisjoint) {
            return true;
        }
    }
    return false;
}
struct Sublayer {
    GUID key{};
    UINT16 weight = 0;
    bool tailscale = false;
};
bool inspectLayers(HANDLE engine, policy_detail::Facts &facts, std::vector<Sublayer> &layers, InspectionFailure &failure)
{
    HANDLE enumeration = nullptr;
    const auto createStatus = FwpmSubLayerCreateEnumHandle0(engine, nullptr, &enumeration);
    if (createStatus) {
        return failure.record("sublayer-enumeration", "sublayer-enumerator-create-failed", createStatus);
    }
    bool complete = false, valid = true;
    unsigned countTailscale = 0;
    for (unsigned page = 0; page < 32; ++page) {
        WfpMemory<FWPM_SUBLAYER0 *> entries;
        UINT32 count = 0;
        const auto enumStatus = FwpmSubLayerEnum0(engine, enumeration, 128, &entries.value, &count);
        if (enumStatus) {
            failure.record("sublayer-enumeration", "sublayer-enumeration-failed", enumStatus);
            break;
        }
        for (UINT32 i = 0; i < count; ++i) {
            const auto &layer = *entries.value[i];
            const bool ts = named(layer.displayData.name, L"Tailscale permissive and blocking filters");
            layers.push_back({layer.subLayerKey, layer.weight, ts});
            if (ts) {
                ++countTailscale;
                facts.sublayer = layer.subLayerKey;
                const auto metadataValid = !layer.providerKey && !layer.flags && !layer.providerData.size && layer.weight < 256;
                valid &= metadataValid;
                if (!metadataValid) {
                    failure.record("tailscale-sublayer-metadata",
                                   layer.providerKey             ? "sublayer-provider-unexpected"
                                       : layer.flags             ? "sublayer-flags-unexpected"
                                       : layer.providerData.size ? "sublayer-data-unexpected"
                                                                 : "sublayer-weight-unexpected");
                }
            }
        }
        if (count < 128) {
            complete = true;
            break;
        }
    }
    FwpmSubLayerDestroyEnumHandle0(engine, enumeration);
    facts.hasTailscale = countTailscale != 0;
    facts.metadataTrusted = valid && countTailscale == 1;
    if (!complete) {
        return failure.record("sublayer-enumeration", "sublayer-enumeration-incomplete");
    }
    if (countTailscale > 1) {
        return failure.record("tailscale-sublayer-metadata", "tailscale-sublayer-duplicated");
    }
    return valid;
}
bool inspectFilters(HANDLE engine,
                    IN_ADDR server,
                    const GUID &own,
                    const std::vector<Sublayer> &layers,
                    const FWP_BYTE_BLOB *serviceApp,
                    policy_detail::Facts &facts,
                    InspectionFailure &failure)
{
    bool providerSeen = false;
    const GUID *layerKeys[] = {&FWPM_LAYER_ALE_AUTH_CONNECT_V4, &FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4};
    for (unsigned layerIndex = 0; layerIndex != 2; ++layerIndex) {
        FWPM_FILTER_ENUM_TEMPLATE0 query{};
        query.layerKey = *layerKeys[layerIndex];
        query.enumType = FWP_FILTER_ENUM_FULLY_CONTAINED;
        query.actionMask = 0xffffffff;
        HANDLE enumeration = nullptr;
        const auto createStatus = FwpmFilterCreateEnumHandle0(engine, &query, &enumeration);
        if (createStatus) {
            return failure.record("filter-enumeration", "filter-enumerator-create-failed", createStatus);
        }
        bool complete = false, valid = true;
        for (unsigned page = 0; page < 512; ++page) {
            WfpMemory<FWPM_FILTER0 *> entries;
            UINT32 count = 0;
            const auto enumStatus = FwpmFilterEnum0(engine, enumeration, 256, &entries.value, &count);
            if (enumStatus) {
                failure.record("filter-enumeration", "filter-enumeration-failed", enumStatus);
                break;
            }
            for (UINT32 i = 0; i < count; ++i) {
                const auto &filter = *entries.value[i];
                // Unrelated providers are outside the approved recognition
                // contract. Do not require their metadata to be readable or
                // parse unfamiliar layouts merely to recognize Tailscale.
                const auto isOwn = IsEqualGUID(own, filter.subLayerKey) != FALSE;
                const auto isTailscale = facts.hasTailscale && IsEqualGUID(facts.sublayer, filter.subLayerKey);
                if (!isOwn && !isTailscale) {
                    continue;
                }
                auto layer = std::find_if(layers.begin(), layers.end(), [&](const Sublayer &value) {
                    return IsEqualGUID(value.key, filter.subLayerKey);
                });
                if (layer == layers.end()) {
                    failure.record("filter-sublayer", "filter-references-unobserved-sublayer");
                    valid = false;
                    continue;
                }
                policy_detail::Rule rule;
                rule.layer = layerIndex;
                rule.sublayerWeight = layer->weight;
                rule.own = isOwn;
                rule.tailscale = isTailscale;
                rule.disjoint = definitelyDisjoint(filter, server);
                rule.action = filter.action.type == FWP_ACTION_PERMIT ? policy_detail::Action::Permit
                    : filter.action.type == FWP_ACTION_BLOCK          ? policy_detail::Action::Block
                    : (filter.action.type & FWP_ACTION_FLAG_CALLOUT)  ? policy_detail::Action::Callout
                                                                      : policy_detail::Action::Other;
                if (rule.tailscale) {
                    if (!filter.providerKey) {
                        failure.record("tailscale-filter-provider", "filter-provider-missing");
                        valid = false;
                        continue;
                    }
                    if (!providerSeen) {
                        providerSeen = true;
                        facts.provider = *filter.providerKey;
                        WfpMemory<FWPM_PROVIDER0> provider;
                        const auto providerStatus = FwpmProviderGetByKey0(engine, filter.providerKey, &provider.value);
                        if (providerStatus) {
                            failure.record("tailscale-filter-provider", "provider-query-failed", providerStatus);
                            valid = false;
                        } else if (!named(provider.value->displayData.name, L"Tailscale provider") || provider.value->flags || provider.value->providerData.size
                                   || (provider.value->serviceName && *provider.value->serviceName)) {
                            failure.record("tailscale-provider-metadata",
                                           !named(provider.value->displayData.name, L"Tailscale provider") ? "provider-name-unexpected"
                                               : provider.value->flags                                     ? "provider-flags-unexpected"
                                               : provider.value->providerData.size                         ? "provider-data-unexpected"
                                                                                                           : "provider-service-unexpected");
                            valid = false;
                        }
                    }
                    if (!IsEqualGUID(facts.provider, *filter.providerKey)) {
                        failure.record("tailscale-filter-provider", "filter-provider-inconsistent");
                        valid = false;
                    }
                    rule.flagsSupported = (filter.flags & ~FWPM_FILTER_FLAG_INDEXED) == 0 && !filter.providerData.size && filter.weight.type == FWP_UINT64
                        && filter.weight.uint64 && filter.effectiveWeight.type == FWP_UINT64 && filter.effectiveWeight.uint64;
                    if (rule.flagsSupported) {
                        rule.filterWeight = *filter.weight.uint64;
                        rule.flagsSupported = *filter.effectiveWeight.uint64 == rule.filterWeight;
                    }
                    if (!rule.flagsSupported) {
                        valid = false;
                        failure.record("tailscale-filter-metadata",
                                       (filter.flags & ~FWPM_FILTER_FLAG_INDEXED)                        ? "filter-flags-unsupported"
                                           : filter.providerData.size                                    ? "filter-provider-data-unexpected"
                                           : (filter.weight.type != FWP_UINT64 || !filter.weight.uint64) ? "filter-weight-type-unexpected"
                                           : (filter.effectiveWeight.type != FWP_UINT64 || !filter.effectiveWeight.uint64)
                                           ? "filter-effective-weight-type-unexpected"
                                           : "filter-effective-weight-mismatch");
                    }
                    rule.shape = classifyShape(filter, serviceApp, rule.tunnel);
                }
                facts.rules.push_back(rule);
            }
            if (count < 256) {
                complete = true;
                break;
            }
        }
        FwpmFilterDestroyEnumHandle0(engine, enumeration);
        if (!complete || !valid) {
            return failure.record("filter-enumeration", !complete ? "filter-enumeration-incomplete" : "filter-metadata-unverified");
        }
    }
    facts.metadataTrusted &= providerSeen;
    if (facts.hasTailscale && !providerSeen) {
        return failure.record("tailscale-filter-provider", "tailscale-provider-not-observed");
    }
    return true;
}
}

namespace policy_detail
{
Shape classifyFilterShape(const FWPM_FILTER0 &filter, const FWP_BYTE_BLOB *serviceApp, UINT64 &luid)
{
    return classifyShape(filter, serviceApp, luid);
}

bool filterDefinitelyDisjoint(const FWPM_FILTER0 &filter, IN_ADDR server)
{
    return definitelyDisjoint(filter, server);
}
}

PolicyProof inspectPolicy(HANDLE engine, IN_ADDR server, const GUID &ownSublayer)
{
    policy_detail::Facts facts;
    auto fail = [](const char *reason) {
        PolicyProof proof;
        proof.reason = reason;
        return proof;
    };
    SOCKADDR_INET destination{};
    destination.Ipv4.sin_family = AF_INET;
    destination.Ipv4.sin_addr = server;
    MIB_IPFORWARD_ROW2 route{};
    SOCKADDR_INET source{};
    if (!engine || GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source)) {
        return fail("server-route-unavailable");
    }
    MIB_IF_ROW2 row{};
    row.InterfaceLuid = route.InterfaceLuid;
    if (GetIfEntry2(&row)) {
        return fail("route-interface-unavailable");
    }
    facts.routeLuid = row.InterfaceLuid.Value;
    facts.routeUp = row.OperStatus == IfOperStatusUp;
    facts.routeHardware = row.InterfaceAndOperStatusFlags.HardwareInterface && (row.Type == IF_TYPE_ETHERNET_CSMACD || row.Type == IF_TYPE_IEEE80211);
    // Session enumeration is forbidden inside any WFP transaction. Capture
    // identity outside it, then require the same live identity after it ends.
    std::wstring serviceImage;
    DWORD servicePid = 0;
    GUID sessionKey{};
    DWORD sessionPid = 0;
    Handle serviceLifetime, sessionLifetime;
    PinnedAncestry sessionAncestry;
    InspectionFailure identityFailure, snapshotFailure;
    facts.complete = policy_detail::collectSnapshot(
        facts.hasTailscale,
        {[&] {
             facts.serviceTrusted = trustedTailscaleService(serviceImage, servicePid, identityFailure);
             facts.sessionTrusted =
                 facts.serviceTrusted && trustedFirewallSession(engine, servicePid, serviceImage, sessionKey, sessionPid, sessionAncestry, identityFailure);
             serviceLifetime.value = facts.sessionTrusted ? OpenProcess(SYNCHRONIZE, FALSE, servicePid) : nullptr;
             if (facts.sessionTrusted && !serviceLifetime.value) {
                 identityFailure.record("tailscale-service-lifetime", "process-lifetime-open-failed", GetLastError());
             }
             sessionLifetime.value = facts.sessionTrusted ? OpenProcess(SYNCHRONIZE, FALSE, sessionPid) : nullptr;
             if (facts.sessionTrusted && !sessionLifetime.value) {
                 identityFailure.record("tailscale-session-lifetime", "process-lifetime-open-failed", GetLastError());
             }
             facts.sessionTrusted = facts.sessionTrusted && serviceLifetime.value && sessionLifetime.value;
         },
         [&] {
             const auto transactionStatus = FwpmTransactionBegin0(engine, FWPM_TXN_READ_ONLY);
             if (transactionStatus) {
                 return snapshotFailure.record("read-transaction", "policy-read-transaction-unavailable", transactionStatus);
             }
             return true;
         },
         [&] {
             std::vector<Sublayer> layers;
             auto ok = inspectLayers(engine, facts, layers, snapshotFailure);
             WfpMemory<FWP_BYTE_BLOB> serviceApp;
             if (ok && facts.hasTailscale) {
                 if (!facts.serviceTrusted || !facts.sessionTrusted) {
                     snapshotFailure = identityFailure;
                     return false;
                 }
                 const auto appIdStatus = FwpmGetAppIdFromFileName0(serviceImage.c_str(), &serviceApp.value);
                 if (appIdStatus) {
                     return snapshotFailure.record("tailscale-service-app-id", "service-app-id-unavailable", appIdStatus);
                 }
             }
             return ok && inspectFilters(engine, server, ownSublayer, layers, serviceApp.value, facts, snapshotFailure);
         },
         [&] {
             const auto abortStatus = FwpmTransactionAbort0(engine);
             return !abortStatus || snapshotFailure.record("read-transaction", "policy-read-transaction-close-failed", abortStatus);
         },
         [&] {
             std::wstring currentImage;
             DWORD currentServicePid = 0, currentSessionPid = 0;
             GUID currentSessionKey{};
             PinnedAncestry currentAncestry;
             const auto unchanged = WaitForSingleObject(serviceLifetime.value, 0) == WAIT_TIMEOUT
                 && WaitForSingleObject(sessionLifetime.value, 0) == WAIT_TIMEOUT && sessionAncestry.alive()
                 && trustedTailscaleService(currentImage, currentServicePid, snapshotFailure) && currentServicePid == servicePid && currentImage == serviceImage
                 && trustedFirewallSession(engine, currentServicePid, currentImage, currentSessionKey, currentSessionPid, currentAncestry, snapshotFailure)
                 && IsEqualGUID(currentSessionKey, sessionKey) && currentSessionPid == sessionPid && sessionAncestry.alive()
                 && policy_detail::sameAncestry(sessionAncestry.identities, currentAncestry.identities);
             if (!unchanged) {
                 snapshotFailure.record("identity-recheck", "tailscale-identity-changed-during-policy-check");
             }
             return unchanged;
         }});
    if (!facts.complete) {
        return snapshotFailure.proof();
    }
    // Routing state is not part of the WFP transaction. Refuse a snapshot if
    // the selected route or interface changed while identity was being checked.
    MIB_IPFORWARD_ROW2 currentRoute{};
    SOCKADDR_INET currentSource{};
    MIB_IF_ROW2 currentRow{};
    if (GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &currentRoute, &currentSource)) {
        return fail("route-changed-during-policy-check");
    }
    currentRow.InterfaceLuid = currentRoute.InterfaceLuid;
    if (GetIfEntry2(&currentRow) || currentRoute.InterfaceLuid.Value != route.InterfaceLuid.Value || currentSource.si_family != source.si_family
        || currentSource.Ipv4.sin_addr.S_un.S_addr != source.Ipv4.sin_addr.S_un.S_addr || currentRoute.NextHop.si_family != route.NextHop.si_family
        || currentRoute.NextHop.Ipv4.sin_addr.S_un.S_addr != route.NextHop.Ipv4.sin_addr.S_un.S_addr || currentRow.OperStatus != row.OperStatus
        || currentRow.Type != row.Type || currentRow.InterfaceAndOperStatusFlags.HardwareInterface != row.InterfaceAndOperStatusFlags.HardwareInterface) {
        return fail("route-changed-during-policy-check");
    }
    if (facts.hasTailscale && !sessionAncestry.alive()) {
        return fail("tailscale-ancestry-exited-during-policy-check");
    }
    return policy_detail::assess(facts);
}
