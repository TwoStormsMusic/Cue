#pragma once

// The app's line to a running Cue: the control socket of `cue run`
// (docs/control.md). It connects, and whenever the engine goes away (or is
// not there yet) it tries again every second, so the app can be started,
// closed and started again while the engine plays on.

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>

class QLocalSocket;

namespace cue::gui {

class EngineLink : public QObject {
    Q_OBJECT
public:
    explicit EngineLink(QString socketPath, QObject* parent = nullptr);
    ~EngineLink() override;

    // $XDG_RUNTIME_DIR/cue/engine.sock, or /tmp/cue-<uid>/engine.sock: where
    // `cue run` answers unless told otherwise.
    static QString defaultSocketPath();
    const QString& socketPath() const { return path_; }

    // Connects now, and again a second after every failure or disconnect.
    void start();
    virtual bool connected() const { return connected_; }

    // The reply comes later, with "ok" and the answer; a request that cannot
    // be sent, or is still waiting when the line drops, gets ok false.
    using Reply = std::function<void(const QJsonObject&)>;
    virtual void request(QJsonObject request, Reply reply = {});

    // The events wanted ("status", "log", "tuner"): subscribed now and on
    // every connect.
    void setEvents(const QStringList& events);
    const QStringList& events() const { return events_; }

signals:
    void connectionChanged(bool connected);
    // An event: its name and the whole message.
    void event(const QString& name, const QJsonObject& message);

private:
    void tryConnect();
    void onConnected();
    void onDisconnected();
    void onReadable();
    void failPending(const QString& why);

    QString path_;
    QLocalSocket* socket_ = nullptr;
    QTimer retry_;
    QByteArray in_;
    bool connected_ = false;
    int nextId_ = 1;
    QHash<int, Reply> pending_;
    QStringList events_, subscribed_;
};

} // namespace cue::gui
