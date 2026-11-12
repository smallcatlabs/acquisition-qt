// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QStringList>

#include <chrono>
#include <expected>
#include <memory>
#include <set>

#include "poe/types/website/webstashtab.h"
#include "ratelimit/fetcherror.h"
#include "refreshoutcome.h"
#include "shoprender.h"
#include "util/programstate.h"

class QSettings;

class Application;
class BuyoutManager;
class DataStore;
class ItemsManager;
class NetworkManager;
class PoeApiClient;

class Shop : public QObject
{
    Q_OBJECT
public:
    explicit Shop(QSettings &settings,
                  NetworkManager &network_manager,
                  PoeApiClient &api,
                  DataStore &datastore,
                  ItemsManager &items_manager,
                  BuyoutManager &buyout_manager);
    void SetThread(const QStringList &threads);
    void SetAutoUpdate(bool update);
    void SetShopTemplate(const QString &shop_template);
    void CopyToClipboard();
    // An output-affecting LOCAL edit (buyout, template, threads): advances
    // the input revision and drops any waiting automatic capture (M2
    // D8/R6-4) — draining a pre-edit capture after the user's edit would
    // post data older than their intent. The active job is deliberately
    // unaffected: its capture is immutable and posts as captured.
    void ExpireShopData();
    // A published SNAPSHOT boundary (final ItemsRefreshed): advances the
    // input revision only. A waiting clean capture survives — the gate's
    // invariant is cleanliness, not recency (M2 D8/R5-6 keep-and-drain).
    void OnPublishedSnapshot();
    void SubmitShopToForum(bool force = false);
    // How long to wait before fetching the edit page again after the forum
    // said the security token had expired. Tests shorten it.
    void SetRetryDelay(std::chrono::milliseconds delay) { m_retry_delay = delay; }
    bool auto_update() const { return m_auto_update; }

    QStringList threads() const { return m_threads; }
    QStringList shop_data() const { return m_shop_data; }
    QString shop_template() const { return m_shop_template; }
    // True when the preview/clipboard cache was rendered from an input
    // revision older than the current one (items-pipeline M2, D8). Deltas
    // deliberately do not advance the input revision (R4-2), so this is
    // snapshot-granular, not delta-granular.
    bool shop_data_outdated() const { return m_cache_revision < m_input_revision; }

    // Why a submission stopped. `message` is shown to the user for an
    // Error; for a rejected session it says which reply rejected it.
    struct Failure
    {
        enum class Kind { Error, SessionRejected, Canceled };
        Kind kind{Kind::Error};
        QString message;
    };

public slots:
    // The automatic-submission gate (items-pipeline M2, D8/R1-1): auto
    // update fires only on a CLEAN completed refresh. A completed-with-skips
    // outcome must never auto-post — the skipped tabs' stale contents would
    // reach the forum as if fresh — and a failed refresh posts nothing.
    void OnRefreshFinished(const RefreshOutcome &outcome);

signals:
    void StatusUpdate(ProgramState state, const QString &status);
    void UserWarning(const QString &message);
    void PoeSessionRejected();
    // Emitted when the automatic-update setting changes, whoever changed
    // it, so the menu item can follow it.
    void AutoUpdateChanged(bool enabled);

private:
    // One forum submission job (items-pipeline M2, D8/R2-1): its input is
    // captured BY VALUE at request time — the postable items' identity,
    // location, and buyout fields plus every other output-affecting input
    // (template, realm/league, target thread list) — and is immutable from
    // then on. The job also owns all of its transport state (stash index,
    // rendered data and hash, request counter), so nothing it reads can be
    // mutated by streamed deltas, local edits, or a later job. Value
    // capture, not retained shared_ptrs: a successful FinishUpdate rebases
    // the shared Item objects in place, so a pointer capture would mutate
    // under the submission.
    struct ShopJob
    {
        // Immutable capture: the postable items, template and realm/league
        // (the stash index is added to it when it arrives), and the target
        // thread list.
        ShopRenderInput input;
        QStringList threads;
        bool force{false};
        quint64 input_revision{0};
        // Job-local transport state. Every submission renders its own
        // capture (M2 D8): correctness never depends on preview-cache
        // freshness.
        RenderedShop rendered;
        qsizetype requests_completed{0};
        // POSTs made for the current thread.
        int attempts{0};
        // True for a submission started by a clean refresh rather than by
        // the user; it decides how often a standing warning is repeated.
        bool automatic{false};
    };

    std::unique_ptr<ShopJob> CaptureJob(bool force) const;
    void PublishPreviewCache(const ShopJob &job);

    // Automatic admission (M2 D8/R3-1): captures BEFORE applying the busy
    // policy — when a job is active, the capture becomes (or replaces) the
    // single waiting automatic capture instead of being refused. Manual
    // admission (SubmitShopToForum) still refuses while a job is active.
    void SubmitAutomaticShop();

    // What the user is told about. A manual request hears every notice;
    // an automatic submission hears each kind once per session, so a
    // standing condition does not raise a dialog after every refresh.
    enum class Notice {
        BuyoutsNotLoaded,
        NoThreads,
        NoSession,
        Busy,
        PageShortfall,
        SessionRejected,
        SubmissionFailed
    };
    void Warn(Notice notice, bool automatic, const QString &message);

    // The admission gate both submission paths ask first: the buyouts
    // loaded (fail-closed, #250), threads set, and a POESESSID present.
    // False after telling the user why, by Warn's rule.
    bool ReadyToPost(bool automatic);

    // The two terminal exits every submission converges on (M2 D8).
    // Success — including the unchanged-hash no-post — releases the job and
    // drains the newest waiting automatic capture; failure releases the job,
    // drains nothing, and discards the waiting capture (R4-3: no kind
    // discrimination), so the next clean automatic or manual request
    // recaptures the latest published state. FailActiveJob is the only
    // failure exit: it always sets the status line, tells the user by
    // Warn's rule, and for a rejected session also turns automatic updates
    // off and asks for the POESESSID to be cleared.
    void CompleteActiveJob();
    void FailActiveJob(const Failure &failure);

    void UpdateStashIndex();
    void OnStashIndexReceived(
        const std::expected<poe::WebStashListWrapper, RateLimit::FetchError> &result);
    void OnStashIndexUpdated();

    // The forum conversation for the current thread: GET its edit page,
    // POST the page there, and either move to the next thread or, when the
    // forum asks for a retry, start this thread again from the GET (fresh
    // token and title), at most kMaxAttemptsPerThread POSTs per thread.
    void SubmitSingleShop();
    void OnEditPageFinished(QNetworkReply *reply);
    void SubmitNextShop(const QString &title, const QString &hash);
    void OnShopSubmitted(QNetworkReply *reply);

    QSettings &m_settings;
    NetworkManager &m_network_manager;
    PoeApiClient &m_api;
    DataStore &m_datastore;
    ItemsManager &m_items_manager;
    BuyoutManager &m_buyout_manager;

    QStringList m_threads;
    QString m_shop_template;
    bool m_auto_update;
    std::set<Notice> m_warned;
    std::chrono::milliseconds m_retry_delay{5000};

    // The submission desired state (M2 D8/R3-1): at most one active job and
    // at most one waiting automatic capture behind it — the newest clean
    // capture wins, so intermediate clean snapshots coalesce rather than
    // making the forum observe every refresh generation.
    std::unique_ptr<ShopJob> m_active_job;
    std::unique_ptr<ShopJob> m_waiting_capture;

    // Preview/clipboard cache, published only by rendered jobs that are not
    // older than the cache already here. Monotonic revisions replace the old
    // single outdated flag: ExpireShopData() advances the input revision;
    // rendering job N can mark only N's revision clean — never a newer one.
    QStringList m_shop_data;
    quint64 m_input_revision{1};
    quint64 m_cache_revision{0};
};
