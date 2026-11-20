// Adapted from Omacut's src/filepicker.h
// (https://github.com/omacom-io/omacut, MIT, David Heinemeier Hansson).
#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

// Open and save dialogs, behind an interface so tests can answer them without
// a desktop portal. A cancelled dialog emits nothing.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openOpml() = 0;
    virtual void saveOpml(const QString &suggestedName) = 0;

signals:
    void openSelected(const QUrl &url);
    void saveSelected(const QUrl &url);
    void failed(const QString &message);
};
