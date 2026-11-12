// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "shop.h"

#include <QApplication>
#include <QClipboard>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <optional>
#include <variant>

#include "buyoutmanager.h"
#include "datastore/datastore.h"
#include "item.h"
#include "itemsmanager.h"
#include "poe/poeapiclient.h"
#include "poe/types/website/webstashtab.h"
#include "replytimeout.h"
#include "shoprender.h"
#include "util/json_readers.h"
#include "util/networkmanager.h"
#include "util/programstate.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

namespace {

    constexpr const char *kPoeEditThread = "https://www.pathofexile.com/forum/edit-thread/";

    // POSTs per thread before the submission fails. Each one follows a
    // freshly fetched edit page.
    constexpr int kMaxAttemptsPerThread = 3;

    // The longest wait the forum may ask for that a submission holds out
    // for; a longer one fails the submission and tells the user.
    constexpr int kMaxRateLimitWaitSeconds = 60;

    // A POST sent too soon after its edit page was served comes back with an
    // expired security token.
    constexpr std::chrono::milliseconds kEditToPostDelay{500};

    // An element whose class marks an error, then the first list item after
    // it: the forum's error message. The match can start at an input that
    // carries the class, such as the hash input, so only the list item's
    // text may be used — never the whole match (#315).
    const QRegularExpression kErrorRegex(
        R"regex(
		# Start the match looking for any class attribute that indicates an error
		class="(?:input-error|errors)"

		# Skip over as much as possible while looking for an <li> start tag that
		# should be the start of the error message.
		.*?

		# Match the list item element and capture it's contents, because this is
		# expected to be the error message.
		<li>(.*?)</li>
	)regex",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption
            | QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::ExtendedPatternSyntaxOption);

    const QRegularExpression kRateLimitRegex(R"regex(You must wait (\d+) seconds.)regex",
                                             QRegularExpression::CaseInsensitiveOption);

    // What a forum reply carried, read once.
    struct ForumReply
    {
        int status{0};
        QNetworkReply::NetworkError error{QNetworkReply::NoError};
        QString error_string;
        QByteArray body;
    };

    ForumReply ReadReply(QNetworkReply &reply)
    {
        return {reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                reply.error(),
                reply.errorString(),
                reply.readAll()};
    }

    // The website may redirect an unauthenticated request to an error page
    // whose final response is 200, so the status alone is not enough. The
    // phrases count only as the page's own heading (the layoutBoxTitle
    // <h1>) or the start of its <title>: the edit page carries the user's
    // post and the thread view everyone's, and either may contain them.
    // No logged-out page has been captured; this shape is derived from the
    // headings and titles of the captured pages.
    const QRegularExpression kSessionRejectedHeading(
        R"regex(<h1[^>]*\blayoutBoxTitle\b[^>]*>\s*(?:Login Required|Permission Denied|Content Denied)\s*</h1>)regex"
        R"regex(|<title>\s*(?:Login Required|Permission Denied|Content Denied))regex");

    bool IsPoeSessionRejected(const ForumReply &reply)
    {
        return reply.status == 401 || reply.status == 403
               || kSessionRejectedHeading.match(QString::fromUtf8(reply.body)).hasMatch();
    }

    Shop::Failure Error(const QString &message)
    {
        return {Shop::Failure::Kind::Error, message};
    }

    Shop::Failure SessionRejected(const QString &reason)
    {
        return {Shop::Failure::Kind::SessionRejected, reason};
    }

    // The edit form a POST needs: the title (HTML-escaped, as the page
    // carries it) and the CSRF token.
    struct EditForm
    {
        QString title;
        QString hash;
    };

    std::variant<EditForm, Shop::Failure> ParseEditPage(const ForumReply &reply,
                                                        const QString &thread)
    {
        if (IsPoeSessionRejected(reply)) {
            return SessionRejected("the forum edit page rejected the POESESSID");
        }
        if (reply.error != QNetworkReply::NoError) {
            if (reply.status == 0) {
                // No HTTP response at all: nothing is known about the thread.
                return Error(
                    QString("pathofexile.com did not answer (%1).").arg(reply.error_string));
            }
            if (reply.status >= 500) {
                return Error(QString("pathofexile.com answered with an error (HTTP %1). Try again "
                                     "later.")
                                 .arg(reply.status));
            }
            return Error(QString("The forum would not open thread %1 for editing (HTTP %2). "
                                 "Check the thread number.")
                             .arg(thread)
                             .arg(reply.status));
        }
        EditForm form;
        form.hash = Util::GetCsrfToken(reply.body, "hash");
        form.title = Util::FindTextBetween(QString::fromUtf8(reply.body),
                                           "<input type=\"text\" name=\"title\" id=\"title\" "
                                           "onkeypress=\"return&#x20;event.keyCode&#x21;&#x3D;13\" "
                                           "value=\"",
                                           "\">");
        if (form.hash.isEmpty() || form.title.isEmpty()) {
            return Error(QString("The forum's page for thread %1 had no edit form. Check the "
                                 "thread number, that the thread was started by the account the "
                                 "POESESSID belongs to, and that the POESESSID is still logged in.")
                             .arg(thread));
        }
        return form;
    }

    struct Posted
    {};

    // The forum asked for the POST to be made again: after `seconds`, or,
    // when it is zero, after the shop's retry delay.
    struct RetryAfter
    {
        int seconds{0};
        QString reason;
    };

    std::variant<Posted, RetryAfter, Shop::Failure> ClassifyPostReply(const ForumReply &reply,
                                                                      const QString &thread)
    {
        if (reply.status == 401 || reply.status == 403) {
            return SessionRejected("the forum rejected the POST");
        }
        if (reply.error != QNetworkReply::NoError) {
            if (reply.status == 0) {
                return Error(QString("The forum did not answer (%1).").arg(reply.error_string));
            }
            return Error(QString("The forum refused the post (HTTP %1).").arg(reply.status));
        }
        if (IsPoeSessionRejected(reply)) {
            return SessionRejected("the forum answered the POST with an authentication page");
        }

        // Errors show up as an element whose class is "errors" or
        // "input-error", followed by the message in a list item. One or more
        // may be present; any that is not a request to retry fails the post.
        std::optional<RetryAfter> retry;
        QRegularExpressionMatchIterator matches = kErrorRegex.globalMatch(reply.body);
        while (matches.hasNext()) {
            const QString message = QTextDocumentFragment::fromHtml(matches.next().captured(1))
                                        .toPlainText()
                                        .trimmed();
            if (message.startsWith("Security token has expired.")) {
                if (!retry) {
                    retry = RetryAfter{0, message};
                }
            } else if (message.startsWith("Rate limiting", Qt::CaseInsensitive)) {
                const int seconds = kRateLimitRegex.match(message).captured(1).toInt();
                if (seconds == 0) {
                    return Error("The forum is limiting posts but did not say for how long. Try "
                                 "again in a few minutes.");
                }
                if (!retry || retry->seconds < seconds) {
                    retry = RetryAfter{seconds, message};
                }
            } else if (message.startsWith("Failed to find item.", Qt::CaseInsensitive)) {
                return Error("The forum could not find one of your items; your stash has "
                             "probably changed since the last refresh. Refresh your tabs and "
                             "update the shop again.");
            } else {
                return Error("The forum reported an error: " + message);
            }
        }
        if (retry) {
            return *retry;
        }
        if (reply.body.contains("class=\"errors\"")
            || reply.body.contains("class=\"input-error\"")) {
            return Error("The forum reported an error the shop does not recognise.");
        }
        // No known error is not enough: a saved edit answers with the
        // thread's own page, which links to view-thread/<id> (written
        // "&#x2F;" in the breadcrumb). The edit form coming back, an empty
        // reply, or any other page means the post was not seen to save.
        const QRegularExpression thread_link(
            QString(R"regex(view-thread(?:/|&#x2F;)%1(?![0-9]))regex")
                .arg(QRegularExpression::escape(thread)));
        if (reply.body.contains("name=\"hash\"")
            || !thread_link.match(QString::fromUtf8(reply.body)).hasMatch()) {
            return Error(QString("The forum sent a page Acquisition did not expect, so the post "
                                 "for thread %1 may not have been saved.")
                             .arg(thread));
        }
        return Posted{};
    }

} // namespace

Shop::Shop(QSettings &settings,
           NetworkManager &network_manager,
           PoeApiClient &api,
           DataStore &datastore,
           ItemsManager &items_manager,
           BuyoutManager &buyout_manager)
    : m_settings(settings)
    , m_network_manager(network_manager)
    , m_api(api)
    , m_datastore(datastore)
    , m_items_manager(items_manager)
    , m_buyout_manager(buyout_manager)
{
    spdlog::debug("Shop: initializing");
    // SkipEmptyParts: an empty or missing "shop" key must load as no
    // threads, or the no-threads warning in SubmitShopToForum is
    // unreachable (F45).
    m_threads = m_datastore.Get("shop").split(";", Qt::SkipEmptyParts);
    m_auto_update = m_settings.value("shop_autoupdate").toBool();
    m_shop_template = m_datastore.Get("shop_template");
    if (m_shop_template.isEmpty()) {
        m_shop_template = kShopTemplateItems;
    }
}

void Shop::SetThread(const QStringList &threads)
{
    if (m_active_job) {
        spdlog::warn("Shop: cannot set shop threads: shop is still updating");
        return;
    }
    spdlog::debug("Shop: setting {} thread(s) to {}", threads.size(), threads.join(";"));
    m_threads = threads;
    m_datastore.Set("shop", threads.join(";"));
    ExpireShopData();
    m_datastore.Set("shop_hash", "");
}

void Shop::SetAutoUpdate(bool update)
{
    spdlog::debug("Shop: setting autoupdate to {}", update);
    const bool changed = (update != m_auto_update);
    m_auto_update = update;
    m_settings.setValue("shop_autoupdate", update);
    // Disabling automation drops the waiting automatic capture (M2 D8/R5-6):
    // the active job finishes normally, but no new automatic job may start
    // after the user turned automation off.
    if (!update && m_waiting_capture) {
        spdlog::debug("Shop: dropping the waiting automatic capture — auto-update disabled");
        m_waiting_capture.reset();
    }
    if (changed) {
        emit AutoUpdateChanged(update);
    }
}

void Shop::SetShopTemplate(const QString &shop_template)
{
    spdlog::debug("Shop: setting template to {}", shop_template);
    m_shop_template = shop_template;
    m_datastore.Set("shop_template", shop_template);
    ExpireShopData();
}

void Shop::OnRefreshFinished(const RefreshOutcome &outcome)
{
    spdlog::trace("Shop::OnRefreshFinished() entered");
    if (!m_auto_update) {
        return;
    }
    // The gate (M2 D8/R1-1): automatic submission requires a CLEAN
    // completed refresh. A failed refresh posts nothing; a
    // completed-with-skips refresh posts nothing either — the skipped
    // sources' published items are stale, and auto-posting them would
    // present them to buyers as fresh.
    const auto *completed = std::get_if<CompletedRefresh>(&outcome);
    if (!completed) {
        spdlog::debug("Shop: no automatic submission — the refresh failed");
        return;
    }
    if (!completed->skipped.empty()) {
        // Keep-and-drain (R5-6): the refresh was not clean, so it never
        // becomes eligible — but an already-waiting clean capture is still
        // the newest clean published state and survives untouched.
        spdlog::warn("Shop: no automatic submission — {} sources were skipped this refresh",
                     completed->skipped.size());
        return;
    }
    spdlog::trace("Shop::OnRefreshFinished() submitting shops");
    SubmitAutomaticShop();
}

void Shop::Warn(Notice notice, bool automatic, const QString &message)
{
    const bool first_time = m_warned.insert(notice).second;
    if (!automatic || first_time) {
        emit UserWarning(message);
    }
}

bool Shop::ReadyToPost(bool automatic)
{
    if (!m_buyout_manager.BuyoutsLoaded()) {
        spdlog::error("Shop: not submitting the shop because the buyouts did not load.");
        Warn(Notice::BuyoutsNotLoaded,
             automatic,
             "Acquisition could not load your prices, so your forum shop was not "
             "updated. Your shop threads have been left as they are."
             "\n\n"
             "Check the log file for details.");
        return false;
    }
    if (m_threads.empty()) {
        spdlog::error("Shop: cannot submit shops because shop threads are not set.");
        Warn(Notice::NoThreads,
             automatic,
             "No forum threads have been set."
             "\n\n"
             "Use the Shop --> 'Forum shop thread...' menu item.");
        return false;
    }
    if (m_settings.value("session_id").toString().isEmpty()) {
        spdlog::error("Shop: cannot submit a shop because the POESESSID is not set");
        Warn(Notice::NoSession,
             automatic,
             "Cannot update forum shop threads because POESESSID has not been set."
             "\n\n"
             "Use the Shop --> 'Update Shop POESESSID' menu item."
             "\n\n"
             "This is required even if you have logged in with OAuth, because the "
             "forums do not support OAuth.");
        return false;
    }
    return true;
}

void Shop::SubmitAutomaticShop()
{
    if (!ReadyToPost(true)) {
        return;
    }

    // Automatic admission captures BEFORE applying the busy policy (M2
    // D8/R3-1): while a job is active, the newest clean capture becomes —
    // or replaces — the single waiting automatic capture, so the pipeline
    // converges to the newest clean snapshot instead of posting every
    // clean refresh generation.
    auto job = CaptureJob(false);
    job->automatic = true;
    if (m_active_job) {
        spdlog::debug("Shop: a submission is active; holding the newest clean capture");
        m_waiting_capture = std::move(job);
        return;
    }
    m_active_job = std::move(job);
    UpdateStashIndex();
}

void Shop::SubmitShopToForum(bool force)
{
    spdlog::debug("Shop: submitting shop(s) to forums with force = {}", force);
    if (!ReadyToPost(false)) {
        return;
    }
    if (m_active_job) {
        spdlog::warn("Shop: cannot submit shops because the last update is not finished.");
        Warn(Notice::Busy,
             false,
             "Your forum shop is already being updated. Try again when that update has "
             "finished.");
        return;
    }

    // The complete submission input is captured by value here, at request
    // time (M2 D8/R2-1): an update legally started after this point can
    // stream deltas into the published state, and this job must never read
    // them. Only the legacy stash index is still missing; it is applied to
    // the job when it arrives.
    m_active_job = CaptureJob(force);

    UpdateStashIndex();
}

std::unique_ptr<Shop::ShopJob> Shop::CaptureJob(bool force) const
{
    auto job = std::make_unique<ShopJob>();
    job->input.shop_template = m_shop_template;
    job->input.realm = m_settings.value("realm").toString();
    job->input.league = m_settings.value("league").toString();
    job->threads = m_threads;
    job->force = force;
    job->input_revision = m_input_revision;
    for (const auto &item : m_items_manager.items()) {
        const Buyout bo = m_buyout_manager.Get(*item);
        if (bo.IsPostable()) {
            job->input.items.push_back({item->PrettyName(), item->location(), bo});
        }
    }
    return job;
}

void Shop::UpdateStashIndex()
{
    spdlog::debug("Shop: updating the stash index");

    const QString account = m_settings.value("account").toString();

    // Context-bound (R6-2): the continuation is tied to this Shop, so it
    // never runs against a destroyed one. Shop stays callback-style — no
    // coroutines here (R6-3).
    m_api.getLegacyStashIndex(account, m_active_job->input.realm, m_active_job->input.league)
        .then(this,
              [this](const std::expected<poe::WebStashListWrapper, RateLimit::FetchError> &result) {
                  OnStashIndexReceived(result);
              });
}

void Shop::OnStashIndexReceived(
    const std::expected<poe::WebStashListWrapper, RateLimit::FetchError> &result)
{
    // Every failure the network can produce arrives as one value, already
    // classified — the transport check, the status check, and the parse
    // check this function used to do all live below the facade now.
    if (!result) {
        const auto &error = result.error();
        if (error.kind == RateLimit::FetchError::Kind::Canceled) {
            FailActiveJob({Failure::Kind::Canceled, "the stash index request was canceled"});
        } else if (error.kind == RateLimit::FetchError::Kind::Http
                   && (error.http_status == 401 || error.http_status == 403)) {
            FailActiveJob(SessionRejected("the legacy stash index returned HTTP 401/403"));
        } else {
            FailActiveJob(Error(QString("Acquisition could not read your stash tab list from "
                                        "pathofexile.com (%1: %2).")
                                    .arg(RateLimit::ToString(error.kind), error.message)));
        }
        return;
    }
    const poe::WebStashListWrapper *tabs_wrapper = &result.value();

    if (!tabs_wrapper->tabs) {
        FailActiveJob(Error("The stash tab list from pathofexile.com had no tabs in it."));
        return;
    }

    // The index is job-local transport state: it completes the job's capture
    // and is never shared with a later job.
    const auto &tabs = tabs_wrapper->tabs.value();
    spdlog::debug("Shop: received legacy tabs list, there are {} tabs", tabs.size());
    for (const auto &tab : tabs) {
        m_active_job->input.tab_index[tab.id.first(10)] = tab.i;
    }

    OnStashIndexUpdated();
}

void Shop::ExpireShopData()
{
    // A monotonic revision, not a flag (M2 D8): completing an older job can
    // only mark its own revision rendered, never this newer one.
    spdlog::trace("Shop: expiring shop data");
    ++m_input_revision;
    // An output-affecting local edit drops the waiting automatic capture
    // (R6-4): draining a pre-edit capture after the user's edit would post
    // data older than their intent while telling them the shop updated. The
    // active job's immutable capture is deliberately unaffected.
    if (m_waiting_capture) {
        spdlog::debug("Shop: dropping the waiting automatic capture — the shop input changed");
        m_waiting_capture.reset();
    }
}

void Shop::OnPublishedSnapshot()
{
    // The snapshot boundary dirties the preview/clipboard cache like any
    // other input change, but a waiting CLEAN capture survives it (R5-6):
    // a completed-with-skips or failed refresh publishing its snapshot must
    // not invalidate the newest clean state waiting to drain.
    spdlog::trace("Shop: expiring shop data at the published snapshot");
    ++m_input_revision;
}

void Shop::CompleteActiveJob()
{
    // The success terminal exit (M2 D8): including the unchanged-hash
    // no-post. Drain the newest waiting automatic capture, converging to
    // the newest clean snapshot (R3-1).
    m_active_job.reset();
    if (m_waiting_capture) {
        spdlog::debug("Shop: starting the waiting automatic submission");
        m_active_job = std::move(m_waiting_capture);
        UpdateStashIndex();
    }
}

void Shop::FailActiveJob(const Failure &failure)
{
    // The failure terminal exit (M2 D8/R4-3, no kind discrimination):
    // drain nothing and discard the waiting capture, leaving the input
    // revision dirty — the next clean automatic request or manual request
    // recaptures the latest published state instead of retrying a possibly
    // stale snapshot or hammering an auth/network failure.
    const bool automatic = m_active_job && m_active_job->automatic;
    m_active_job.reset();
    if (m_waiting_capture) {
        spdlog::debug("Shop: discarding the waiting automatic capture after a failed submission");
        m_waiting_capture.reset();
    }

    switch (failure.kind) {
    case Failure::Kind::Canceled:
        spdlog::debug("Shop: the submission was canceled: {}", failure.message);
        emit StatusUpdate(ProgramState::Ready, "Forum shop update canceled");
        break;
    case Failure::Kind::SessionRejected:
        spdlog::warn("Shop: rejecting POESESSID: {}", failure.message);
        SetAutoUpdate(false);
        emit PoeSessionRejected();
        emit StatusUpdate(ProgramState::Ready,
                          "Forum shop not updated: Path of Exile rejected the POESESSID");
        Warn(Notice::SessionRejected,
             false,
             "Path of Exile rejected the saved POESESSID. Acquisition has "
             "cleared it and disabled automatic forum updates as a precaution.\n\n"
             "Normal stash and character updates are unaffected.");
        break;
    case Failure::Kind::Error:
        spdlog::error("Shop: the forum shop was not updated: {}", failure.message);
        emit StatusUpdate(ProgramState::Ready, "Forum shop not updated: " + failure.message);
        Warn(Notice::SubmissionFailed,
             automatic,
             "Your forum shop was not updated.\n\n" + failure.message);
        break;
    }
}

void Shop::OnStashIndexUpdated()
{
    ShopJob &job = *m_active_job;
    spdlog::debug("Shop: updating {} forum shop threads", job.threads.size());
    const QString previous_hash = m_datastore.Get("shop_hash");

    // Every submission renders from its capture (M2 D8), independent of
    // preview-cache freshness — a cached preview may predate streamed
    // deltas that this job's capture already includes, or vice versa.
    job.rendered = RenderShop(job.input);
    for (const auto &item : job.rendered.unlinked) {
        spdlog::error("Shop: cannot determine tab index for {} in {}",
                      item.pretty_name,
                      item.location.GetHeader());
    }
    PublishPreviewCache(job);

    // More pages than threads (#253): the pages that fit are posted and the
    // rest are not. Checked before the unchanged-hash exit, so the user
    // hears about a standing shortfall even when nothing needs posting:
    // every time for a manual request, once per session for automatic ones.
    if (job.threads.size() < job.rendered.pages.size()) {
        const qsizetype missing = job.rendered.pages.size() - job.threads.size();
        spdlog::warn("Shop: need {} more shops defined to fit all your items.", missing);
        Warn(Notice::PageShortfall,
             job.automatic,
             QString("Your forum shop needs %1 more thread(s) to list all of your "
                     "priced items. Only the first %2 page(s) are posted."
                     "\n\n"
                     "Use the Shop --> 'Forum shop thread...' menu item to add "
                     "threads.")
                 .arg(missing)
                 .arg(job.threads.size()));
    }

    // Don't resubmit the shop if the data hasn't changed. The no-post case
    // is a SUCCESS terminal exit (M2 D8): it drains the waiting capture.
    if ((previous_hash == job.rendered.hash) && !job.force) {
        spdlog::trace("Shop: skipping update because the shop hash has not changed");
        CompleteActiveJob();
        return;
    }

    job.requests_completed = 0;
    job.attempts = 0;
    SubmitSingleShop();
}

void Shop::PublishPreviewCache(const ShopJob &job)
{
    // A rendered job publishes the preview cache only if it is not older
    // than the cache already here (M2 D8), and it can mark only its own
    // captured revision rendered — an older job finishing late can neither
    // clobber a newer preview nor mark newer input clean.
    if (job.input_revision < m_cache_revision) {
        return;
    }
    m_shop_data = job.rendered.pages;
    m_cache_revision = job.input_revision;
}

void Shop::SubmitSingleShop()
{
    ShopJob &job = *m_active_job;

    // Finish when all threads have been updated. The persisted hash is the
    // job's own rendered hash — the successful-completion terminal exit.
    if (job.requests_completed >= job.threads.size()) {
        spdlog::debug("Shop: updated {} threads", job.threads.size());
        emit StatusUpdate(ProgramState::Ready, "Shop threads updated");
        m_datastore.Set("shop_hash", job.rendered.hash);
        CompleteActiveJob();
        return;
    }

    const QString &thread = job.threads[job.requests_completed];
    spdlog::debug("Shop: updating shop thread # {}: {}", job.requests_completed + 1, thread);
    emit StatusUpdate(ProgramState::Ready,
                      QString("Sending your shops to the forum, %1/%2")
                          .arg(job.requests_completed)
                          .arg(job.threads.size()));

    // First get the edit-thread page, for its CSRF token and title.
    QNetworkRequest request(QUrl(kPoeEditThread + thread));
    request.setRawHeader("Cache-Control", "max-age=0");
    request.setTransferTimeout(kEditThreadTimeout);
    QNetworkReply *reply = m_network_manager.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { OnEditPageFinished(reply); });
}

void Shop::OnEditPageFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (!m_active_job) {
        spdlog::warn("Shop: ignoring an edit page reply because no submission job is active");
        return;
    }
    const ShopJob &job = *m_active_job;
    auto page = ParseEditPage(ReadReply(*reply), job.threads[job.requests_completed]);
    if (const auto *failure = std::get_if<Failure>(&page)) {
        FailActiveJob(*failure);
        return;
    }
    const EditForm form = std::get<EditForm>(std::move(page));
    const ShopJob *posting = m_active_job.get();
    QTimer::singleShot(kEditToPostDelay, this, [this, posting, form]() {
        if (m_active_job.get() == posting) {
            SubmitNextShop(form.title, form.hash);
        }
    });
}

void Shop::SubmitNextShop(const QString &title, const QString &hash)
{
    ShopJob &job = *m_active_job;
    ++job.attempts;
    spdlog::debug("Shop: posting thread # {}, attempt {}", job.requests_completed + 1, job.attempts);

    const QString content = job.requests_completed < job.rendered.pages.size()
                                ? job.rendered.pages[job.requests_completed]
                                : "Empty";
    const QByteArray data = Util::EncodeForm({{"title", Util::Decode(title)},
                                              {"content", content},
                                              {"notify_owner", "0"},
                                              {"hash", hash},
                                              {"submit", "Submit"}});
    QNetworkRequest request(QUrl(kPoeEditThread + job.threads[job.requests_completed]));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Cache-Control", "max-age=0");
    request.setTransferTimeout(kEditThreadTimeout);

    QNetworkReply *reply = m_network_manager.post(request, data);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { OnShopSubmitted(reply); });
}

void Shop::OnShopSubmitted(QNetworkReply *reply)
{
    reply->deleteLater();
    if (!m_active_job) {
        spdlog::warn("Shop: ignoring a submission reply because no job is active");
        return;
    }
    ShopJob &job = *m_active_job;
    const auto outcome = ClassifyPostReply(ReadReply(*reply), job.threads[job.requests_completed]);

    if (const auto *failure = std::get_if<Failure>(&outcome)) {
        FailActiveJob(*failure);
        return;
    }

    if (const auto *retry = std::get_if<RetryAfter>(&outcome)) {
        spdlog::warn("Shop: the forum asked for a retry: {}", retry->reason);
        if (retry->seconds > kMaxRateLimitWaitSeconds) {
            FailActiveJob(Error(QString("The forum asks you to wait %1 seconds before posting "
                                        "again. Update the shop after that.")
                                    .arg(retry->seconds)));
            return;
        }
        if (job.attempts >= kMaxAttemptsPerThread) {
            FailActiveJob(Error(QString("The forum did not accept the post for thread %1 after %2 "
                                        "attempts. Its last answer was: %3")
                                    .arg(job.threads[job.requests_completed])
                                    .arg(job.attempts)
                                    .arg(retry->reason)));
            return;
        }
        // The rate-limit wait is the forum's own, plus a second of margin.
        const std::chrono::milliseconds delay = retry->seconds > 0
                                                    ? std::chrono::seconds(retry->seconds + 1)
                                                    : m_retry_delay;
        const ShopJob *retrying = &job;
        QTimer::singleShot(delay, this, [this, retrying]() {
            if (m_active_job.get() == retrying) {
                SubmitSingleShop();
            }
        });
        return;
    }

    ++job.requests_completed;
    job.attempts = 0;
    SubmitSingleShop();
}

void Shop::CopyToClipboard()
{
    if (m_shop_data.empty()) {
        spdlog::warn("Shop: there is nothing to copy to the clipboard");
        return;
    }
    if (shop_data_outdated()) {
        spdlog::warn("Shop: copying outdated shop data!");
    }
    if (m_shop_data.size() > 1) {
        spdlog::warn(
            "Shop: you have multiple shops; only the first will be copied to the clipboard");
    }
    spdlog::trace("Shop: copying {} characters of shop data to the clipboard",
                  m_shop_data[0].size());
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(m_shop_data[0]);
}
