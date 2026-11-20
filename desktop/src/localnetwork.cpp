#include "localnetwork.h"

#include <QNetworkRequest>

bool LocalNetworkAccessManager::allowed(const QUrl &url) {
    const QString scheme = url.scheme().toLower();
    return scheme == QLatin1String("qrc") || (scheme == QLatin1String("file") && url.host().isEmpty());
}

QNetworkReply *LocalNetworkAccessManager::createRequest(Operation operation, const QNetworkRequest &request,
                                                        QIODevice *outgoingData) {
    if (allowed(request.url()))
        return QNetworkAccessManager::createRequest(operation, request, outgoingData);
    // A scheme Qt has no handler for: the reply fails with
    // ProtocolUnknownError and nothing goes out.
    QNetworkRequest refused(request);
    refused.setUrl(QUrl(QStringLiteral("refused:")));
    return QNetworkAccessManager::createRequest(operation, refused, outgoingData);
}

QNetworkAccessManager *LocalNetworkFactory::create(QObject *parent) {
    auto *manager = new LocalNetworkAccessManager(parent);
    manager->setCookieJar(nullptr);
    return manager;
}
