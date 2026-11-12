// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev
// Ref: http://qt-project.org/forums/viewthread/13728

#include "ui/verticalscrollarea.h"

#include <QEvent>
#include <QScopedValueRollback>
#include <QScrollBar>

VerticalScrollArea::VerticalScrollArea(QWidget *parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

bool VerticalScrollArea::eventFilter(QObject *o, QEvent *e)
{
    // This works because QScrollArea::setWidget installs an eventFilter on the widget
    if (!m_adjusting_width && o && o == widget() && e->type() == QEvent::Resize) {
        // Setting a minimum width here relays the scroll area out, which
        // resizes the child widget, which delivers another Resize event back to
        // this filter. In 0.18.3-beta.1 that fed back on itself ~429 times and
        // exhausted the stack: the crash dump shows this filter, QLayout::activate
        // and QCoreApplicationPrivate::sendThroughObjectEventFilters repeating.
        //
        // The loop only terminates on its own if the computed width settles, and
        // it need not: the search form's content uses heightForWidth (FlowLayout),
        // so height depends on width, the vertical scroll bar's visibility depends
        // on height under ScrollBarAsNeeded, and the width computed below depends
        // on the scroll bar again. Guard the re-entry rather than assume the
        // arithmetic converges.
        QScopedValueRollback<bool> guard(m_adjusting_width, true);

        // sizeHint() rather than width(): a hidden scroll bar keeps whatever
        // width it was last laid out to, which makes this term depend on
        // visibility for no useful reason.
        const int width = widget()->minimumSizeHint().width()
                          + verticalScrollBar()->sizeHint().width() + 6;
        if (width != minimumWidth()) {
            setMinimumWidth(width);
        }
    }
    return QScrollArea::eventFilter(o, e);
}
