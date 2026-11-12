// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#pragma once

#include <QDialog>
#include <QHash>
#include <QTimer>

class QLabel;
class QTreeWidget;
class QVBoxLayout;

class QString;

class RateLimiter;
class RateLimitPolicy;
// The generated moc file registers signals/slots that take a RateLimitPolicy,
// so it needs the full type, not the forward declaration (GCC 16
// -Wsfinae-incomplete, issue #217).
Q_MOC_INCLUDE("ratelimit/ratelimitpolicy.h")

class RateLimitDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RateLimitDialog(QWidget *parent, RateLimiter *limiter);
public slots:
    void OnPause(int pause, const QString &policy_name);
    void OnPolicyUpdate(const RateLimitPolicy &policy);

    // Coalesced (M1-M2/D10): the batch submit emits QueueUpdate once per
    // queued request in a single loop turn — thousands at the 2,000-tab
    // scale, and the handler runs whether or not the dialog is visible.
    // Applying each one synchronously blocked the UI thread past a frame
    // (measured ~11.5 us per emission, ~23 ms per 2,000-entry burst;
    // docs/design/m1-m2-result.md), so updates park in a latest-value map
    // and land on a short non-resetting single-shot timer: the whole burst
    // becomes one apply, and the widget is never more than the flush
    // interval behind the queue.
    void OnQueueUpdate(const QString &policy_name, int queue_size);

private:
    void FlushQueueUpdates();

    QVBoxLayout *m_layout;
    QTreeWidget *m_treeWidget;
    QLabel *m_statusLabel;

    QHash<QString, int> m_pending_queue_updates;
    QTimer m_queue_flush_timer;
};
