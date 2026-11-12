// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "util/util.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFontMetrics>
#include <QLabel>
#include <QLineEdit>
#include <QMetaEnum>
#include <QNetworkReply>
#include <QPainter>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTextDocument>
#include <QUrlQuery>

#include <cmath>

#include "currency.h"
#include "poe/types/stashtab.h"

QByteArray Util::toPathBytes(const QString &path)
{
    return QFile::encodeName(QDir::toNativeSeparators(path));
}

QString Util::Md5(const QString &value)
{
    const QString hash = QString(
        QCryptographicHash::hash(value.toStdString().c_str(), QCryptographicHash::Md5).toHex());
    return hash.toUtf8().constData();
}

double Util::AverageDamage(const QString &s)
{
    const QStringList parts = s.split("-");
    if (parts.size() < 2) {
        return s.toDouble();
    } else {
        return (parts[0].toDouble() + parts[1].toDouble()) / 2;
    }
}

void Util::PopulateBuyoutTypeComboBox(QComboBox *combobox)
{
    combobox->addItems(QStringList(
        {"[Ignore]", "Buyout", "Fixed price", "Current Offer", "No price", "[Inherit]"}));
    combobox->setCurrentIndex(5);
}

void Util::PopulateBuyoutCurrencyComboBox(QComboBox *combobox)
{
    for (auto type : Currency::Types()) {
        combobox->addItem(QString(Currency(type).AsString()));
    }
}

constexpr std::array width_strings = {"max#", "Map Tier", "R##", "Defense", "Master-crafted"};

int Util::TextWidth(TextWidthId id)
{
    static bool calculated = false;
    static std::vector<int> result;

    if (!calculated) {
        calculated = true;
        result.resize(width_strings.size());
        QLineEdit textbox;
        QFontMetrics fm(textbox.fontMetrics());
        for (size_t i = 0; i < width_strings.size(); ++i) {
            result[i] = fm.horizontalAdvance(width_strings[i]);
        }
    }
    return result[static_cast<int>(id)];
}

QString Util::GetCsrfToken(const QByteArray &page, const QString &name)
{
    // As of October 2023, the CSRF token can appear in one of two ways:
    //  name="hash" value="..."
    //	or
    //	name="hash" class="input-error" value="..."
    //
    // clang-format off
    static const QString expr = QString(R"regex(
		name="%1"
		\s+
		(?:
			class=".*?"
			\s+
		)?
		value="(.*?)")regex").simplified().arg(name);
    // clang-format on
    static const QRegularExpression re(expr,
                                       QRegularExpression::CaseInsensitiveOption
                                           | QRegularExpression::MultilineOption
                                           | QRegularExpression::DotMatchesEverythingOption
                                           | QRegularExpression::ExtendedPatternSyntaxOption);
    const QRegularExpressionMatch match = re.match(page);
    return match.captured(1);
}

QString Util::FindTextBetween(const QString &page, const QString &left, const QString &right)
{
    const std::string s = page.toStdString();
    const size_t first = s.find(left.toStdString());
    const size_t last = s.find(right.toStdString(), first);
    if ((first == std::string::npos) || (last == std::string::npos) || (first > last)) {
        return "";
    } else {
        return QString::fromStdString(s.substr(first + left.size(), last - first - left.size()));
    }
}

void Util::GetTabColor(const poe::StashTab &stash, int &r, int &g, int &b)
{
    r = 0;
    g = 0;
    b = 0;

    if (stash.metadata.colour) {
        // The colour field is supposed to be a 6-character string, but it some really old
        // tabs it's only 4 characters or 2 characters, and GGG has confirmed that in these
        // cases the leading values should be treated as zeros.
        const std::string colour = stash.metadata.colour->toStdString();
        switch (colour.length()) {
        case 6:
            r = std::stoul(colour.substr(0, 2), nullptr, 16);
            g = std::stoul(colour.substr(2, 2), nullptr, 16);
            b = std::stoul(colour.substr(4, 2), nullptr, 16);
            break;
        case 4:
            g = std::stoul(colour.substr(0, 2), nullptr, 16);
            b = std::stoul(colour.substr(2, 2), nullptr, 16);
            break;
        case 2:
            b = std::stoul(colour.substr(0, 2), nullptr, 16);
            break;
        default:
            spdlog::debug("Could not parse stash tab colour from {}: {} ({})",
                          *stash.metadata.colour,
                          stash.id,
                          stash.name);
            break;
        }
    } else {
        spdlog::debug("StashTab metadata does not have a colour: {} ({})", stash.id, stash.name);
    }
}

QString Util::StringReplace(const QString &haystack, const QString &needle, const QString &replace)
{
    std::string out = haystack.toStdString();
    for (size_t pos = 0;; pos += replace.length()) {
        pos = out.find(needle.toStdString(), pos);
        if (pos == std::string::npos) {
            break;
        }
        out.erase(pos, needle.length());
        out.insert(pos, replace.toStdString());
    }
    return QString::fromStdString(out);
}

bool Util::MatchMod(const char *const match, const char *const mod, double *output)
{
    double result = 0.0;
    auto pmatch = match;
    auto pmod = mod;
    int cnt = 0;

    // Callers read the output even when the match fails: a failed match
    // reports 0.
    *output = 0.0;

    while (*pmatch && *pmod) {
        if (*pmatch == '#') {
            ++cnt;
            auto prev = pmod;
            while ((*pmod >= '0' && *pmod <= '9') || *pmod == '.')
                ++pmod;
            result += std::strtod(prev, NULL);
            ++pmatch;
        } else if (*pmatch == *pmod) {
            ++pmatch;
            ++pmod;
        } else {
            return false;
        }
    }
    if (*pmatch || *pmod) {
        return false;
    }
    // A mod with no number has no value to average (#265).
    *output = cnt > 0 ? result / cnt : 0.0;
    return true;
}

QString Util::TimeAgoInWords(const QDateTime &buyout_time)
{
    const QDateTime current_date = QDateTime::currentDateTime();
    const qint64 secs = buyout_time.secsTo(current_date);
    const qint64 days = secs / 60 / 60 / 24;
    const qint64 hours = (secs / 60 / 60) % 24;
    const qint64 minutes = (secs / 60) % 60;

    // YEARS
    if (days > 365) {
        int years = (days / 365);
        if (days % 365 != 0) {
            years++;
        }
        return QString("%1 %2 ago").arg(years).arg(years == 1 ? "year" : "years");
    }

    // MONTHS
    if (days > 30) {
        int months = (days / 365);
        if (days % 30 != 0) {
            months++;
        }
        return QString("%1 %2 ago").arg(months).arg(months == 1 ? "month" : "months");
    }

    // DAYS
    if (days > 0) {
        return QString("%1 %2 ago").arg(days).arg(days == 1 ? "day" : "days");
    }

    // HOURS
    if (hours > 0) {
        return QString("%1 %2 ago").arg(hours).arg(hours == 1 ? "hour" : "hours");
    }

    //MINUTES
    if (minutes > 0) {
        return QString("%1 %2 ago").arg(minutes).arg(minutes == 1 ? "minute" : "minutes");
    }

    // SECONDS
    if (secs > 5) {
        return QString("%1 %2 ago").arg(secs).arg("seconds");
    } else if (secs < 5) {
        return QString("just now");
    }

    return "";
}

QString Util::Decode(const QString &entity)
{
    QTextDocument text;
    text.setHtml(entity);
    return text.toPlainText();
}

QUrlQuery Util::EncodeQueryItems(const std::vector<std::pair<QString, QString>> &items)
{
    // https://github.com/owncloud/client/issues/9203
    QUrlQuery result;
    for (const auto &item : items) {
        const QString key = QUrl::toPercentEncoding(item.first);
        const QString value = QUrl::toPercentEncoding(item.second);
        result.addQueryItem(key, value);
    }
    return result;
}

QColor Util::recommendedForegroundTextColor(const QColor &backgroundColor)
{
    const float R = (float) backgroundColor.red() / 255.0f;
    const float G = (float) backgroundColor.green() / 255.0f;
    const float B = (float) backgroundColor.blue() / 255.0f;

    const float gamma = 2.2f;
    const float L = 0.2126f * pow(R, gamma) + 0.7152f * pow(G, gamma) + 0.0722f * pow(B, gamma);

    return (L > 0.5f) ? QColor(QColorConstants::Black) : QColor(QColorConstants::White);
}

// Obsolete timezones are allowed by RFC2822, but they aren't parsed by
// QT 6.5.3 so we have to fix them manually.
QByteArray Util::FixTimezone(const QByteArray &rfc2822_date)
{
    constexpr const std::array<std::pair<const char *, const char *>, 10> OBSOLETE_ZONES{
        {{"GMT", "+0000"},
         {"UT", "+0000"},
         {"EST", "-0005"},
         {"EDT", "-0004"},
         {"CST", "-0006"},
         {"CDT", "-0005"},
         {"MST", "-0007"},
         {"MDT", "-0006"},
         {"PST", "-0008"},
         {"PDT", "-0007"}}};
    for (auto &pair : OBSOLETE_ZONES) {
        const QByteArray &zone = pair.first;
        const QByteArray &offset = pair.second;
        if (rfc2822_date.endsWith(zone)) {
            const int k = rfc2822_date.length() - zone.length();
            return rfc2822_date.left(k) + offset;
        }
    }
    return rfc2822_date;
}
