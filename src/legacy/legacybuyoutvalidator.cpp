// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#include "legacybuyoutvalidator.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>

#include "version_defines.h"

const QString LegacyBuyoutValidator::SettingsKey = "skip_buyout_validation";

LegacyBuyoutValidator::LegacyBuyoutValidator(QSettings &settings, const QString &filename)
    : QObject()
    , m_settings(settings)
    , m_filename(filename)
    , m_datastore(filename)
{
    if (!m_datastore.isValid()) {
        m_status = ValidationResult::Error;
    }
}

LegacyBuyoutValidator::ValidationResult LegacyBuyoutValidator::validate()
{
    if (m_status != ValidationResult::Error) {
        validateTabBuyouts();
        validateItemBuyouts();
        m_status = m_issues.empty() ? ValidationResult::Valid : ValidationResult::Invalid;
    }
    return m_status;
}

void LegacyBuyoutValidator::notifyUser()
{
    QLocale locale = QLocale::system();

    const auto &data = m_datastore.data();
    const auto &tabs = m_datastore.tabs();

    QStringList lines;
    lines += QString("Suspected issues:");
    for (const auto &pair : m_issues) {
        const QString &issue = pair.first;
        const int count = pair.second.size();
        lines += QString("    - %1 %2").arg(locale.toString(count), issue);
    }
    lines += QString();
    lines += "The data file is: \"" + m_filename + "\"";
    lines += QString();
    lines += "The data file contains:";
    lines += QString("   - %1 items").arg(locale.toString(m_datastore.itemCount()));
    lines += QString("   - %1 stash tabs").arg(locale.toString(tabs.stashes.size()));
    lines += QString("   - %1 characters").arg(locale.toString(tabs.characters.size()));
    lines += QString("   - %1 stash tab buyouts").arg(locale.toString(data.tab_buyouts.size()));
    lines += QString("   - %1 item buyouts").arg(locale.toString(data.buyouts.size()));
    QString message = lines.join("\n");

    QTextEdit *details = new QTextEdit;
    details->setReadOnly(true);
    details->setText(message);

    QPushButton *report_button = new QPushButton("Send a buyout report");
    QPushButton *ignore_button = new QPushButton("Ignore and continue");
    QCheckBox *reminder_checkbox = new QCheckBox("Don't ask again (applies to " APP_VERSION_STRING
                                                 ")");

    QLabel *help_label = new QLabel;
    help_label->setText(
        "Please consider submitting a buyout report. This process is automatic,"
        " using Bugsplat's crash reporting mechanish. It will also help me troubleshoot"
        " this issue, since I only have my own accounts to test."
        "<br/>"
        "<br/>"
        "For more information or to ask questions, you can email me at"
        " <a href=\"mailto:gerwaric@gmail.com\">gerwaric@gmail.com</a>"
        " or use this Github discussion:"
        "<br/>"
        "<a "
        "href=\"https://github.com/smallcatlabs/acquisition-qt/discussions/88\">https://github.com/"
        "smallcatlabs/acquisition-qt/discussions/88</a>.");
    help_label->setOpenExternalLinks(true);
    help_label->setWordWrap(true);
    help_label->setMargin(10);

    QLayout *layout = new QVBoxLayout;
    layout->addWidget(
        new QLabel("The buyout validator has detected potential issues with your data."));
    layout->addWidget(details);
    layout->addWidget(help_label);
    layout->addWidget(report_button);
    layout->addWidget(ignore_button);
    layout->addWidget(reminder_checkbox);

    QDialog *dialog = new QDialog;
    dialog->setWindowTitle("Acquisition Buyout Validator");
    dialog->setSizeGripEnabled(true);
    dialog->setModal(true);
    dialog->setLayout(layout);

    // Connect the reminder checkbox.
    connect(reminder_checkbox, &QCheckBox::checkStateChanged, this, [=]() {
        if (reminder_checkbox->isChecked()) {
            m_settings.setValue(LegacyBuyoutValidator::SettingsKey,
                                QStringLiteral(APP_VERSION_STRING));
        } else {
            m_settings.remove(LegacyBuyoutValidator::SettingsKey);
        }
    });

    // Connect the github button.
    connect(report_button, &QPushButton::clicked, this, [=]() {
        const QString exportfile
            = QFileInfo(m_filename).dir().absoluteFilePath("../export/buyouts.tgz");
        if (!m_datastore.exportTgz(exportfile)) {
            QMessageBox::information(nullptr,
                                     "Acquisition",
                                     "Unable to export buyout data; acquisition will continue. "
                                     "Please consider reporting this issue on github.");
            spdlog::warn("Unable to export tgz");
            return;
        }
        QMessageBox::warning(
            nullptr,
            "Acquisition",
            "Acquisition will now exit to trigger a crash report with buyout information. "
            "You will need to restart acquisition manually.");
        spdlog::critical("Aborting acquisition to trigger a crash report with buyout information");
        abort();
    });

    // Connect the ignore button.
    connect(ignore_button, &QPushButton::clicked, this, [=]() {
        dialog->close();
        dialog->deleteLater();
    });

    // Show the dialog and wait for the user to close it.
    dialog->exec();
}

void LegacyBuyoutValidator::validateTabBuyouts()
{
    const auto &stashes = m_datastore.tabs().stashes;
    const auto &characters = m_datastore.tabs().characters;
    const auto &buyouts = m_datastore.data().tab_buyouts;

    QLocale locale = QLocale::system();
    spdlog::info("Validating tab buyouts:");
    spdlog::info("Found {} stash tabs", locale.toString(stashes.size()));
    spdlog::info("Found {} characters", locale.toString(characters.size()));
    spdlog::info("Found {} tab buyouts", locale.toString(buyouts.size()));

    using Location = QString;

    std::set<Location> all_locations;
    std::set<Location> duplicated_locations;
    std::set<Location> duplicated_buyouts;
    std::set<Location> ambiguous_buyouts;
    std::set<Location> matched_buyouts;
    std::set<Location> orphaned_buyouts;

    // Add stash tab location tags.
    for (const auto &location : stashes) {
        const Location tag = "stash:" + location.name;
        if (all_locations.count(tag) <= 0) {
            all_locations.insert(tag);
        } else {
            duplicated_locations.insert(tag);
        }
    }

    // Add character location tags.
    for (const auto &location : characters) {
        const Location tag = "character:" + location.name;
        if (all_locations.count(tag) <= 0) {
            all_locations.insert(tag);
        } else {
            duplicated_locations.insert(tag);
        }
    }

    // Validate all the tab buyouts.
    for (const auto &buyout : buyouts) {
        const Location &tag = buyout.first;
        if (matched_buyouts.count(tag) > 0) {
            duplicated_buyouts.insert(tag);
        } else if (all_locations.count(tag) > 0) {
            matched_buyouts.insert(tag);
        } else {
            orphaned_buyouts.insert(tag);
        }
        // If the location tag is one of the duplicated locations,
        // then we don't know which tab this buyout really belongs to.
        if (duplicated_locations.count(tag) > 0) {
            ambiguous_buyouts.insert(tag);
        }
    }

    if (!duplicated_locations.empty()) {
        m_issues["Duplicated tabs"] = duplicated_locations;
        spdlog::warn("Found {} duplicated tab locations",
                     locale.toString(duplicated_locations.size()));
    }
    if (!duplicated_buyouts.empty()) {
        m_issues["Duplicated tab buyouts"] = duplicated_buyouts;
        spdlog::warn("Found duplicated tab buyouts", locale.toString(duplicated_buyouts.size()));
    }
    if (!ambiguous_buyouts.empty()) {
        m_issues["Ambiguous tab buyouts"] = ambiguous_buyouts;
        spdlog::warn("Found {} ambiguous tab buyouts", locale.toString(ambiguous_buyouts.size()));
    }
    if (!orphaned_buyouts.empty()) {
        m_issues["Orphaned tab buyouts"] = orphaned_buyouts;
        spdlog::warn("Found {} orphaned buyouts", locale.toString(orphaned_buyouts.size()));
    }
}

void LegacyBuyoutValidator::validateItemBuyouts()
{
    const auto &collections = m_datastore.items();
    const auto &buyouts = m_datastore.data().buyouts;

    QLocale locale = QLocale::system();
    spdlog::info("Validating item buyouts");
    spdlog::info("Found item buyouts", locale.toString(buyouts.size()));

    using BuyoutHash = QString;

    std::set<BuyoutHash> unique_buyouts;
    std::set<BuyoutHash> duplicated_buyouts;
    std::set<BuyoutHash> matched_buyouts;
    std::set<BuyoutHash> orphaned_buyouts;

    for (const auto &pair : buyouts) {
        const BuyoutHash &hash = pair.first;
        if (unique_buyouts.count(hash) <= 0) {
            unique_buyouts.insert(hash);
        } else {
            duplicated_buyouts.insert(hash);
        }
    }

    size_t item_count = 0;

    for (const auto &collection : collections) {
        const QString &loc = collection.first;
        const std::vector<LegacyItem> &items = collection.second;
        for (const auto &item : items) {
            const BuyoutHash hash = item.hash();
            if (matched_buyouts.count(hash) > 0) {
                duplicated_buyouts.insert(hash);
            } else if (buyouts.count(hash) > 0) {
                matched_buyouts.insert(hash);
            }
        }
        item_count += items.size();
    }
    spdlog::info("Found {} items", locale.toString(item_count));

    // Now go back and make sure all of the buyouts have beem matched.
    for (const BuyoutHash &hash : unique_buyouts) {
        if (matched_buyouts.count(hash) <= 0) {
            orphaned_buyouts.insert(hash);
        }
    }

    if (!duplicated_buyouts.empty()) {
        m_issues["Duplicated item buyouts"] = duplicated_buyouts;
        spdlog::warn("Found {} duplicated item buyouts", locale.toString(duplicated_buyouts.size()));
    }
    if (!orphaned_buyouts.empty()) {
        m_issues["Orphaned item buyouts"] = orphaned_buyouts;
        spdlog::warn("Found orphaned item buyouts", locale.toString(orphaned_buyouts.size()));
    }
}
