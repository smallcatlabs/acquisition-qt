// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include "application.h"
#include "repoe/repoe.h"

// The one test-only seam into Application (#378), on the pattern of
// WorkerTestAccess: Application only forward-declares this as a friend and it
// is DEFINED here, in a test-only header, so a test or a hand-run harness can
// reach the objects Application assembles without Application gaining any
// public accessor.
class ApplicationTestAccess
{
public:
    explicit ApplicationTestAccess(Application &app)
        : m_app(app)
    {}

    LoginDialog &login() const { return m_app.login(); }
    OAuthManager &oauth() const { return m_app.oauth_manager(); }

    // True once the game data (RePoE) has been loaded, or given up on.
    bool gameDataLoaded() const { return m_app.repoe().IsInitialized(); }

    // False until LoginDialog::LoginComplete has built the user session.
    bool hasSession() const { return m_app.m_session != nullptr; }

    // Everything below needs a session.
    const SessionIdentity &identity() const { return m_app.session().identity; }
    ItemsManager &itemsManager() const { return m_app.items_manager(); }
    Shop &shop() const { return m_app.shop(); }
    MainWindow &mainWindow() const { return m_app.main_window(); }

private:
    Application &m_app;
};
