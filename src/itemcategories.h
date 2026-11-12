// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2022 testpushpleaseignore <elnino2k10@gmail.com>

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

void InitItemClasses(const QByteArray &classes);
void InitItemBaseTypes(const QByteArray &baseTypes);

QString GetItemCategory(const QString &baseType);

const QStringList &GetItemCategories();

// The item classes, besides gear, whose items Path of Building imports:
// the flasks and the jewels, by RePoE item class id.
const QStringList &PathOfBuildingNonGearClassIds();

// Whether Path of Building imports items of this RePoE item class id: gear
// (a class the game can give influence tags) or one of the classes above.
bool IsPathOfBuildingItemClass(const QString &classId);

// Whether Path of Building imports an item of this base type.
bool IsPathOfBuildingBaseType(const QString &baseType);
