// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "application.h"
#include "applicationtestaccess.h"
#include "buyout.h"
#include "currency.h"
#include "datastore/buyoutrepo.h"
#include "datastore/stashrepo.h"
#include "datastore/userstore.h"
#include "itemsmanager.h"
#include "poe/types/stashtab.h"
#include "refreshoutcome.h"
#include "scriptednetwork.h"
#include "testfixtures.h"
#include "ui/logindialog.h"
#include "ui/mainwindow.h"
#include "util/json_readers.h"

// The real Application in a test (#377): everything main() builds, over a
// temporary data directory, with the network answered by a ScriptedNetwork.
// Nothing else is replaced: the login dialog, the session, the rate limiter
// with its real timers, the worker, the stores on disk and the main window
// are the application's own, wired by the application.
//
// One scenario per process (acq_add_scenario_test): the game data tables are
// process-global, a process holds one user session, and the session's owners
// may only be destroyed once no further event is processed. The fixture is
// therefore a local of the test function and nothing runs after it.
//
// The scene is three stash tabs from tests/fixtures/payloads, real replies:
// they are in the user store before the application starts, as after an
// earlier run, and the scripted API serves the same replies to a refresh.
class AppFixture
{
public:
    static constexpr const char *kAccount = "ExampleAccount#0000";
    static constexpr const char *kRealm = "pc";
    static constexpr const char *kLeague = "Hardcore";

    // One tab of the scene, as the API sent it.
    struct Tab
    {
        QString id;
        QString name;
        QByteArray reply; // the reply's `stash` object, byte for byte
    };

    AppFixture()
    {
        QStandardPaths::setTestModeEnabled(true);
        if (!dir.isValid()) {
            qFatal("AppFixture: no temporary directory");
        }
        for (const char *file : {"stash-Hardcore-PremiumStash-1.json",
                                 "stash-Hardcore-PremiumStash-3.json",
                                 "stash-Hardcore-PremiumStash-5.json"}) {
            const QByteArray reply = readFixture(QString("payloads/%1").arg(file));
            const auto stash = json::readStash(reply);
            if (!stash) {
                qFatal("AppFixture: %s does not read as a stash", file);
            }
            tabs.push_back({stash->id, stash->name, reply});
        }
        seedUserStore();
        seedSettings();

        auto scripted = std::make_unique<ScriptedNetwork>();
        network = scripted.get();
        answerStartUp();
        answerApi();

        // A modal dialog would block the test until CTest's timeout; close it
        // and keep its title and text, so the scenario fails with what it said.
        QObject::connect(&m_dialog_watch, &QTimer::timeout, &m_dialog_watch, [this] {
            for (QWidget *window : QApplication::topLevelWidgets()) {
                if (!window->isVisible() || !window->isModal()) {
                    continue;
                }
                const auto *box = qobject_cast<QMessageBox *>(window);
                const QString said = window->windowTitle() + (box ? ": " + box->text() : QString());
                if (dialogs.isEmpty() || (dialogs.last() != said)) {
                    dialogs << said;
                }
                // done(), not close(): a message box with no single escape
                // button ignores a close, and no button is clicked this way.
                if (auto *dialog = qobject_cast<QDialog *>(window)) {
                    dialog->done(QDialog::Rejected);
                } else {
                    window->close();
                }
            }
        });
        m_dialog_watch.start(100);

        app = std::make_unique<Application>(QDir(dir.path()), std::move(scripted));
    }

    ApplicationTestAccess access() const { return ApplicationTestAccess(*app); }

    // What the login dialog does once the user is authenticated and presses
    // Login. The OAuth exchange before it needs a browser and stays manual.
    void logIn()
    {
        emit access().login().LoginComplete(makeTestIdentity(kAccount, kRealm, kLeague));
    }

    MainWindow &window() const { return access().mainWindow(); }

    QTreeView &tree() const
    {
        auto *view = window().findChild<QTreeView *>("treeView");
        if (!view) {
            qFatal("AppFixture: the main window has no treeView");
        }
        return *view;
    }

    // Waits for the game data, as a user who has to find the Login button
    // does, then logs in and waits until the cached items are in the tree.
    bool logInAndLoad()
    {
        if (!QTest::qWaitFor([this] { return access().gameDataLoaded(); }, 10000)) {
            return false;
        }
        logIn();
        return QTest::qWaitFor([this] { return tree().model()->rowCount() == int(tabs.size()); },
                               10000);
    }

    // The first row, a tab's header or an item, whose name contains `text`.
    // A scenario names rows it knows are shown, so none found is fatal.
    QModelIndex row(const QString &text) const
    {
        const QAbstractItemModel &model = *tree().model();
        for (int tab = 0; tab < model.rowCount(); ++tab) {
            const QModelIndex header = model.index(tab, 0);
            if (header.data().toString().contains(text)) {
                return header;
            }
            for (int item = 0; item < model.rowCount(header); ++item) {
                const QModelIndex index = model.index(item, 0, header);
                if (index.data().toString().contains(text)) {
                    return index;
                }
            }
        }
        qFatal("AppFixture: the tree shows no row named %s", qPrintable(text));
    }

    // Makes `text`'s row the current row and the only selected one, as a
    // click on it does.
    void select(const QString &text)
    {
        tree().selectionModel()->setCurrentIndex(row(text),
                                                 QItemSelectionModel::ClearAndSelect
                                                     | QItemSelectionModel::Rows);
    }

    // Runs `start`, which asks for a refresh, and waits for the refresh's one
    // terminal event. Empty if none came in time. The limiter's real timers
    // run, so a refresh takes about a quarter of a second for each request.
    std::optional<RefreshOutcome> refresh(const std::function<void()> &start)
    {
        std::optional<RefreshOutcome> outcome;
        const auto connection = QObject::connect(&access().itemsManager(),
                                                 &ItemsManager::RefreshFinished,
                                                 &access().itemsManager(),
                                                 [&outcome](const RefreshOutcome &finished) {
                                                     outcome = finished;
                                                 });
        start();
        const bool finished = QTest::qWaitFor([&outcome] { return outcome.has_value(); }, 30000);
        QObject::disconnect(connection);
        return finished ? outcome : std::nullopt;
    }

    // True for a refresh that completed with every source fetched.
    static bool isClean(const std::optional<RefreshOutcome> &outcome)
    {
        const auto *completed = outcome ? std::get_if<CompletedRefresh>(&*outcome) : nullptr;
        return completed && completed->skipped.empty();
    }

    // The stash and character requests made so far, as "/stash/Hardcore/<id>",
    // in the order they were made.
    QStringList apiGets() const
    {
        QStringList paths;
        for (const QString &url :
             network->requested(QNetworkAccessManager::GetOperation, apiUrl(""))) {
            const QString path = url.mid(apiUrl("").size());
            if (path.startsWith("/stash") || path.startsWith("/character")) {
                paths << path;
            }
        }
        return paths;
    }

    // The price box under the tree, with the type and the currency chosen
    // the way a user picks them from the two lists.
    QLineEdit &priceBox(int type = Buyout::BUYOUT_TYPE_BUYOUT,
                        int currency = Currency::CURRENCY_CHAOS_ORB) const
    {
        auto *types = window().findChild<QComboBox *>("buyoutTypeComboBox");
        auto *currencies = window().findChild<QComboBox *>("buyoutCurrencyComboBox");
        auto *value = window().findChild<QLineEdit *>("buyoutValueLineEdit");
        if (!types || !currencies || !value) {
            qFatal("AppFixture: the main window has no price editor");
        }
        if (types->isEnabled()) {
            types->setCurrentIndex(type);
            emit types->activated(type);
            currencies->setCurrentIndex(currency);
            emit currencies->activated(currency);
        }
        return *value;
    }

    // Types `text` into the price box for the selected rows and presses Enter.
    void typePrice(const QString &text)
    {
        QLineEdit &box = priceBox();
        box.selectAll();
        QTest::keyClicks(&box, text);
        QTest::keyClick(&box, Qt::Key_Return);
    }

    // The two boxes, minimum and maximum, of the search form's number filter
    // labelled `caption`.
    QList<QLineEdit *> numberBoxes(const QString &caption) const
    {
        for (const QLabel *label : window().findChildren<QLabel *>()) {
            if ((label->text() == caption) && label->parentWidget()) {
                const auto boxes = label->parentWidget()->findChildren<QLineEdit *>();
                if (boxes.size() == 2) {
                    return boxes;
                }
            }
        }
        qFatal("AppFixture: no number filter labelled %s", qPrintable(caption));
    }

    // The search form's Priced checkbox.
    QCheckBox &pricedBox() const
    {
        for (const QLabel *label : window().findChildren<QLabel *>()) {
            if ((label->text() == "Priced") && label->parentWidget()) {
                if (auto *box = label->parentWidget()->findChild<QCheckBox *>()) {
                    return *box;
                }
            }
        }
        qFatal("AppFixture: the search form has no Priced checkbox");
    }

    // The status bar's message and labels, joined.
    QString statusText() const
    {
        QStringList texts{window().statusBar()->currentMessage()};
        for (const QLabel *label : window().statusBar()->findChildren<QLabel *>()) {
            texts.append(label->text());
        }
        return texts.join('|');
    }

    // What is in the user store on disk, read through a second connection:
    // the prices of items by item id, and of tabs by tab id.
    std::unordered_map<QString, Buyout> storedItemPrices() const
    {
        UserStore store(QDir(dir.filePath("data")), kAccount);
        return store.buyouts().loadItemBuyouts().value();
    }
    std::unordered_map<QString, Buyout> storedTabPrices() const
    {
        UserStore store(QDir(dir.filePath("data")), kAccount);
        return store.buyouts().loadLocationBuyouts().value();
    }

    // One stored price as the application writes it out ("b/o 7 chaos"), or
    // "(none)".
    static QString priceText(const std::unordered_map<QString, Buyout> &prices, const QString &id)
    {
        const auto it = prices.find(id);
        return (it == prices.end()) ? QString("(none)") : it->second.AsText();
    }

    // The id of the item named `name` in tab `index` of the scene.
    QString itemId(size_t index, const QString &name) const
    {
        const QJsonArray items
            = QJsonDocument::fromJson(tabs.at(index).reply).object().value("items").toArray();
        for (const QJsonValue &item : items) {
            if (item.toObject().value("name").toString() == name) {
                return item.toObject().value("id").toString();
            }
        }
        qFatal("AppFixture: tab %zu has no item named %s", index, qPrintable(name));
    }

    // Renames tab `index` from now on, in the list and in its own reply.
    void renameTab(size_t index, const QString &name)
    {
        Tab &tab = tabs.at(index);
        QJsonObject stash = QJsonDocument::fromJson(tab.reply).object();
        stash.insert("name", name);
        tab.name = name;
        tab.reply = QJsonDocument(stash).toJson(QJsonDocument::Compact);
        answerApi();
    }

    // Replaces what the API serves for one tab from now on.
    void serveStash(const QString &id, const QByteArray &stash_object)
    {
        network->answer(apiUrl(QString("/stash/%1/%2").arg(kLeague, id)),
                        apiAnswer("stash-request-limit", "{\"stash\":" + stash_object + "}"));
    }

    static QString apiUrl(const QString &path) { return "https://api.pathofexile.com" + path; }

    static QByteArray readFixture(const QString &name)
    {
        QFile file(QFINDTESTDATA(QString("fixtures/%1").arg(name)));
        if (!file.open(QIODevice::ReadOnly)) {
            qFatal("AppFixture: cannot read fixture %s", qPrintable(name));
        }
        return file.readAll();
    }

    QTemporaryDir dir;
    std::vector<Tab> tabs;
    ScriptedNetwork *network = nullptr; // owned by the application
    QStringList dialogs;                // every modal dialog that opened
    std::unique_ptr<Application> app;

private:
    // The rate-limit headers the API sends with every reply, HEAD included:
    // one rule with room to spare, so the limiter never has to wait for more
    // than its own spacing between sends.
    static ScriptedNetwork::Answer apiAnswer(const QByteArray &policy, const QByteArray &body)
    {
        return {.body = body,
                .headers = {
                    {"X-Rate-Limit-Policy", policy},
                    {"X-Rate-Limit-Rules", "Account"},
                    {"X-Rate-Limit-Account", "30:10:60"},
                    {"X-Rate-Limit-Account-State", "1:10:0"},
                    {"Date", QDateTime::currentDateTimeUtc().toString(Qt::RFC2822Date).toUtf8()},
                }};
    }

    // A stash as the list reply carries it: without its items.
    static QJsonObject withoutItems(const QByteArray &stash_object)
    {
        QJsonObject stash = QJsonDocument::fromJson(stash_object).object();
        stash.remove("items");
        return stash;
    }

    void seedUserStore()
    {
        const QDir data_dir(dir.filePath("data"));
        if (!QDir().mkpath(data_dir.path())) {
            qFatal("AppFixture: cannot create the data directory");
        }
        UserStore store(data_dir, kAccount);
        if (!store.isUsable()) {
            qFatal("AppFixture: the user store is not usable");
        }
        std::vector<poe::StashTab> list;
        for (const Tab &tab : tabs) {
            poe::StashTab stash = *json::readStash(tab.reply);
            if (!store.stashes().saveStash(stash, tab.reply, kRealm, kLeague)) {
                qFatal("AppFixture: could not save tab %s", qPrintable(tab.id));
            }
            stash.items.reset();
            list.push_back(std::move(stash));
        }
        if (!store.stashes().saveStashList(list, kRealm, kLeague)) {
            qFatal("AppFixture: could not save the tab list");
        }
    }

    void seedSettings()
    {
        QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("realm", kRealm);
        settings.setValue("league", kLeague);
        settings.setValue("account", kAccount);
    }

    // What the application asks for before anyone has logged in: the game
    // data, the release list and the league list.
    void answerStartUp()
    {
        const QString repoe = "https://repoe-fork.github.io/";
        network->answer(repoe + "version.txt", {.body = "3.29.3.6"});
        network->answer(repoe + "item_classes.min.json",
                        {.body = readFixture("repoe/item_classes.min.json")});
        network->answer(repoe + "base_items.min.json",
                        {.body = readFixture("repoe/base_items.json")});
        network->answer(repoe + "stat_translations.min.json",
                        {.body = readFixture("repoe/stat_translations.json")});
        network->answer(repoe + "stat_translations/necropolis.min.json", {.body = "[]"});
        network->answer("https://api.github.com/repos/smallcatlabs/acquisition-qt/releases",
                        {.body = "[]"});
        network->answer("https://api.pathofexile.com/leagues?type=main&compact=1",
                        {.body
                         = R"([{"id":"Standard","realm":"pc"},{"id":"Hardcore","realm":"pc"}])"});
        // Every item icon is the same small picture.
        QByteArray png;
        QBuffer buffer(&png);
        QImage icon(8, 8, QImage::Format_ARGB32);
        icon.fill(Qt::darkGray);
        if (!buffer.open(QIODevice::WriteOnly) || !icon.save(&buffer, "PNG")) {
            qFatal("AppFixture: cannot make an icon");
        }
        network->answer([png](QNetworkAccessManager::Operation,
                              const QUrl &url) -> std::optional<ScriptedNetwork::Answer> {
            if (url.host() == "web.poecdn.com") {
                return ScriptedNetwork::Answer{.body = png};
            }
            return std::nullopt;
        });
    }

    // The account's stashes and characters, as the API serves them.
    void answerApi()
    {
        QJsonArray list;
        for (const Tab &tab : tabs) {
            list.append(withoutItems(tab.reply));
            serveStash(tab.id, tab.reply);
        }
        network->answer(apiUrl(QString("/stash/%1").arg(kLeague)),
                        apiAnswer("stash-list-request-limit",
                                  QJsonDocument(QJsonObject{{"stashes", list}})
                                      .toJson(QJsonDocument::Compact)));
        network->answer(apiUrl("/character"),
                        apiAnswer("character-list-request-limit", R"({"characters":[]})"));
    }

    QTimer m_dialog_watch;
};
