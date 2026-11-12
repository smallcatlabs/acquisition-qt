// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2020 testpushpleaseignore <elnino2k10@gmail.com>

#pragma once

#include <QIcon>
#include <QString>

#include "item.h"

// The resource path of an influence's symbol, as drawn by the tooltip and
// the Inf column; empty for NONE.
QString InfluenceSymbolPath(Item::INFLUENCE_TYPES influence);

QIcon combineInflunceIcons(const QIcon &leftIcon, const QIcon &rightIcon);
