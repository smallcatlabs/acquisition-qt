// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>
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
// No Q_OBJECT: no new signals, slots, or properties.
class ScriptedNetwork : public NetworkManager
{
public:
    struct Answer
    {
        QByteArray body;
        int status = 200;
        QList<QNetworkReply::RawHeaderPair> headers;
        QNetworkReply::NetworkError error = QNetworkReply::NoError;
    };

    using Rule = std::function<std::optional<Answer>(QNetworkAccessManager::Operation, const QUrl &)>;

    struct Request
    {
        QNetworkAccessManager::Operation op;
        QUrl url;
        bool claimed;
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
                                 QIODevice *) override
    {
        std::optional<Answer> found;
        for (const Rule &rule : m_rules) {
            found = rule(op, request.url());
            if (found) {
                break;
            }
        }
        m_requests.push_back({op, request.url(), found.has_value()});
        if (!found) {
            found = Answer{.status = 404, .error = QNetworkReply::ContentNotFoundError};
        }
        auto *reply = new InFlightReply(request, this);
        // Completed from the event loop; the reply may be gone by then.
        const QPointer<InFlightReply> guarded(reply);
        QTimer::singleShot(0, reply, [guarded, answer = *found] {
            if (guarded && !guarded->isFinished()) {
                guarded->finish(answer.headers, answer.status, answer.error, answer.body);
            }
        });
        return reply;
    }

private:
    std::vector<Rule> m_rules;
    std::vector<Request> m_requests;
};
