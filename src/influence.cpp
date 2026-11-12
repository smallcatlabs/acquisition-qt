// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2020 testpushpleaseignore <elnino2k10@gmail.com>

#include "influence.h"

#include <QIcon>
#include <QImage>
#include <QPainter>

QString InfluenceSymbolPath(Item::INFLUENCE_TYPES influence)
{
    switch (influence) {
    case Item::SHAPER:
        return QStringLiteral(":/symbols/ShaperSymbol.png");
    case Item::ELDER:
        return QStringLiteral(":/symbols/ElderSymbol.png");
    case Item::CRUSADER:
        return QStringLiteral(":/symbols/CrusaderSymbol.png");
    case Item::REDEEMER:
        return QStringLiteral(":/symbols/RedeemerSymbol.png");
    case Item::HUNTER:
        return QStringLiteral(":/symbols/HunterSymbol.png");
    case Item::WARLORD:
        return QStringLiteral(":/symbols/WarlordSymbol.png");
    case Item::SYNTHESISED:
        return QStringLiteral(":/symbols/SynthesisedSymbol.png");
    case Item::FRACTURED:
        return QStringLiteral(":/symbols/FracturedSymbol.png");
    case Item::SEARING_EXARCH:
        return QStringLiteral(":/symbols/SearingExarchSymbol.png");
    case Item::EATER_OF_WORLDS:
        return QStringLiteral(":/symbols/EaterOfWorldsSymbol.png");
    case Item::NONE:
        break;
    }
    return {};
}

QIcon combineInflunceIcons(const QIcon &leftIcon, const QIcon &rightIcon)
{
    const int width = 54;
    const int height = 27;

    QPixmap layered(width, height);
    layered.fill(Qt::transparent);
    QPainter layered_painter(&layered);

    layered_painter.drawPixmap(0, 0, leftIcon.pixmap(27, 27));
    layered_painter.drawPixmap(27, 0, rightIcon.pixmap(27, 27));

    return QIcon(layered);
}
