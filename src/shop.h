// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QNetworkReply>
#include <QObject>
#include <QUrlQuery>

#include <expected>
#include <map>
#include <memory>
#include <vector>

#include "buyout.h"
#include "itemlocation.h"
#include "poe/types/website/webstashtab.h"
#include "ratelimit/fetcherror.h"
#include "refreshoutcome.h"
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
    bool auto_update() const { return m_auto_update; }

    QStringList threads() const { return m_threads; }
    QStringList shop_data() const { return m_shop_data; }
    QString shop_template() const { return m_shop_template; }
    // True when the preview/clipboard cache was rendered from an input
    // revision older than the current one (items-pipeline M2, D8). Deltas
    // deliberately do not advance the input revision (R4-2), so this is
    // snapshot-granular, not delta-granular.
    bool shop_data_outdated() const { return m_cache_revision < m_input_revision; }

public slots:
    void OnEditPageFinished();
    void OnShopSubmitted(QUrlQuery query, QNetworkReply *reply);
    // The automatic-submission gate (items-pipeline M2, D8/R1-1): auto
    // update fires only on a CLEAN completed refresh. A completed-with-skips
    // outcome must never auto-post — the skipped tabs' stale contents would
    // reach the forum as if fresh — and a failed refresh posts nothing.
    void OnRefreshFinished(const RefreshOutcome &outcome);

signals:
    void StatusUpdate(ProgramState state, const QString &status);
    void UserWarning(const QString &message);
    void PoeSessionRejected();

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
        struct CapturedItem
        {
            QString id;
            QString pretty_name;
            ItemLocation location;
            Buyout buyout;
        };
        // Immutable capture.
        std::vector<CapturedItem> items;
        QString shop_template;
        QString realm;
        QString league;
        QStringList threads;
        bool force{false};
        quint64 input_revision{0};
        // Job-local transport state.
        std::map<QString, unsigned int> tab_index;
        QStringList shop_data;
        QString shop_hash;
        qsizetype requests_completed{0};
        // True for a submission started by a clean refresh rather than by
        // the user; it decides how often a standing warning is repeated.
        bool automatic{false};
    };

    std::unique_ptr<ShopJob> CaptureJob(bool force) const;
    // Every submission renders from its capture (M2 D8): correctness never
    // depends on preview-cache freshness.
    void RenderJob(ShopJob &job) const;
    void PublishPreviewCache(const ShopJob &job);

    // Automatic admission (M2 D8/R3-1): captures BEFORE applying the busy
    // policy — when a job is active, the capture becomes (or replaces) the
    // single waiting automatic capture instead of being refused. Manual
    // admission (SubmitShopToForum) still refuses while a job is active.
    void SubmitAutomaticShop();

    // The fail-closed gate both submission paths ask first (#250): false
    // when the buyouts did not load, after telling the user — every time
    // for a manual request, once per session for automatic ones.
    bool BuyoutsReadyToPost(bool automatic);

    // The two terminal exits every submission converges on (M2 D8).
    // Success — including the unchanged-hash no-post — releases the job and
    // drains the newest waiting automatic capture; failure releases the job,
    // drains nothing, and discards the waiting capture (R4-3: no kind
    // discrimination), so the next clean automatic or manual request
    // recaptures the latest published state.
    void CompleteActiveJob();
    void FailActiveJob();

    void UpdateStashIndex();
    void OnStashIndexReceived(
        const std::expected<poe::WebStashListWrapper, RateLimit::FetchError> &result);
    void OnStashIndexUpdated();
    void RejectPoeSession(const QString &reason);

    void SubmitSingleShop();
    void SubmitNextShop(const QString &title, const QString &hash);
    QString ShopEditUrl(qsizetype idx);
    static QString SpoilerBuyout(const Buyout &bo);

    QSettings &m_settings;
    NetworkManager &m_network_manager;
    PoeApiClient &m_api;
    DataStore &m_datastore;
    ItemsManager &m_items_manager;
    BuyoutManager &m_buyout_manager;

    QStringList m_threads;
    QString m_shop_template;
    bool m_auto_update;
    bool m_warned_unloaded_buyouts{false};
    bool m_warned_page_shortfall{false};

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

    static const QRegularExpression error_regex;
    static const QRegularExpression ratelimit_regex;
};
