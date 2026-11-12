// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#include "util/checkmsvc.h"

#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVersionNumber>
#include <QtGlobal>

#include "util/spdlog_qt.h" // IWYU pragma: keep

#ifdef Q_OS_WINDOWS
static QString DLL(const QString &name)
{
#ifdef QT_DEBUG
    return name + "d.dll";
#else
    return name + ".dll";
#endif
}
#endif

bool checkMicrosoftRuntime()
{
#ifndef Q_OS_WINDOWS
    // Do nothing on Linux or macOS, but we still want this file to be included
    // in the build we can check for things like linting errors when developing
    // on other platforms.
    return true;
#else
    spdlog::info("Checking Microsoft Visual C++ Runtime...");
    const QStringList libraries = {DLL("msvcp140"), DLL("vcruntime140"), DLL("vcruntime140_1")};
    spdlog::debug("Checking MSVC runtime libraries: {}", libraries.join(", "));
    return checkApplicationDirectory(QDir(QGuiApplication::applicationDirPath()), libraries);
#endif
}

bool checkApplicationDirectory(const QDir &dir, const QStringList &libraries)
{
    const QString path = dir.absolutePath();

    spdlog::debug("Checking application directory for unexpected MSVC libraries.");
    spdlog::debug("Application directory: {}", path);

    QStringList found;
    for (const auto &dll : libraries) {
        if (dir.exists(dll)) {
            found.append(dll);
        }
    }

    if (!found.isEmpty()) {
        spdlog::debug("Found {} unexpected MSVC libraries: {}", found.size(), found.join(", "));

        QStringList msg;
        msg.append("The application directory contains one or more MSVC runtime dlls:");
        msg.append("");
        for (const auto &filename : found) {
            msg.append("\t" + filename);
        }
        msg.append("");
        msg.append("Please delete these files and restart acquisition; they may cause unexpected "
                   "crashes.");

        // Construct a warning dialog box.
        QMessageBox msgbox;
        msgbox.setWindowTitle("Acquisition");
        msgbox.setText(msg.join("\n"));
        msgbox.setIcon(QMessageBox::Warning);
        const auto *open = msgbox.addButton("Open folder and quit", QMessageBox::NoRole);
        const auto *quit = msgbox.addButton("Quit", QMessageBox::NoRole);
        const auto *ignore = msgbox.addButton("Ignore and continue", QMessageBox::NoRole);
        Q_UNUSED(quit);

        // Get and react to the user input.
        msgbox.exec();
        const auto &clicked = msgbox.clickedButton();
        if (clicked == open) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
        if (clicked != ignore) {
            spdlog::info("Quitting at the user's request.");
            return false;
        }
    }
    return true;
}
