// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <QStringList>

class QDir;

// Each returns false when the user chose to quit.
bool checkMicrosoftRuntime();
bool checkApplicationDirectory(const QDir &dir, const QStringList &libraries);
