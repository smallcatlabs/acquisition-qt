// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Ilya Zhuravlev

#include "ui/updateprompt.h"

#include <QMessageBox>
#include <QPushButton>

#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/updatechecker.h"

void AskUserToUpdate(UpdateChecker &checker)
{
    if (!checker.UpdateOffered()) {
        // Nothing to offer. This is reached when the "no longer available"
        // signal is connected here (the login window's connection), and
        // must not open a prompt.
        spdlog::debug("UpdateChecker: no newer versions available");
        return;
    }

    // Create the dialog box.
    QMessageBox msgbox(nullptr);
    msgbox.setWindowTitle("Acquisition Update Checker");
    msgbox.setText(checker.UpdateMessage());
    auto accept_button = msgbox.addButton("  Go to Github  ", QMessageBox::AcceptRole);
    auto remind_button = msgbox.addButton("  Ignore  ", QMessageBox::RejectRole);
    auto ignore_button = msgbox.addButton("  Ignore, and don't ask again  ",
                                          QMessageBox::RejectRole);
    msgbox.setDefaultButton(ignore_button);

    // Resize the buttons so the text fits.
    accept_button->setMinimumWidth(accept_button->sizeHint().width());
    ignore_button->setMinimumWidth(ignore_button->sizeHint().width());
    remind_button->setMinimumWidth(remind_button->sizeHint().width());

    // Get the user's choice.
    msgbox.exec();
    const auto clicked = msgbox.clickedButton();
    if (clicked == ignore_button) {
        checker.ApplyUpdateChoice(UpdateChecker::UpdateChoice::DontAskAgain);
    } else if (clicked == accept_button) {
        checker.ApplyUpdateChoice(UpdateChecker::UpdateChoice::Download);
    } else {
        checker.ApplyUpdateChoice(UpdateChecker::UpdateChoice::AskAgainLater);
    }
}
