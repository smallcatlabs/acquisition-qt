// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <QString>

// For broken invariants only: logs at critical level and aborts. A problem
// the user can fix (missing SSL, a configuration error) exits cleanly instead.
[[noreturn]] void FatalError(const QString &message);
