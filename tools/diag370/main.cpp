// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic for issue #370: which TLS backends does a deployed Qt find?
#include <QCoreApplication>
#include <QSslSocket>

#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    std::printf("qVersion: %s\n", qVersion());
    std::printf("libraryPaths: %s\n",
                QCoreApplication::libraryPaths().join(';').toUtf8().constData());
    std::printf("availableBackends: %s\n",
                QSslSocket::availableBackends().join(',').toUtf8().constData());
    std::printf("activeBackend: %s\n", QSslSocket::activeBackend().toUtf8().constData());
    const bool ok = QSslSocket::supportsSsl();
    std::printf("supportsSsl: %s\n", ok ? "true" : "false");
    std::printf("sslLibraryVersion: %s\n",
                QSslSocket::sslLibraryVersionString().toUtf8().constData());
    std::fflush(stdout);
    return ok ? 0 : 1;
}
