// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Ilya Zhuravlev

#pragma once

class UpdateChecker;

// Asks the user what to do about the update `checker` has found, and hands
// the answer to it. Does nothing when nothing is left to offer.
void AskUserToUpdate(UpdateChecker &checker);
