#pragma once

#include <QObject>

// The facade QML talks to as `backend`. Phase 0 has no state yet; the store,
// fetcher, and sync hang off it from phase 1 on.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(int feedCount READ feedCount NOTIFY feedCountChanged)

public:
    explicit Backend(QObject *parent = nullptr);

    int feedCount() const { return 0; }

signals:
    void feedCountChanged();
};
