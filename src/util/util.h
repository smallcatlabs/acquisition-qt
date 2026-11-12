// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QMetaEnum>
#include <QObject>
#include <QString>

#include <utility>
#include <vector>

#include "util/spdlog_qt.h"

class QComboBox;
class QNetworkReply;

struct Buyout;

namespace poe {
    struct StashTab;
}

namespace Util {
    Q_NAMESPACE

    enum class TextWidthId {
        WIDTH_MIN_MAX,
        WIDTH_LABEL,
        WIDTH_RGB,
        WIDTH_GROUP, // Unused?
        WIDTH_BOOL_LABEL
    };
    Q_ENUM_NS(TextWidthId);

    enum class RefreshReason { Unknown, ItemsChanged, SearchFormChanged, TabCreated, TabChanged };
    Q_ENUM_NS(RefreshReason)

    enum class TabSelection { All, Checked, Selected, TabsOnly };
    Q_ENUM_NS(TabSelection)

    QString Md5(const QString &value);
    double AverageDamage(const QString &s);
    void PopulateBuyoutTypeComboBox(QComboBox *combobox);
    void PopulateBuyoutCurrencyComboBox(QComboBox *combobox);

    int TextWidth(TextWidthId id);

    QString GetCsrfToken(const QByteArray &page, const QString &name);
    QString FindTextBetween(const QString &page, const QString &left, const QString &right);

    void GetTabColor(const poe::StashTab &stash, int &r, int &g, int &b);

    QString StringReplace(const QString &haystack, const QString &needle, const QString &replace);
    QColor recommendedForegroundTextColor(const QColor &backgroundColor);

    /*
        Example usage:
            MatchMod("+# to Life", "+12.3 to Life", &result);
        Will return true if matches and save average value to output; on no match
        the output is 0.
    */
    bool MatchMod(const char *const match, const char *const mod, double *output);

    QString TimeAgoInWords(const QDateTime &buyout_time);

    QString Decode(const QString &entity);

    // An application/x-www-form-urlencoded body: each key and value
    // percent-encoded as UTF-8, joined with '=' and '&'.
    QByteArray EncodeForm(const std::vector<std::pair<QString, QString>> &fields);

    QByteArray FixTimezone(const QByteArray &rfc2822_date);

    // Convert Q_ENUM and Q_ENUM_NS objects to their string value.
    template<typename T>
    QString toString(const T &value)
    {
        return QMetaEnum::fromType<T>().valueToKey(static_cast<std::underlying_type_t<T>>(value));
    }

} // namespace Util

using TextWidthId = Util::TextWidthId;
template<>
struct fmt::formatter<TextWidthId, char> : QtEnumFormatter<TextWidthId>
{};

using RefreshReason = Util::RefreshReason;
template<>
struct fmt::formatter<RefreshReason, char> : QtEnumFormatter<RefreshReason>
{};

using TabSelection = Util::TabSelection;
template<>
struct fmt::formatter<TabSelection, char> : QtEnumFormatter<TabSelection>
{};
