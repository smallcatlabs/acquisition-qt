// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "modlist.h"

#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QStringView>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "item.h"
#include "pseudomods.h"
#include "repoe/stattranslation.h"
#include "util/glaze_qt.h"
#include "util/spdlog_qt.h"
#include "util/util.h"

namespace {

    // A number as the game prints it in a mod line, with its sign if any.
    const QRegularExpression &number_pattern()
    {
        static const QRegularExpression re(R"([+-]?[0-9]+(?:\.[0-9]+)?)");
        return re;
    }

    // The mod names a line can be filed under, built once from the stat
    // translations and not changed afterwards.
    //
    // A line is first rewritten the way it always was, every run of digits
    // and points to "#", and looked up by that name; every line that
    // matched before matches the same way. A line that does not is matched
    // through its shape, every number and every placeholder (each with any
    // sign before it) read as "#", and then checked against each template
    // of that shape piece by piece: the template's own numbers must be in
    // the line as written ("Level 20"), and a "+#" placeholder takes the
    // line's number with its sign (#265).
    class ModCatalogue
    {
    public:
        struct Match
        {
            QString name;
            double value;
        };

        explicit ModCatalogue(const std::set<QString> &names)
            : m_names(names.begin(), names.end())
        {
            for (const QString &name : names) {
                const size_t index = m_templates.size();
                m_templates.push_back(tokenize(name));
                m_by_shape[shape(name)].push_back(index);
            }
            // Where templates of one shape differ only in which numbers are
            // placeholders, a line goes to the one with the most
            // placeholders, as the rewrite above has always done; then to
            // one that writes the line's sign as text ("-# to ...", which
            // RePoE uses for negative values) over one that reads it.
            for (auto &[key, indices] : m_by_shape) {
                std::ranges::stable_sort(indices, {}, [this](size_t index) {
                    int placeholders = 0;
                    int signed_placeholders = 0;
                    for (const Token &token : m_templates[index].tokens) {
                        placeholders += (token.kind != Kind::Literal) ? 1 : 0;
                        signed_placeholders += (token.kind == Kind::SignedNumber) ? 1 : 0;
                    }
                    return std::pair{-placeholders, signed_placeholders};
                });
            }
        }

        // The rewrite the lookup has always used.
        static QString generic(const QString &line)
        {
            static const QRegularExpression digits("[0-9.]+");
            QString result = line;
            result.replace(digits, "#");
            return result;
        }

        std::optional<Match> match(const QString &line, const QString &generic_line) const
        {
            if (m_names.contains(generic_line)) {
                double value = 0.0;
                if (Util::MatchMod(generic_line.toStdString().c_str(),
                                   line.toStdString().c_str(),
                                   &value)) {
                    return Match{generic_line, value};
                }
            }
            const auto bucket = m_by_shape.find(shape(line));
            if (bucket == m_by_shape.end()) {
                return std::nullopt;
            }
            for (const size_t index : bucket->second) {
                if (const auto value = verify(m_templates[index], line)) {
                    return Match{m_templates[index].name, *value};
                }
            }
            return std::nullopt;
        }

    private:
        enum class Kind { Literal, Number, SignedNumber };

        struct Token
        {
            Kind kind;
            QString literal;
        };

        struct Template
        {
            QString name;
            std::vector<Token> tokens;
        };

        static QString shape(const QString &text)
        {
            static const QRegularExpression placeholder(R"([+-]?#)");
            QString result = text;
            result.replace(placeholder, "#");
            result.replace(number_pattern(), "#");
            return result;
        }

        static Template tokenize(const QString &name)
        {
            Template result{name, {}};
            QString literal;
            for (qsizetype i = 0; i < name.size(); ++i) {
                if (name[i] != '#') {
                    literal.append(name[i]);
                    continue;
                }
                Kind kind = Kind::Number;
                if (literal.endsWith('+')) {
                    literal.chop(1);
                    kind = Kind::SignedNumber;
                }
                if (!literal.isEmpty()) {
                    result.tokens.push_back({Kind::Literal, literal});
                    literal.clear();
                }
                result.tokens.push_back({kind, {}});
            }
            if (!literal.isEmpty()) {
                result.tokens.push_back({Kind::Literal, literal});
            }
            return result;
        }

        // The mean of the line's numbers if it is this template, read in the
        // C locale.
        static std::optional<double> verify(const Template &templ, const QString &line)
        {
            qsizetype pos = 0;
            double sum = 0.0;
            int count = 0;
            for (const Token &token : templ.tokens) {
                if (token.kind == Kind::Literal) {
                    if (!QStringView(line).sliced(pos).startsWith(token.literal)) {
                        return std::nullopt;
                    }
                    pos += token.literal.size();
                    continue;
                }
                const QRegularExpressionMatch number
                    = number_pattern().matchView(QStringView(line),
                                                 pos,
                                                 QRegularExpression::NormalMatch,
                                                 QRegularExpression::AnchorAtOffsetMatchOption);
                if (!number.hasMatch()) {
                    return std::nullopt;
                }
                const QStringView text = number.capturedView(0);
                const bool has_sign = (text.front() == '+') || (text.front() == '-');
                if (has_sign != (token.kind == Kind::SignedNumber)) {
                    return std::nullopt;
                }
                sum += text.toDouble();
                ++count;
                pos += text.size();
            }
            if (pos != line.size()) {
                return std::nullopt;
            }
            return count > 0 ? sum / count : 0.0;
        }

        std::unordered_set<QString> m_names;
        std::vector<Template> m_templates;
        std::unordered_map<QString, std::vector<size_t>> m_by_shape;
    };

    QStringListModel m_mod_list_model;
    std::set<QString> mods;
    // Replaced whole by InitModList; never changed in place.
    std::shared_ptr<const ModCatalogue> catalogue;

} // namespace

QStringListModel &mod_list_model()
{
    return m_mod_list_model;
}

void InitStatTranslations()
{
    spdlog::trace("InitStatTranslations() entered");
    mods.clear();

    // Seed the mods list with pseudo mods.
    for (const auto &[pseudo_mod, real_mods] : PseudoModManager::SUMMING_MODS) {
        mods.insert(pseudo_mod);
    }
}

void AddStatTranslations(const QByteArray &statTranslations)
{
    spdlog::trace("AddStatTranslations() entered");

    std::vector<repoe::StatTranslation> translations;

    constexpr const glz::opts permissive{.error_on_unknown_keys = false};
    const std::string_view sv{statTranslations.constData(), size_t(statTranslations.size())};
    const auto ec = glz::read<permissive>(translations, sv);
    if (ec) {
        const auto msg = glz::format_error(ec, sv);
        spdlog::error("Unable to parse RePoE stat translations: {}", msg);
        return;
    }

    for (auto &translation : translations) {
        for (auto &stat : translation.English) {
            if (stat.is_markup.value_or(false)) {
                // This was added with the change to process json files inside
                // the stat_translations directory. In this case, the necropolis
                // mods from 3.24 have some kind of duplicate formatting with
                // markup that acquisition has not had to deal with before.
                //
                // It's possible this is true for other files in the stat_translations
                // folder, but acquisition has never needed to load modifiers from those
                // files before.
                continue;
            }
            QString stat_string = stat.string;
            // A stat whose format is "ignore" is not displayed and has no
            // placeholder in the string; every other stat's placeholder is
            // replaced, whichever stat comes first (#265).
            for (size_t i = 0; i < stat.format.size(); i++) {
                if (stat.format[i] != QLatin1String("ignore")) {
                    stat_string.replace(QStringLiteral("{%1}").arg(i), stat.format[i]);
                }
            }
            if (stat_string.length() > 0) {
                mods.insert(stat_string);
            }
        }
    }
}

void InitModList()
{
    spdlog::trace("InitModList() entered");

    for (const auto &[pseudo_mod, sources] : PseudoModManager::SUMMING_MODS) {
        for (const QString &source : sources) {
            if (!mods.contains(source)) {
                spdlog::debug("Pseudo-mod {} sums {}, which no stat translation names",
                              pseudo_mod,
                              source);
            }
        }
    }
    catalogue = std::make_shared<const ModCatalogue>(mods);

    QStringList mod_list(mods.begin(), mods.end());
    mod_list.sort(Qt::CaseInsensitive);
    m_mod_list_model.setStringList(mod_list);
}

void AddModToTable(const QString &raw_mod, ModTable &output)
{
    const QString generic_mod = ModCatalogue::generic(raw_mod);
    const auto match = catalogue ? catalogue->match(raw_mod, generic_mod) : std::nullopt;

    if (match) {
        // The same mod can appear in several sections (implicit and explicit,
        // say); the item's value is the sum of its lines (#262).
        output[match->name] += match->value;
    }

    // Pseudo-mods sum the mods they name. A line that matched no template
    // still feeds them under its rewritten text, as it always has.
    const QString &source = match ? match->name : generic_mod;
    const auto it = PseudoModManager::SUMMING_MODS_LOOKUP.find(source);
    if (it == PseudoModManager::SUMMING_MODS_LOOKUP.end()) {
        return;
    }
    double value = 0.0;
    if (match) {
        value = match->value;
    } else {
        Util::MatchMod(generic_mod.toStdString().c_str(), raw_mod.toStdString().c_str(), &value);
    }
    for (const QString &pseudo_mod : it->second) {
        output[pseudo_mod] += value;
    }
}
