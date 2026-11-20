#pragma once

#include <QNetworkAccessManager>
#include <QQmlNetworkAccessManagerFactory>

// The network access manager QML gets, which reaches nothing beyond the app
// itself: qrc: and file: load, and every other request (http, https, ftp,
// data) fails at once without being sent. Feeds are fetched by Fetcher, so
// the window never needs the network, and nothing a feed manages to slip
// into a rich text view or an image source can phone home.
class LocalNetworkAccessManager : public QNetworkAccessManager {
public:
    using QNetworkAccessManager::QNetworkAccessManager;

    static bool allowed(const QUrl &url);

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoingData = nullptr) override;
};

class LocalNetworkFactory : public QQmlNetworkAccessManagerFactory {
public:
    QNetworkAccessManager *create(QObject *parent) override;
};
