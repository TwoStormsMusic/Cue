#include "gui/link.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QtEndian>

#include <unistd.h>

namespace cue::gui {

namespace {

// As the engine: a message is at most 16 MB.
constexpr quint32 kMaxMessage = 16u << 20;

} // namespace

EngineLink::EngineLink(QString socketPath, QObject* parent)
    : QObject(parent), path_(socketPath.isEmpty() ? defaultSocketPath() : std::move(socketPath)) {
    socket_ = new QLocalSocket(this);
    retry_.setSingleShot(true);
    retry_.setInterval(1000);
    connect(&retry_, &QTimer::timeout, this, &EngineLink::tryConnect);
    connect(socket_, &QLocalSocket::connected, this, &EngineLink::onConnected);
    connect(socket_, &QLocalSocket::disconnected, this, &EngineLink::onDisconnected);
    connect(socket_, &QLocalSocket::readyRead, this, &EngineLink::onReadable);
    // A failed connect: no engine yet, or one that has just gone. Once
    // connected, a failure ends in disconnected().
    connect(socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (!connected_)
            retry_.start();
    });
}

EngineLink::~EngineLink() {
    // No signals into a half-destroyed owner.
    socket_->disconnect(this);
    socket_->abort();
}

QString EngineLink::defaultSocketPath() {
    const QByteArray run = qgetenv("XDG_RUNTIME_DIR");
    if (!run.isEmpty())
        return QString::fromLocal8Bit(run) + "/cue/engine.sock";
    return QString("/tmp/cue-%1/engine.sock").arg(getuid());
}

void EngineLink::start() { tryConnect(); }

void EngineLink::tryConnect() {
    if (connected_)
        return;
    socket_->abort();
    socket_->connectToServer(path_);
}

void EngineLink::onConnected() {
    connected_ = true;
    in_.clear();
    subscribed_.clear();
    emit connectionChanged(true);
    setEvents(events_);
}

void EngineLink::onDisconnected() {
    if (!connected_)
        return;
    connected_ = false;
    in_.clear();
    subscribed_.clear();
    failPending("the engine went away");
    emit connectionChanged(false);
    retry_.start();
}

void EngineLink::failPending(const QString& why) {
    const auto pending = std::exchange(pending_, {});
    for (const Reply& reply : pending)
        if (reply)
            reply(QJsonObject{{"ok", false}, {"error", why}});
}

void EngineLink::request(QJsonObject request, Reply reply) {
    if (!connected_) {
        if (reply)
            QTimer::singleShot(0, this, [reply] { reply(QJsonObject{{"ok", false}, {"error", "not connected to Cue"}}); });
        return;
    }
    const int id = nextId_++;
    request["id"] = id;
    if (reply)
        pending_.insert(id, std::move(reply));
    const QByteArray body = QJsonDocument(request).toJson(QJsonDocument::Compact);
    char head[4];
    qToBigEndian<quint32>(static_cast<quint32>(body.size()), head);
    socket_->write(head, 4);
    socket_->write(body);
    // Now, not when the event loop next comes round: a tap should be heard at once.
    socket_->flush();
}

void EngineLink::setEvents(const QStringList& events) {
    events_ = events;
    if (!connected_)
        return;
    QJsonArray add, drop;
    for (const QString& e : events_)
        if (!subscribed_.contains(e))
            add.append(e);
    for (const QString& e : subscribed_)
        if (!events_.contains(e))
            drop.append(e);
    if (!add.isEmpty())
        request({{"cmd", "subscribe"}, {"events", add}});
    if (!drop.isEmpty())
        request({{"cmd", "unsubscribe"}, {"events", drop}});
    subscribed_ = events_;
}

void EngineLink::onReadable() {
    in_ += socket_->readAll();
    qsizetype at = 0;
    while (in_.size() - at >= 4) {
        const quint32 n = qFromBigEndian<quint32>(in_.constData() + at);
        if (n > kMaxMessage) {
            socket_->abort();
            return;
        }
        if (in_.size() - at < 4 + static_cast<qsizetype>(n))
            break;
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(in_.constData() + at + 4, n));
        at += 4 + n;
        if (!doc.isObject())
            continue;
        const QJsonObject m = doc.object();
        if (m.contains("event")) {
            emit event(m.value("event").toString(), m);
        } else {
            const Reply reply = pending_.take(m.value("id").toInt());
            if (reply)
                reply(m);
        }
        // A handler may have dropped the line.
        if (!connected_)
            return;
    }
    in_.remove(0, at);
}

} // namespace cue::gui
