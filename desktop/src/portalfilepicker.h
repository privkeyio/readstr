// Adapted from Omacut's src/portalfilepicker.h
// (https://github.com/omacom-io/omacut, MIT, David Heinemeier Hansson).
#pragma once

#include <QVariantMap>

#include "filepicker.h"

class QDBusMessage;
class QDBusPendingCallWatcher;

// Open and save dialogs through xdg-desktop-portal's FileChooser over D-Bus,
// so they are the desktop's own dialogs rather than Qt's. Nothing here waits
// on the bus: calls are sent asynchronously, without introspection.
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void openOpml() override;
    void saveOpml(const QString &suggestedName) override;

protected:
    enum class Action { None, Open, Save };

    // The D-Bus side, which tests replace. The session bus's unique name
    // (":1.42"), or empty without a session bus.
    virtual QString senderName() const;
    virtual bool listen(const QString &requestPath);
    virtual void stopListening(const QString &requestPath);
    // Sends a FileChooser call; its reply goes to replied() with `generation`.
    virtual void send(const QDBusMessage &call, int generation);
    // Best-effort Request.Close, without waiting for a reply.
    virtual void closeRequest(const QString &requestPath);

    // The portal's reply to a FileChooser call: the request's path, or why
    // the call failed.
    void replied(int generation, const QString &requestPath, const QString &error);
    // A Response signal from the request object at `requestPath`.
    void responded(const QString &requestPath, uint response, const QVariantMap &results);

private slots:
    void handleResponse(uint response, const QVariantMap &results, const QDBusMessage &message);

private:
    void requestFile(const QString &method, const QString &title, QVariantMap options, Action action);
    void clearPending();

    QString m_pendingPath;
    Action m_pendingAction = Action::None;
    // Bumped per request, so a late reply for a replaced request is ignored.
    int m_pendingGeneration = 0;
    QDBusPendingCallWatcher *m_pendingWatcher = nullptr;
};
