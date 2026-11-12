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

    bool IsPoeSessionRejected(const QNetworkReply *reply)
    {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        return status == 401 || status == 403;
    }

    bool IsPoeSessionRejected(const QByteArray &body)
    {
        return body.contains("Login Required") || body.contains("Permission Denied")
               || body.contains("Content Denied");
    }

} // namespace

// Use a regular expression to look for html errors.
const QRegularExpression Shop::error_regex(
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

const QRegularExpression Shop::ratelimit_regex(R"regex(You must wait (\d+) seconds.)regex",
                                               QRegularExpression::CaseInsensitiveOption);

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
            spdlog::debug("Shop: the stash index request was canceled");
        } else if (error.kind == RateLimit::FetchError::Kind::Http
                   && (error.http_status == 401 || error.http_status == 403)) {
            RejectPoeSession("legacy stash index returned HTTP 401/403");
        } else {
            spdlog::error("Shop: could not index stashes ({}): {}",
                          RateLimit::ToString(error.kind),
                          error.message);
        }
        FailActiveJob();
        return;
    }
    const poe::WebStashListWrapper *tabs_wrapper = &result.value();

    if (!tabs_wrapper->tabs) {
        spdlog::error("Shop: stash list result does not contains tabs list.");
        FailActiveJob();
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

void Shop::RejectPoeSession(const QString &reason)
{
    spdlog::warn("Shop: rejecting POESESSID: {}", reason);

    m_waiting_capture.reset();
    SetAutoUpdate(false);

    emit PoeSessionRejected();
    emit UserWarning("Path of Exile rejected the saved POESESSID. Acquisition has "
                     "cleared it and disabled automatic forum updates as a precaution.\n\n"
                     "Normal stash and character updates are unaffected.");
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

void Shop::FailActiveJob()
{
    // The failure terminal exit (M2 D8/R4-3, no kind discrimination):
    // drain nothing and discard the waiting capture, leaving the input
    // revision dirty — the next clean automatic request or manual request
    // recaptures the latest published state instead of retrying a possibly
    // stale snapshot or hammering an auth/network failure.
    m_active_job.reset();
    if (m_waiting_capture) {
        spdlog::debug("Shop: discarding the waiting automatic capture after a failed submission");
        m_waiting_capture.reset();
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

QString Shop::ShopEditUrl(qsizetype idx)
{
    if (idx >= m_active_job->threads.size()) {
        spdlog::error("Shop: cannot create edit url for thread # {}", idx);
        return "";
    }
    const QString url = kPoeEditThread + m_active_job->threads[idx];
    spdlog::debug("Shop: shop edit url # {} is {}", idx, url);
    return url;
}

void Shop::SubmitSingleShop()
{
    spdlog::debug("Shop: submitting a single shop.");
    ShopJob &job = *m_active_job;

    // Submet the next thread.
    if (job.requests_completed < job.threads.size()) {
        spdlog::debug("Shop: updating shop thread # {}: {}",
                      (job.requests_completed + 1),
                      job.threads[job.requests_completed]);
        emit StatusUpdate(ProgramState::Ready,
                          QString("Sending your shops to the forum, %1/%2")
                              .arg(job.requests_completed)
                              .arg(job.threads.size()));

        // first, get to the edit-thread page to grab CSRF token
        QNetworkRequest request = QNetworkRequest(QUrl(ShopEditUrl(job.requests_completed)));
        request.setRawHeader("Cache-Control", "max-age=0");
        request.setTransferTimeout(kEditThreadTimeout);
        QNetworkReply *fetched = m_network_manager.get(request);
        connect(fetched, &QNetworkReply::finished, this, &Shop::OnEditPageFinished);
        return;
    }

    // Finish when all threads have been updated. The persisted hash is the
    // job's own rendered hash — the successful-completion terminal exit.
    if (job.requests_completed == job.threads.size()) {
        spdlog::debug("Shop: updated {} threads", job.threads.size());
        emit StatusUpdate(ProgramState::Ready, "Shop threads updated");
        m_datastore.Set("shop_hash", job.rendered.hash);
        CompleteActiveJob();
        return;
    }

    // This error should never happen.
    spdlog::error("Shop: shop thread # {} does not exist", job.requests_completed);
    emit StatusUpdate(ProgramState::Ready, "Shop threads not updated due to an error.");
    FailActiveJob();
}

void Shop::OnEditPageFinished()
{
    spdlog::trace("Shop: edit page finished");
    if (!m_active_job) {
        spdlog::warn("Shop: ignoring an edit page reply because no submission job is active");
        return;
    }
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());

    if (IsPoeSessionRejected(reply)) {
        RejectPoeSession("forum edit page returned HTTP 401/403");
        reply->deleteLater();
        FailActiveJob();
        return;
    }

    const QByteArray bytes = reply->readAll();
    if (IsPoeSessionRejected(bytes)) {
        // The website may redirect an unauthenticated request to an error
        // page whose final response is 200, so status alone is insufficient.
        RejectPoeSession("forum edit page returned an authentication error page");
        reply->deleteLater();
        FailActiveJob();
        return;
    }

    const QString hash = Util::GetCsrfToken(bytes, "hash");
    if (hash.isEmpty()) {
        spdlog::error("Cannot update shop: unable to extract CSRF token from the page. The "
                      "thread ID may be invalid.");
        emit StatusUpdate(ProgramState::Ready, "Shop threads not updated due to an error.");
        FailActiveJob();
        reply->deleteLater();
        return;
    }
    spdlog::trace("CSRF token found.");

    // now submit our edit
    // holy shit give me some html parser library please
    const QString page(bytes);
    QString title
        = Util::FindTextBetween(page,
                                "<input type=\"text\" name=\"title\" id=\"title\" "
                                "onkeypress=\"return&#x20;event.keyCode&#x21;&#x3D;13\" value=\"",
                                "\">");
    if (title.isEmpty()) {
        spdlog::error("Cannot update shop: title is empty. Check if thread ID is valid.");
        FailActiveJob();
        reply->deleteLater();
        return;
    }

    QTimer::singleShot(500, this, [=, this]() { SubmitNextShop(title, hash); });
    reply->deleteLater();
}

void Shop::SubmitNextShop(const QString &title, const QString &hash)
{
    spdlog::debug("Shop: submitting the next shop.");
    if (!m_active_job) {
        spdlog::warn("Shop: ignoring a deferred submission because no job is active");
        return;
    }
    const ShopJob &job = *m_active_job;

    const QString content = job.requests_completed < job.rendered.pages.size()
                                ? job.rendered.pages[job.requests_completed]
                                : "Empty";

    const QByteArray data = Util::EncodeForm({{"title", Util::Decode(title)},
                                              {"content", content},
                                              {"notify_owner", "0"},
                                              {"hash", hash},
                                              {"submit", "Submit"}});
    QNetworkRequest request((QUrl(ShopEditUrl(job.requests_completed))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Cache-Control", "max-age=0");
    request.setTransferTimeout(kEditThreadTimeout);

    QNetworkReply *reply = m_network_manager.post(request, data);
    connect(reply, &QNetworkReply::finished, this, [=, this]() { OnShopSubmitted(title, reply); });
}

void Shop::OnShopSubmitted(const QString &title, QNetworkReply *reply)
{
    spdlog::debug("Shop: shop submission reply received.");

    // Make sure the reply is deleted.
    reply->deleteLater();

    if (!m_active_job) {
        spdlog::warn("Shop: ignoring a submission reply because no job is active");
        return;
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (IsPoeSessionRejected(reply)) {
        RejectPoeSession("forum submission returned HTTP 401/403");
        FailActiveJob();
        return;
    }

    // Check for transport errors. reply->error() is a Qt NetworkError enum,
    // not an HTTP status; the response status lives in the request attribute.
    if (reply->error() != QNetworkReply::NoError) {
        spdlog::error("Shop: network error submitting shop: HTTP {}, Qt error {}: {}",
                      status,
                      reply->error(),
                      reply->errorString());
        FailActiveJob();
        return;
    }

    const QByteArray bytes = reply->readAll();
    if (IsPoeSessionRejected(bytes)) {
        RejectPoeSession("forum submission returned an authentication error page");
        FailActiveJob();
        return;
    }

    // Errors can show up in a couple different places. So far, the easiest way to identify
    // them seems to be to look for an html tag with the "class" attribute set to
    // "input-error" or "errors".
    //
    // After this class attribute, there seems to always be an error message enclosed in
    // an list item tag, with varying differents between the class attribute and that tag.
    QRegularExpressionMatchIterator i = error_regex.globalMatch(bytes);
    if (i.hasNext()) {
        // Process one or more errors (I've never seen more than one in practice).
        int seconds = 0;
        while (i.hasNext()) {
            // We only know the error message if the list item element was found.
            const QRegularExpressionMatch error_match = i.next();
            const QString error_message
                = (error_match.lastCapturedIndex() > 0)
                      ? QTextDocumentFragment::fromHtml(error_match.captured(1)).toPlainText()
                      : "(Failed to parse the error message)";
            spdlog::error("Shop: error submitting shop thread: {}", error_message);
            if (error_message.startsWith("Failed to find item.", Qt::CaseInsensitive)) {
                spdlog::error("Shop: the stash index may be out of date. Try submitting again.");
                FailActiveJob();
                return;
            } else if (error_message.startsWith("Security token has expired.")) {
                // This error would occur somewhat randomly before a delay was added in OnEditPageFinished.
                // With that delay, this error doesn't seem to happen any more, but we should probably
                // keep checking for it.
                if (seconds < 5) {
                    seconds = 5;
                    spdlog::trace("Shop: setting {} second delay.", seconds);
                }
            } else if (error_message.startsWith("Rate limiting", Qt::CaseInsensitive)) {
                // Look for a rate limiting error here, because there are no headers to check for
                // like the other API calls.
                const QRegularExpressionMatch ratelimit_match = ratelimit_regex.match(error_message);
                int ratelimit_delay = ratelimit_match.captured(1).toInt();
                if (ratelimit_delay == 0) {
                    spdlog::error("Shop: error parsing wait time from error message.");
                    FailActiveJob();
                    return;
                }
                if (seconds < ratelimit_delay) {
                    seconds = ratelimit_delay + 1;
                    spdlog::trace("Shop: setting {} second delay.", seconds);
                }
            } else {
                // Only the list item's text, logged above: the whole match
                // can start at the hash input and span the CSRF token (#315).
                spdlog::error("Shop: the forum reported an error the shop does not handle");
                spdlog::debug("Shop: the query was: {}", reply->request().url().toDisplayString());
                FailActiveJob();
                return;
            }
        }
        if (seconds > 0) {
            // Resubmit if the errors indicate we only have to try again later.
            const int ms = seconds * 1000;
            const QString hash = Util::GetCsrfToken(bytes, "hash");
            spdlog::warn("Shop: resubmitting shop after {} seconds.", seconds);
            QTimer::singleShot(ms, this, [=, this]() { SubmitNextShop(title, hash); });
            return;
        } else {
            // Quit the update for any other error.
            FailActiveJob();
            return;
        }
    }

    // Keep legacy error-checking in place for now.
    QString page(bytes);
    QString error = Util::FindTextBetween(page, "<ul class=\"errors\"><li>", "</li></ul>");
    if (!error.isEmpty()) {
        spdlog::error("Shop: (DEPRECATED) Error while submitting shop to forums: {}", error);
        FailActiveJob();
        return;
    }

    // This slightly different error was encountered while debugging an issue with v0.9.9-beta.1.
    // It's possible GGG has updated the forums so the previous error checking is no longer
    // relavent, but that's not certain or documented anywhere, so let's do both.
    QString input_error = Util::FindTextBetween(page, "class=\"input-error\">", "</div>");
    if (!input_error.isEmpty()) {
        spdlog::error("Shop: (DEPRECATED) Input error while submitting shop to forums ({} "
                      "characters of markup not logged)",
                      input_error.size());
        FailActiveJob();
        return;
    }

    // Let's err on the side of being cautious and look for an error the above code might
    // have missed. Otherwise errors might just silently fall through the cracks.
    for (auto &substr : {"class=\"errors\"", "class=\"input-error\""}) {
        if (page.contains(substr)) {
            spdlog::error("Shop: (DEPRECATED) An error was detected but not handled while "
                          "submitting shop to forums: {} (the {}-byte reply is not logged)",
                          substr,
                          bytes.size());
            FailActiveJob();
            return;
        }
    }

    ++m_active_job->requests_completed;
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
