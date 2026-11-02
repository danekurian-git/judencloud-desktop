// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "broker_process_security.h"
#include <sddl.h>

#include <array>
#include <cstdio>

namespace
{
int checks = 0, failures = 0, writes = 0;
DWORD readError = 0, writeError = 0;
bool nullDacl = false;

void check(bool condition, const char *message)
{
    ++checks;
    failures += !condition;
    if (!condition) {
        std::printf("FAIL %s\n", message);
    }
}

bool ace(PACL acl, WELL_KNOWN_SID_TYPE kind, BYTE type, ACCESS_MASK exactMask)
{
    for (DWORD index = 0; index != acl->AceCount; ++index) {
        ACE_HEADER *header = nullptr;
        if (!GetAce(acl, index, reinterpret_cast<void **>(&header)) || header->AceType != type) {
            continue;
        }
        const auto *entry = reinterpret_cast<const ACCESS_ALLOWED_ACE *>(header);
        if (IsWellKnownSid(const_cast<DWORD *>(&entry->SidStart), kind) && entry->Mask == exactMask && header->AceFlags == 0) {
            return true;
        }
    }
    return false;
}

DWORD WINAPI
get(HANDLE, SE_OBJECT_TYPE object, SECURITY_INFORMATION information, PSID *owner, PSID *group, PACL *dacl, PACL *sacl, PSECURITY_DESCRIPTOR *security)
{
    check(object == SE_KERNEL_OBJECT && information == DACL_SECURITY_INFORMATION && !owner && !group && !sacl, "only own kernel object DACL is queried");
    if (readError) {
        return readError;
    }
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x1;;;IU)(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, security, nullptr)) {
        return GetLastError();
    }
    BOOL present = FALSE, defaulted = FALSE;
    GetSecurityDescriptorDacl(*security, &present, dacl, &defaulted);
    if (nullDacl) {
        *dacl = nullptr;
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI set(HANDLE, SE_OBJECT_TYPE object, SECURITY_INFORMATION information, PSID owner, PSID group, PACL dacl, PACL sacl)
{
    ++writes;
    check(object == SE_KERNEL_OBJECT && information == DACL_SECURITY_INFORMATION && !owner && !group && !sacl,
          "only DACL is updated; owner and SACL are untouched");
    check(dacl && IsValidAcl(dacl) && dacl->AceCount == 4, "one narrow ACE added to existing ACL");
    check(ace(dacl, WinInteractiveSid, ACCESS_ALLOWED_ACE_TYPE, SYNCHRONIZE), "interactive users receive exactly SYNCHRONIZE");
    check(ace(dacl, WinInteractiveSid, ACCESS_DENIED_ACE_TYPE, PROCESS_TERMINATE), "existing deny preserved");
    check(ace(dacl, WinLocalSystemSid, ACCESS_ALLOWED_ACE_TYPE, GENERIC_ALL) && ace(dacl, WinBuiltinAdministratorsSid, ACCESS_ALLOWED_ACE_TYPE, GENERIC_ALL),
          "SYSTEM and Administrators ACEs preserved");
    return writeError;
}
}

int main()
{
    BYTE system[SECURITY_MAX_SID_SIZE]{}, interactive[SECURITY_MAX_SID_SIZE]{};
    DWORD systemSize = sizeof(system), interactiveSize = sizeof(interactive);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemSize)
        || !CreateWellKnownSid(WinInteractiveSid, nullptr, interactive, &interactiveSize)) {
        return 1;
    }
    LUID allowed{23, 0}, unexpected{20, 0};
    std::array<LUID_AND_ATTRIBUTES, 1> minimal{{{allowed, SE_PRIVILEGE_ENABLED}}};
    std::array<LUID_AND_ATTRIBUTES, 2> wider{{minimal[0], {unexpected, 0}}};
    check(broker_process_security::minimalSystemToken(system, minimal, allowed), "SYSTEM with only change-notify accepted");
    check(!broker_process_security::minimalSystemToken(interactive, minimal, allowed), "non-SYSTEM rejected");
    check(!broker_process_security::minimalSystemToken(nullptr, minimal, allowed), "missing identity rejected");
    check(!broker_process_security::minimalSystemToken(system, wider, allowed), "additional disabled privilege also rejected");
    wider[1].Attributes = SE_PRIVILEGE_ENABLED;
    check(!broker_process_security::minimalSystemToken(system, wider, allowed), "additional enabled privilege rejected");
    check(!broker_process_security::minimalSystemToken(system, {}, allowed), "missing expected privilege rejected");
    minimal[0].Luid = unexpected;
    check(!broker_process_security::minimalSystemToken(system, minimal, allowed), "wrong single privilege rejected");
    minimal[0] = {allowed, SE_PRIVILEGE_REMOVED};
    check(!broker_process_security::minimalSystemToken(system, minimal, allowed), "removed privilege is not a usable startup context");
    const broker_process_security::ProcessAclApi api{&get, &set};
    const auto fakeProcess = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1));
    check(broker_process_security::grantInteractiveSynchronize(fakeProcess, api) == ERROR_SUCCESS && writes == 1,
          "injected ACL flow succeeds without modifying any real process");
    readError = ERROR_ACCESS_DENIED;
    check(broker_process_security::grantInteractiveSynchronize(fakeProcess, api) == ERROR_ACCESS_DENIED && writes == 1, "read failure prevents ACL write");
    readError = 0;
    nullDacl = true;
    check(broker_process_security::grantInteractiveSynchronize(fakeProcess, api) == ERROR_INVALID_ACL && writes == 1,
          "null DACL refuses instead of broadening access");
    nullDacl = false;
    writeError = ERROR_ACCESS_DENIED;
    check(broker_process_security::grantInteractiveSynchronize(fakeProcess, api) == ERROR_ACCESS_DENIED && writes == 2,
          "write failure propagates to refuse service startup");
    PACL result = nullptr;
    check(broker_process_security::appendInteractiveSynchronize(nullptr, &result) == ERROR_INVALID_ACL && !result, "missing ACL rejected");
    check(broker_process_security::appendInteractiveSynchronize(nullptr, nullptr) == ERROR_INVALID_PARAMETER, "missing output rejected");
    std::printf("Broker process security: %d checks, %d failures; injected process ACL calls only.\n", checks, failures);
    return failures ? 1 : 0;
}
