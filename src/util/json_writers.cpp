// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#include "util/json_writers.h"

#include "poe/types/character.h"
#include "poe/types/stashtab.h"
#include "util/glaze_qt.h"  // IWYU pragma: keep
#include "util/spdlog_qt.h" // IWYU pragma: keep

namespace {
    template<typename T>
    QByteArray write_json(const T &obj)
    {
        std::string json;
        const auto err = glz::write_json(obj, json);
        if (err) {
            const auto type = typeid(T).name();
            const auto msg = glz::format_error(err);
            spdlog::error("Error writing {} to json: {}", type, msg);
            return {};
        }
        return QByteArray::fromStdString(json);
    }
} // namespace

QByteArray json::writeCharacter(const poe::Character &character)
{
    return write_json(character);
}

QByteArray json::writeStash(const poe::StashTab &stash)
{
    return write_json(stash);
}

