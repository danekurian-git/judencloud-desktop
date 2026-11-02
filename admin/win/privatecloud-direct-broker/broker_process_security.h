// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include <span>

namespace broker_process_security
{
struct ProcessAclApi {
    decltype(&GetSecurityInfo) get = &GetSecurityInfo;
    decltype(&SetSecurityInfo) set = &SetSecurityInfo;
};

bool minimalSystemToken(PSID user, std::span<const LUID_AND_ATTRIBUTES> privileges, const LUID &changeNotify);
// Returned ACL is owned by the caller and released with LocalFree.
DWORD appendInteractiveSynchronize(PACL existing, PACL *result);
DWORD grantInteractiveSynchronize(HANDLE process, const ProcessAclApi &api = {});
// Real service startup only: verifies its token before changing its own DACL.
DWORD prepare();
}
