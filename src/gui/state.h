#pragma once

// What every view shares: the line to the engine, what it last said, the
// app's settings, and the things a tap can ask of the engine.

#include "gui/link.h"
#include "gui/model.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

namespace cue::gui {

// The app's own settings, in ~/.config/cue/app.ini: the machine's, not the
// performance's.
struct Settings {
    QString startView = "stage"; // stage, tuner or chains
    bool percent = false;        // times as % of the budget rather than ms
    double scale = 1.0;          // size of everything, 0.5 to 2.5
    bool tunerWhenMuted = true;  // the tuner view comes up while the inputs are muted
    bool fullScreen = false;

    static QString path();
    void load();
    void save() const;
};

class AppState : public QObject {
    Q_OBJECT
public:
    explicit AppState(EngineLink& link, QObject* parent = nullptr);

    EngineLink& link;
    Settings settings;
    // When the app started the engine (--start): where its messages go, and
    // how long that file was before.
    QString engineLog;
    qint64 engineLogFrom = 0;

    bool connected() const { return link.connected(); }
    QJsonObject hello;
    Performance perf;
    Status status;
    TunerReading tuner;
    double strobePhase = 0; // the tuner's strobe, turned by each reading
    struct LogLine {
        QString level, text;
        QDateTime at;
    };
    QVector<LogLine> log; // the newest last, at most 200

    // The chain playing, or -1 before the engine has said.
    int playing() const { return status.valid ? status.selected : -1; }
    const ChainInfo* chain(int index) const;
    // The chains of the scene playing, in order.
    QVector<int> sceneChains() const;

    // Asks the engine. Failures come back as a message.
    void select(int chain);
    void step(int delta); // the next or previous chain, as the FCB's up and down
    void run(const QString& action);
    void save();
    void reloadPerformance();
    // A reply's "ok", or a message saying why not.
    bool check(const QJsonObject& reply, const QString& what);

    // What was changed here, so it can be taken back (Ctrl+Z) and made again
    // (Ctrl+Shift+Z): a parameter, a bypass, the parameters picked to show.
    // Each is the request that undoes it and the one that does it again.
    // What a pedal or a plug-in's own editor changes is not in it. A new
    // engine or performance starts it afresh.
    struct Edit {
        QString what; // said when it is undone: "Delay: Output"
        int chain = -1, slot = -1;
        QJsonObject undo, redo;
    };
    void edited(const Edit& e);
    bool canUndo() const { return !undo_.isEmpty(); }
    bool canRedo() const { return !redo_.isEmpty(); }
    void undo();
    void redo();

signals:
    // The connection, the hello or the performance changed.
    void changed();
    void statusChanged();
    void tunerChanged();
    // For a moment at the bottom of the screen.
    void message(const QString& text, bool error);
    // An edit was undone or redone: the plug-in it was on.
    void undone(int chain, int slot);
    void undoChanged();

private:
    void onConnection(bool up);
    void onEvent(const QString& name, const QJsonObject& m);

    void takeBack(QVector<Edit>& from, QVector<Edit>& to, bool undoing);

    QElapsedTimer tunerClock_; // since the last reading
    QVector<Edit> undo_, redo_; // the newest last
};

} // namespace cue::gui
