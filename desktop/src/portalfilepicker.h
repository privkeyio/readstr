// Adapted from Omacut's src/portalfilepicker.h
// (https://github.com/omacom-io/omacut, MIT, David Heinemeier Hansson).
#pragma once

#include <QVariantMap>

#include "filepicker.h"

class QDBusPendingCallWatcher;

// Open and save dialogs through xdg-desktop-portal's FileChooser over D-Bus,
// so they are the desktop's own dialogs rather than Qt's.
class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void openOpml() override;
    void saveOpml(const QString &suggestedName) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Action { None, Open, Save };

    void requestFile(const QString &method, const QString &title, QVariantMap options, Action action);
    bool connectToRequestPath(const QString &path);
    void clearPending();
    // Best-effort Close of the pending portal request, if there is one.
    void closePendingRequest();

    QString m_pendingPath;
    Action m_pendingAction = Action::None;
    // Bumped per request, so a late reply for a replaced request is ignored.
    int m_pendingGeneration = 0;
    QDBusPendingCallWatcher *m_pendingWatcher = nullptr;
};
