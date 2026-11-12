// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev
// Ref: http://qt-project.org/forums/viewthread/13728

#pragma once

#include <QEvent>
#include <QObject>
#include <QScrollArea>
#include <QWidget>

class VerticalScrollArea : public QScrollArea
{
    Q_OBJECT
public:
    explicit VerticalScrollArea(QWidget *parent = 0);
    virtual bool eventFilter(QObject *o, QEvent *e);

private:
    // Guards against eventFilter() re-entering itself through the relayout its
    // own setMinimumWidth() triggers. See the comment in eventFilter().
    bool m_adjusting_width = false;
};
