// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "broker_process_security.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace broker_process_security
{
namespace
{
DWORD queryToken(HANDLE token, TOKEN_INFORMATION_CLASS type, std::vector<BYTE> &result)
{
    DWORD required = 0;
    if (GetTokenInformation(token, type, nullptr, 0, &required)) {
        return ERROR_INVALID_DATA;
    }
    const auto error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER || !required) {
        return error ? error : ERROR_INVALID_DATA;
    }
    result.resize(required);
    return GetTokenInformation(token, type, result.data(), required, &required) ? ERROR_SUCCESS : GetLastError();
}
}

bool minimalSystemToken(PSID user, std::span<const LUID_AND_ATTRIBUTES> privileges, const LUID &changeNotify)
{
    return user && IsValidSid(user) && IsWellKnownSid(user, WinLocalSystemSid) && privileges.size() == 1
        && privileges.front().Luid.LowPart == changeNotify.LowPart && privileges.front().Luid.HighPart == changeNotify.HighPart
        && !(privileges.front().Attributes & SE_PRIVILEGE_REMOVED);
}

DWORD appendInteractiveSynchronize(PACL existing, PACL *result)
{
    if (!result) {
        return ERROR_INVALID_PARAMETER;
    }
    *result = nullptr;
    if (!existing || !IsValidAcl(existing)) {
        return ERROR_INVALID_ACL;
    }
    BYTE interactive[SECURITY_MAX_SID_SIZE]{};
    DWORD size = sizeof(interactive);
    if (!CreateWellKnownSid(WinInteractiveSid, nullptr, interactive, &size)) {
        return GetLastError();
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = SYNCHRONIZE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(interactive);
    return SetEntriesInAclW(1, &access, existing, result);
}

DWORD grantInteractiveSynchronize(HANDLE process, const ProcessAclApi &api)
{
    PACL existing = nullptr;
    PSECURITY_DESCRIPTOR security = nullptr;
    const auto read = api.get(process, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &existing, nullptr, &security);
    if (read) {
        if (security) {
            LocalFree(security);
        }
        return read;
    }
    PACL updated = nullptr;
    const auto built = appendInteractiveSynchronize(existing, &updated);
    LocalFree(security);
    if (built) {
        if (updated) {
            LocalFree(updated);
        }
        return built;
    }
    const auto applied = api.set(process, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
    LocalFree(updated);
    return applied;
}

DWORD prepare()
{
    HANDLE tokenHandle = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenHandle)) {
        return GetLastError();
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> token(tokenHandle, &CloseHandle);
    std::vector<BYTE> userBytes, privilegeBytes;
    const auto userRead = queryToken(token.get(), TokenUser, userBytes);
    const auto privilegeRead = userRead ? userRead : queryToken(token.get(), TokenPrivileges, privilegeBytes);
    if (privilegeRead) {
        return privilegeRead;
    }
    constexpr auto privilegeOffset = offsetof(TOKEN_PRIVILEGES, Privileges);
    if (userBytes.size() < sizeof(TOKEN_USER) || privilegeBytes.size() < privilegeOffset) {
        return ERROR_INVALID_DATA;
    }
    const auto *privileges = reinterpret_cast<const TOKEN_PRIVILEGES *>(privilegeBytes.data());
    if (privileges->PrivilegeCount > (privilegeBytes.size() - privilegeOffset) / sizeof(LUID_AND_ATTRIBUTES)) {
        return ERROR_INVALID_DATA;
    }
    LUID changeNotify{};
    if (!LookupPrivilegeValueW(nullptr, SE_CHANGE_NOTIFY_NAME, &changeNotify)) {
        return GetLastError();
    }
    const auto user = reinterpret_cast<const TOKEN_USER *>(userBytes.data())->User.Sid;
    if (!minimalSystemToken(user, {privileges->Privileges, privileges->PrivilegeCount}, changeNotify)) {
        return ERROR_PRIVILEGE_NOT_HELD;
    }
    return grantInteractiveSynchronize(GetCurrentProcess());
}
}
