/*
 * SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "account.h"

class AccountTestAccess
{
public:
    // Isolate URL formatting and loopback transport fixtures from server admission.
    // Tests of server admission must use Account::setUrl instead.
    static void setUrl(OCC::Account &account, const QUrl &url)
    {
        account._url = url;
        account._userVisibleUrl = url;
    }
};
