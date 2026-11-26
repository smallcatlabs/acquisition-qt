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
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
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
// therefore a local of the test function, destroyed when it returns; only
// Qt Test's own clean-up follows.
//
// What it does not exercise: the request headers and cookies NetworkManager
// adds (scriptednetwork.h). logIn() starts where the login dialog ends; the
// dialog and the OAuth exchange before it are tst_applogin's.
// The scripted API has one rate-limit rule with room to spare, so the
// limiter's waits beyond its own spacing between sends never happen here.
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

    // `before_start` runs once the scene and the script are in place and
    // before the application is built: what the application reads or asks
    // for while it is built (a stored OAuth token, the token endpoint) has to
    // be there by then.
    explicit AppFixture(const std::function<void(AppFixture &)> &before_start = {})
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

        // A modal dialog would block the test until CTest's timeout; close it
        // and keep its title and text, so the scenario fails with what it said.
        QObject::connect(&m_dialog_watch, &QTimer::timeout, &m_dialog_watch, [this] {
            for (QWidget *window : QApplication::topLevelWidgets()) {
                if (!window->isVisible() || !window->isModal()) {
                    continue;
                }
                // A dialog that asks for a text and has an answer waiting
                // gets it, as from a user who types it and presses OK.
                if (auto *input = qobject_cast<QInputDialog *>(window);
                    input && !m_input_answers.isEmpty()) {
                    input->setTextValue(m_input_answers.takeFirst());
                    input->accept();
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

        start(before_start);
    }

    // Ends the application and builds it again over the same data directory,
    // with a new script, as when the user quits Acquisition and starts it
    // again. `before_start` runs as the constructor's does. Only while
    // nobody has logged in: a process holds one user session.
    void restart(const std::function<void(AppFixture &)> &before_start = {})
    {
        if (access().hasSession()) {
            qFatal("AppFixture: restart() after a login");
        }
        app.reset();
        start(before_start);
    }

    ApplicationTestAccess access() const { return ApplicationTestAccess(*app); }

    // What the login dialog does once the user is authenticated and presses
    // Login. The OAuth exchange before it is tst_applogin's.
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

    // The item named `text` in a menu of the application, the tree's context
    // menu included: what a click on that menu entry triggers.
    QAction &menuAction(const QString &text) const
    {
        for (QWidget *widget : QApplication::allWidgets()) {
            if (const auto *menu = qobject_cast<QMenu *>(widget)) {
                for (QAction *action : menu->actions()) {
                    if (action->text() == text) {
                        return *action;
                    }
                }
            }
        }
        qFatal("AppFixture: no menu has an item named %s", qPrintable(text));
    }

    // Triggers the menu item named `menu_text`, which asks for a text in a
    // dialog, and gives the dialog `answer`. False if no dialog took it.
    bool answerMenuDialog(const QString &menu_text, const QString &answer)
    {
        m_input_answers << answer;
        // The dialog runs its own event loop inside trigger(); the dialog
        // watch answers it from there.
        menuAction(menu_text).trigger();
        const bool taken = m_input_answers.isEmpty();
        m_input_answers.clear();
        return taken;
    }

    // The forum from now on, for the thread the captured pages in
    // tests/fixtures/forum belong to: the website's list of stash tabs, the
    // thread's edit page, and the page a saved edit answers with. A scenario
    // that never calls this fails on any request to the website.
    static constexpr const char *kForumThread = "3445874";
    static QString forumEditUrl()
    {
        return QString("https://www.pathofexile.com/forum/edit-thread/") + kForumThread;
    }
    void serveForum()
    {
        // The website numbers the tabs it lists from 0 (T24). The reply is
        // written from the fields the shop reads; no reply of this endpoint
        // has been captured.
        QJsonArray listed;
        for (const Tab &tab : tabs) {
            const QJsonObject stash = QJsonDocument::fromJson(tab.reply).object();
            listed.append(QJsonObject{{"n", tab.name}, {"i", stash.value("index")}, {"id", tab.id}});
        }
        const QByteArray index = QJsonDocument(
                                     QJsonObject{{"numTabs", listed.size()}, {"tabs", listed}})
                                     .toJson(QJsonDocument::Compact);
        network->answer([index](QNetworkAccessManager::Operation,
                                const QUrl &asked) -> std::optional<ScriptedNetwork::Answer> {
            if (asked.toString(QUrl::RemoveQuery)
                == "https://www.pathofexile.com/character-window/get-stash-items") {
                return apiAnswer("backend-item-request-limit", index);
            }
            return std::nullopt;
        });
        const QByteArray edit_page = readFixture("forum/edit-thread.html");
        const QByteArray saved_page = readFixture("forum/after-edit-thread.html");
        network->answer(
            [edit_page, saved_page](QNetworkAccessManager::Operation op,
                                    const QUrl &asked) -> std::optional<ScriptedNetwork::Answer> {
                if (asked.toString() != forumEditUrl()) {
                    return std::nullopt;
                }
                const bool post = (op == QNetworkAccessManager::PostOperation);
                return ScriptedNetwork::Answer{.body = post ? saved_page : edit_page};
            });
    }

    // The forms posted to the forum so far, as sent.
    QList<QByteArray> forumPosts() const
    {
        QList<QByteArray> bodies;
        for (const ScriptedNetwork::Request &request : network->requests()) {
            if ((request.op == QNetworkAccessManager::PostOperation)
                && (request.url.toString() == forumEditUrl())) {
                bodies << request.body;
            }
        }
        return bodies;
    }

    // One field of a posted form, decoded as a form decoder reads it: "+"
    // is a space and "%xx" a byte.
    static QString formField(const QByteArray &form, const QByteArray &name)
    {
        for (const QByteArray &pair : form.split('&')) {
            const qsizetype equals = pair.indexOf('=');
            if ((equals >= 0) && (pair.first(equals) == name)) {
                QByteArray value = pair.mid(equals + 1);
                value.replace('+', ' ');
                return QString::fromUtf8(QByteArray::fromPercentEncoding(value));
            }
        }
        return "(no field " + QString::fromUtf8(name) + ")";
    }

    // Every request made to the website so far, as "GET /forum/edit-thread/1"
    // (the stash list without its query), in order.
    QStringList websiteRequests() const
    {
        QStringList found;
        for (const ScriptedNetwork::Request &request : network->requests()) {
            if (request.url.host() != "www.pathofexile.com") {
                continue;
            }
            const char *verb = (request.op == QNetworkAccessManager::HeadOperation)   ? "HEAD "
                               : (request.op == QNetworkAccessManager::PostOperation) ? "POST "
                                                                                      : "GET ";
            found << verb + request.url.path();
        }
        return found;
    }

    // Every stash and character request made so far, probes included, as
    // "HEAD /stash/Hardcore" or "GET /stash/Hardcore/<id>", in order.
    QStringList apiRequests() const
    {
        QStringList found;
        for (const ScriptedNetwork::Request &request : network->requests()) {
            const QString url = request.url.toString();
            const QString path = url.mid(apiUrl("").size());
            if (url.startsWith(apiUrl(""))
                && (path.startsWith("/stash") || path.startsWith("/character"))) {
                const bool head = (request.op == QNetworkAccessManager::HeadOperation);
                found << (head ? "HEAD " : "GET ") + path;
            }
        }
        return found;
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
            if (currencies->isEnabled()) {
                currencies->setCurrentIndex(currency);
                emit currencies->activated(currency);
            }
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

    // The search form's list of item categories, labelled "Type".
    QComboBox &categoryBox() const
    {
        for (const QLabel *label : window().findChildren<QLabel *>()) {
            if ((label->text() == "Type") && label->parentWidget()) {
                if (auto *box = label->parentWidget()->findChild<QComboBox *>()) {
                    return *box;
                }
            }
        }
        qFatal("AppFixture: the search form has no category list");
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
    // Every tab is served from `tabs` again, so a serveStash() made before
    // this is undone.
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
        answerApiUrl(QString("/stash/%1/%2").arg(kLeague, id),
                     "stash-request-limit",
                     "{\"stash\":" + stash_object + "}");
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
    // Builds the application over the data directory, with a new script.
    void start(const std::function<void(AppFixture &)> &before_start)
    {
        auto scripted = std::make_unique<ScriptedNetwork>();
        network = scripted.get();
        answerStartUp();
        answerApi();
        if (before_start) {
            before_start(*this);
        }
        app = std::make_unique<Application>(QDir(dir.path()), std::move(scripted));
    }

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

    // Answers an API path with `body` and the rate-limit headers. The answer
    // is made when the request arrives, so its Date is the time of the reply.
    void answerApiUrl(const QString &path, const QByteArray &policy, const QByteArray &body)
    {
        const QString url = apiUrl(path);
        network->answer(
            [url, policy, body](QNetworkAccessManager::Operation,
                                const QUrl &asked) -> std::optional<ScriptedNetwork::Answer> {
                if (asked.toString() == url) {
                    return apiAnswer(policy, body);
                }
                return std::nullopt;
            });
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
        network->answer("https://api.pathofexile.com/leagues?type=main&compact=1&realm=pc",
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
        answerApiUrl(QString("/stash/%1").arg(kLeague),
                     "stash-list-request-limit",
                     QJsonDocument(QJsonObject{{"stashes", list}}).toJson(QJsonDocument::Compact));
        answerApiUrl("/character", "character-list-request-limit", R"({"characters":[]})");
    }

    QTimer m_dialog_watch;
    QStringList m_input_answers; // for the next dialogs that ask for a text
};
