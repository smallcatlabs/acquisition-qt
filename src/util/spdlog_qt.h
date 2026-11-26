// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QByteArray>
#include <QMetaEnum>
#include <QNetworkReply>
#include <QString>
#include <QVariant>

#include <spdlog/spdlog.h>

#include <type_traits>

// Define an inline helper function to convert levels into QStrings.

inline QString to_qstring(spdlog::level::level_enum level)
{
    const auto sv = spdlog::level::to_string_view(level);
    return QString::fromUtf8(sv.data(), sv.size());
}

// Create a specialized formatter for spdlog levels.

template<>
struct fmt::formatter<spdlog::level::level_enum>
{
    constexpr auto parse(fmt::format_parse_context &ctx) -> decltype(ctx.begin())
    {
        return ctx.end();
    }
    auto format(const spdlog::level::level_enum &level, fmt::format_context &ctx) const
        -> decltype(ctx.out())
    {
        return fmt::format_to(ctx.out(), "{}", spdlog::level::to_string_view(level));
    }
};

// Create a specialized formatter for QString.

template<>
struct fmt::formatter<QString>
{
    constexpr auto parse(fmt::format_parse_context &ctx) -> decltype(ctx.begin())
    {
        return ctx.end();
    }
    auto format(const QString &str, fmt::format_context &ctx) const -> decltype(ctx.out())
    {
        const auto utf8 = str.toUtf8();
        return fmt::format_to(ctx.out(), "{}", std::string_view(utf8.data(), utf8.size()));
    }
};

// Create a specialized formatter for QByteArray.

template<>
struct fmt::formatter<QByteArray>
{
    constexpr auto parse(fmt::format_parse_context &ctx) -> decltype(ctx.begin())
    {
        return ctx.end();
    }
    auto format(const QByteArray &arr, fmt::format_context &ctx) const -> decltype(ctx.out())
    {
        const auto str = arr.toStdString();
        return fmt::format_to(ctx.out(), "{}", str);
    }
};

// Create a specialized formatter for QVariant.

template<>
struct fmt::formatter<QVariant>
{
    constexpr auto parse(fmt::format_parse_context &ctx) -> decltype(ctx.begin())
    {
        return ctx.end();
    }
    auto format(const QVariant &var, fmt::format_context &ctx) const -> decltype(ctx.out())
    {
        if (!var.isValid()) {
            return fmt::format_to(ctx.out(), "<invalid>");
        }
        if (var.isNull()) {
            return fmt::format_to(ctx.out(), "<null>");
        }
        const std::string str = var.toString().toStdString();
        return fmt::format_to(ctx.out(), "{}", str);
    }
};

// Every enum Qt has a QMetaEnum for (Q_ENUM, Q_ENUM_NS, and Qt's own, such
// as QNetworkReply::NetworkError and Qt::CheckState) is formatted as its key,
// or as Name(value) for a value without one. An enum's header declares
// nothing for this: the file that logs the enum includes this header.

template<typename T>
concept QtMetaEnum = std::is_enum_v<T> && requires(T value) { qt_getEnumMetaObject(value); };

template<QtMetaEnum T>
struct fmt::formatter<T, char>
{
    constexpr auto parse(fmt::format_parse_context &ctx) -> decltype(ctx.begin())
    {
        return ctx.begin();
    }

    auto format(const T &value, fmt::format_context &ctx) const -> decltype(ctx.out())
    {
        const QMetaEnum metaEnum = QMetaEnum::fromType<T>();
        const char *key = metaEnum.valueToKey(static_cast<int>(value));
        if (key) {
            return fmt::format_to(ctx.out(), "{}", key);
        } else {
            return fmt::format_to(ctx.out(), "{}({})", metaEnum.name(), static_cast<int>(value));
        }
    }
};
