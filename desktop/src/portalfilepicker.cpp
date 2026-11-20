// Adapted from Omacut's src/portalfilepicker.cpp
// (https://github.com/omacom-io/omacut, MIT, David Heinemeier Hansson): the
// same portal request handling, with OPML filters instead of video ones and
// without the export quality choice.
#include "portalfilepicker.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QRandomGenerator>

namespace {
struct PortalFilterRule {
    uint type;
    QString pattern;
};

using PortalFilterRules = QList<PortalFilterRule>;

struct PortalFileFilter {
    QString name;
    PortalFilterRules rules;
};

using PortalFileFilters = QList<PortalFileFilter>;

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFilterRule &rule) {
    argument.beginStructure();
    argument << rule.type << rule.pattern;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFilterRule &rule) {
    argument.beginStructure();
    argument >> rule.type >> rule.pattern;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFileFilter &filter) {
    argument.beginStructure();
    argument << filter.name << filter.rules;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFileFilter &filter) {
    argument.beginStructure();
    argument >> filter.name >> filter.rules;
    argument.endStructure();
    return argument;
}

void registerPortalFilterTypes() {
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalFilterRule>();
        qDBusRegisterMetaType<PortalFilterRules>();
        qDBusRegisterMetaType<PortalFileFilter>();
        qDBusRegisterMetaType<PortalFileFilters>();
        return true;
    }();
    Q_UNUSED(registered);
}

PortalFileFilter opmlFilter() {
    return {QCoreApplication::translate("PortalFilePicker", "OPML files"),
            {{1, QStringLiteral("text/x-opml")},
             {0, QStringLiteral("*.opml")},
             {0, QStringLiteral("*.OPML")},
             {0, QStringLiteral("*.xml")}}};
}

PortalFileFilters opmlFilters() {
    return {opmlFilter(), {QCoreApplication::translate("PortalFilePicker", "All files"), {{0, QStringLiteral("*")}}}};
}

QString portalToken() { return QStringLiteral("readstr_%1").arg(QRandomGenerator::global()->generate()); }

QByteArray portalPathBytes(const QString &path) {
    QByteArray bytes = path.toUtf8();
    bytes.append('\0');
    return bytes;
}
}

PortalFilePicker::PortalFilePicker(QObject *parent) : FilePicker(parent) { registerPortalFilterTypes(); }

void PortalFilePicker::openOpml() {
    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), tr("Import"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("multiple"), false);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(QDir::homePath()));
    options.insert(QStringLiteral("filters"), QVariant::fromValue(opmlFilters()));
    options.insert(QStringLiteral("current_filter"), QVariant::fromValue(opmlFilter()));
    requestFile(QStringLiteral("OpenFile"), tr("Import OPML"), options, Action::Open);
}

void PortalFilePicker::saveOpml(const QString &suggestedName) {
    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), tr("Export"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(QDir::homePath()));
    options.insert(QStringLiteral("current_name"), suggestedName);
    options.insert(QStringLiteral("filters"), QVariant::fromValue(PortalFileFilters{opmlFilter()}));
    options.insert(QStringLiteral("current_filter"), QVariant::fromValue(opmlFilter()));
    requestFile(QStringLiteral("SaveFile"), tr("Export OPML"), options, Action::Save);
}

namespace {
const QString portalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString requestInterface = QStringLiteral("org.freedesktop.portal.Request");
}

QString PortalFilePicker::senderName() const {
    const QDBusConnection bus = QDBusConnection::sessionBus();
    return bus.isConnected() ? bus.baseService() : QString();
}

bool PortalFilePicker::listen(const QString &requestPath) {
    return QDBusConnection::sessionBus().connect(portalService, requestPath, requestInterface,
                                                 QStringLiteral("Response"), this,
                                                 SLOT(handleResponse(uint, QVariantMap, QDBusMessage)));
}

void PortalFilePicker::stopListening(const QString &requestPath) {
    QDBusConnection::sessionBus().disconnect(portalService, requestPath, requestInterface, QStringLiteral("Response"),
                                             this, SLOT(handleResponse(uint, QVariantMap, QDBusMessage)));
}

void PortalFilePicker::send(const QDBusMessage &call, int generation) {
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    m_pendingWatcher = watcher;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, generation]() {
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_pendingWatcher == watcher)
            m_pendingWatcher = nullptr;
        if (!reply.isError()) {
            replied(generation, reply.value().path(), {});
            return;
        }
        const QDBusError error = reply.error();
        replied(generation, {}, error.message().isEmpty() ? error.name() : error.message());
    });
}

void PortalFilePicker::closeRequest(const QString &requestPath) {
    QDBusConnection::sessionBus().send(
        QDBusMessage::createMethodCall(portalService, requestPath, requestInterface, QStringLiteral("Close")));
}

void PortalFilePicker::requestFile(const QString &method, const QString &title, QVariantMap options, Action action) {
    // A request whose Response never arrives (the portal crashed or was
    // killed with the dialog up) must not wedge every later dialog behind
    // it: close the stuck one best-effort and replace it.
    if (m_pendingAction != Action::None) {
        closeRequest(m_pendingPath);
        clearPending();
    }

    const QString name = senderName();
    if (name.isEmpty()) {
        emit failed(tr("The desktop portal's file chooser is not available (is xdg-desktop-portal running?)."));
        return;
    }

    // Subscribe to the Response signal at the request path the portal will
    // derive from our handle_token *before* making the call, so a response
    // can't slip past while our match rule is still being installed.
    const QString token = portalToken();
    options.insert(QStringLiteral("handle_token"), token);
    QString sender = name.mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString predictedPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    m_pendingAction = action;
    m_pendingPath = predictedPath;
    if (!listen(predictedPath)) {
        clearPending();
        emit failed(tr("Could not listen for the file chooser's answer."));
        return;
    }

    const int generation = ++m_pendingGeneration;
    QDBusMessage call = QDBusMessage::createMethodCall(portalService, QStringLiteral("/org/freedesktop/portal/desktop"),
                                                       QStringLiteral("org.freedesktop.portal.FileChooser"), method);
    call << QString() << title << options;
    send(call, generation);
}

void PortalFilePicker::replied(int generation, const QString &requestPath, const QString &error) {
    // Superseded by a newer request, or a response may already have been
    // handled while the reply was in flight.
    if (generation != m_pendingGeneration || m_pendingAction == Action::None)
        return;

    if (!error.isEmpty()) {
        clearPending();
        emit failed(tr("The file chooser failed: %1").arg(error));
        return;
    }

    // Old portal versions can hand back a different request path than the
    // handle_token predicts; move our subscription over if so.
    if (requestPath != m_pendingPath) {
        stopListening(m_pendingPath);
        m_pendingPath = requestPath;
        if (!listen(requestPath)) {
            clearPending();
            emit failed(tr("Could not listen for the file chooser's answer."));
        }
    }
}

void PortalFilePicker::handleResponse(uint response, const QVariantMap &results, const QDBusMessage &message) {
    responded(message.path(), response, results);
}

void PortalFilePicker::responded(const QString &requestPath, uint response, const QVariantMap &results) {
    // Only the pending request's answer counts; one for a request replaced
    // since (still subscribed for a moment) is dropped.
    if (m_pendingAction == Action::None || requestPath != m_pendingPath)
        return;
    const Action action = m_pendingAction;
    clearPending();
    // 1 is the user cancelling, which needs no word; 2 is the dialog ending
    // some other way.
    if (response == 1)
        return;
    if (response != 0) {
        emit failed(tr("The file chooser closed without a file."));
        return;
    }
    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    if (uris.isEmpty())
        return;
    const QUrl url(uris.first());
    if (action == Action::Open)
        emit openSelected(url);
    else if (action == Action::Save)
        emit saveSelected(url);
}

void PortalFilePicker::clearPending() {
    // A caller that never gets its reply must not outlive the request it
    // belonged to (repeated stuck requests would otherwise pile them up).
    if (m_pendingWatcher) {
        m_pendingWatcher->deleteLater();
        m_pendingWatcher = nullptr;
    }
    if (!m_pendingPath.isEmpty())
        stopListening(m_pendingPath);
    m_pendingPath.clear();
    m_pendingAction = Action::None;
}
