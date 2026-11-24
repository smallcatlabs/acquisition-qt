// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QBuffer>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "fakesender.h"
#include "util/networkmanager.h"

// An offline NetworkManager that answers requests by rule (#377), for a test
// that builds the whole application: where FakeNetworkManager leaves every
// reply for the test to complete by hand, this one completes each from the
// first rule that claims it, so the application's own request chains (game
// data, the update check, the league list, a refresh) run to their end.
//
// Every reply is completed from the event loop, never inside the call that
// made the request: a real reply is never finished synchronously, and the
// rate limiter's set-up path relies on that. A request no rule claims is
// answered with an error and listed by unanswered(), so a scenario can state
// that the application asked for nothing it was not expected to.
//
// What this does not exercise: it replaces NetworkManager::createRequest, so
// the User-Agent and the bearer token that function adds to a request are
// never set, and an API request without a token is answered like any other.
//
// No Q_OBJECT: no new signals, slots, or properties.
class ScriptedNetwork : public NetworkManager
{
public:
    // Every member has a default, so an answer names only what it sets: GCC
    // reports a member left out of a designated initializer when it has none.
    struct Answer
    {
        QByteArray body = {};
        int status = 200;
        QList<QNetworkReply::RawHeaderPair> headers = {};
        QNetworkReply::NetworkError error = QNetworkReply::NoError;
        // Kept back until release(): for a scenario that acts while the
        // request is on its way.
        bool hold = false;
        // Sent, through Qt's own HTTP stack, to a local server that takes it
        // and never answers: only the request's transfer timeout, if it has
        // one, ends it. The other members are then unused.
        bool silent = false;
    };

    // How long a silent server is waited for, in place of the transfer
    // timeout the request carries, for a scenario that cannot wait that
    // long. Only a request with a timeout is shortened; requests() records
    // the request's own.
    std::optional<std::chrono::milliseconds> silent_wait;

    using Rule = std::function<std::optional<Answer>(QNetworkAccessManager::Operation, const QUrl &)>;

    struct Request
    {
        QNetworkAccessManager::Operation op;
        QUrl url;
        bool claimed;
        QByteArray body;          // what a POST or PUT sent, as sent
        int transfer_timeout = 0; // the request's, in milliseconds; 0 for none
    };

    // The rule added last is asked first, so a scenario's rule overrides the
    // fixture's for the same URL.
    void answer(Rule rule) { m_rules.insert(m_rules.begin(), std::move(rule)); }

    // Answers every request for exactly this URL, whatever the operation.
    void answer(const QString &url, Answer reply)
    {
        answer([url, reply](QNetworkAccessManager::Operation,
                            const QUrl &asked) -> std::optional<Answer> {
            if (asked.toString() == url) {
                return reply;
            }
            return std::nullopt;
        });
    }

    const std::vector<Request> &requests() const { return m_requests; }

    // "GET https://..." for each request with this operation whose URL
    // contains `part`, in the order they were made.
    QStringList requested(QNetworkAccessManager::Operation op, const QString &part = {}) const
    {
        QStringList found;
        for (const Request &request : m_requests) {
            if ((request.op == op) && request.url.toString().contains(part)) {
                found << request.url.toString();
            }
        }
        return found;
    }

    // Completes every answer kept back so far, from the event loop as any
    // other; a reply that is gone by then is skipped. Returns how many were
    // kept back.
    qsizetype release()
    {
        const qsizetype count = qsizetype(m_held.size());
        for (const auto &[reply, answer] : std::exchange(m_held, {})) {
            complete(reply, answer);
        }
        return count;
    }

    QStringList unanswered() const
    {
        QStringList found;
        for (const Request &request : m_requests) {
            if (!request.claimed) {
                found << request.url.toString();
            }
        }
        return found;
    }

protected:
    QNetworkReply *createRequest(QNetworkAccessManager::Operation op,
                                 const QNetworkRequest &request,
                                 QIODevice *outgoing) override
    {
        std::optional<Answer> found;
        for (const Rule &rule : m_rules) {
            found = rule(op, request.url());
            if (found) {
                break;
            }
        }
        const QByteArray sent = outgoing ? outgoing->readAll() : QByteArray();
        m_requests.push_back(
            {op, request.url(), found.has_value(), sent, request.transferTimeout()});
        if (found && found->silent) {
            return sendToSilentServer(op, request, sent);
        }
        if (!found) {
            found = Answer{.status = 404, .error = QNetworkReply::ContentNotFoundError};
        }
        if (op == QNetworkAccessManager::HeadOperation) {
            found->body.clear(); // a HEAD reply has headers only
        }
        auto *reply = new InFlightReply(request, this);
        if (found->hold) {
            m_held.emplace_back(reply, *found);
        } else {
            complete(reply, *found);
        }
        return reply;
    }

private:
    // A real reply from Qt, for a request redirected to a local server that
    // accepts the connection, reads nothing back and never answers.
    QNetworkReply *sendToSilentServer(QNetworkAccessManager::Operation op,
                                      const QNetworkRequest &request,
                                      const QByteArray &sent)
    {
        if (!m_silent_server.isListening()) {
            if (!m_silent_server.listen(QHostAddress::LocalHost)) {
                qFatal("ScriptedNetwork: cannot listen on 127.0.0.1");
            }
            QObject::connect(&m_silent_server, &QTcpServer::newConnection, &m_silent_server, [this] {
                while (QTcpSocket *socket = m_silent_server.nextPendingConnection()) {
                    socket->setParent(&m_silent_server);
                }
            });
        }
        QNetworkRequest redirected = request;
        if (silent_wait && (request.transferTimeout() > 0)) {
            redirected.setTransferTimeout(*silent_wait);
        }
        QUrl url = request.url();
        url.setScheme("http");
        url.setHost("127.0.0.1");
        url.setPort(m_silent_server.serverPort());
        redirected.setUrl(url);
        auto *body = new QBuffer(this);
        body->setData(sent);
        body->open(QIODevice::ReadOnly);
        QNetworkReply *reply = QNetworkAccessManager::createRequest(op, redirected, body);
        body->setParent(reply);
        return reply;
    }

    // Completed from the event loop; the reply may be gone by then.
    static void complete(const QPointer<InFlightReply> &guarded, const Answer &sent)
    {
        if (!guarded) {
            return;
        }
        QTimer::singleShot(0, guarded.data(), [guarded, sent] {
            if (guarded && !guarded->isFinished()) {
                guarded->finish(sent.headers, sent.status, sent.error, sent.body);
            }
        });
    }

    std::vector<Rule> m_rules;
    std::vector<Request> m_requests;
    std::vector<std::pair<QPointer<InFlightReply>, Answer>> m_held;
    QTcpServer m_silent_server;
};
